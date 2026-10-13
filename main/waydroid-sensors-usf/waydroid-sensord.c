// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * waydroid-sensord for Google USF sensor hubs (Pixel 6 AoC).
 *
 * Waydroid starts `waydroid-sensord /dev/hwbinder` when the container starts
 * (and then leaves the in-container stub HAL off). This daemon registers
 * android.hardware.sensors@1.0::ISensors/default on the container's
 * hwbinder (libgbinder, like upstream waydroid-sensors) and serves the
 * sensors of the AoC sensor hub, which it talks to over USF directly:
 * accelerometer, gyroscope, magnetometer, light, proximity, pressure and the
 * AoC's own sensor fusion (rotation vectors, orientation, gravity, linear
 * acceleration). The fusion outputs are what compass and map apps use.
 *
 * USF (reverse engineered, see oriole-sensors/README.md): one FlatBuffer per
 * read()/write() on /dev/acd-com.google.usf, each open() is its own AoC
 * channel, so this runs next to oriole-sensorproxy. The registry must be
 * loaded already (oriole-usf.service); until the sensors show up this
 * retries every few seconds.
 *
 * A sensor samples only while Android has it activated and the display is
 * on. poll() is answered asynchronously: the request is blocked until
 * events arrive.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <glob.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <time.h>
#include <unistd.h>

#include <glib.h>
#include <glib-unix.h>
#include <gbinder.h>

#define HIDL_IFACE	"android.hardware.sensors@1.0::ISensors"
#define HIDL_NAME	"default"
#define USF_DEV		"/dev/acd-com.google.usf"
#define PIDFILE		"/run/waydroid-sensord.pid"

static bool verbose;
#define dbg(...) do { if (verbose) g_message(__VA_ARGS__); } while (0)

/* ---- android.hardware.sensors@1.0 types (64-bit HIDL layout) ---- */

enum {
	TX_GET_SENSORS_LIST = GBINDER_FIRST_CALL_TRANSACTION,
	TX_SET_OPERATION_MODE,
	TX_ACTIVATE,
	TX_POLL,
	TX_BATCH,
	TX_FLUSH,
	TX_INJECT_SENSOR_DATA,
	TX_REGISTER_DIRECT_CHANNEL,
	TX_UNREGISTER_DIRECT_CHANNEL,
	TX_CONFIG_DIRECT_REPORT,
};

enum {
	RESULT_OK = 0,
	RESULT_BAD_VALUE = -22,
	RESULT_INVALID_OPERATION = -38,
};

#define SENSOR_FLAG_ON_CHANGE_MODE	2u
#define SENSOR_TYPE_META_DATA		0
#define META_DATA_FLUSH_COMPLETE	1u
#define ACCURACY_HIGH			3

typedef struct {
	int32_t handle;
	GBinderHidlString name;
	GBinderHidlString vendor;
	int32_t version;
	int32_t type;
	GBinderHidlString typeAsString;
	float maxRange;
	float resolution;
	float power;
	int32_t minDelay;
	uint32_t fifoReservedEventCount;
	uint32_t fifoMaxEventCount;
	GBinderHidlString requiredPermission;
	int32_t maxDelay;
	uint32_t flags;
} __attribute__((aligned(8))) SensorInfo;
G_STATIC_ASSERT(sizeof(SensorInfo) == 112);

typedef struct {
	int64_t timestamp;
	int32_t sensorHandle;
	int32_t sensorType;
	union {
		float data[16];
		uint32_t meta_what;
		uint64_t stepCount;
		struct { float x, y, z; int8_t status; } vec3;
	} u;
} __attribute__((aligned(8))) Event;
G_STATIC_ASSERT(sizeof(Event) == 80);

/* ---- the sensors we export ---- */

enum { VEC3, SCALAR, DATA4, DATA5 };
enum { MODE_CONTINUOUS = 1, MODE_ON_CHANGE = 2 };	/* USF reporting modes */

typedef struct {
	const char *usf_name;		/* SENSOR_INFO name on the AoC */
	const char *name;		/* shown to Android */
	int32_t type;
	const char *type_str;
	int kind;
	bool on_change;			/* Android reporting mode */
	int usf_mode;			/* 0 = continuous */
	/* from SENSOR_INFO */
	int sid;
	float max_range, resolution;
	int64_t min_period, max_period;
	/* state */
	bool active;
	int64_t period;			/* requested by Android, ns */
	int handle;			/* USF sampling handle, -1 = stopped */
	int64_t running_period;
	int64_t last_ts;
	float last[5];
	bool have_last;
} Sensor;

static Sensor sensors[] = {
	{ "LSM6DSR Accelerometer", "LSM6DSR Accelerometer", 1,
	  "android.sensor.accelerometer", VEC3, false },
	{ "MMC56X3X Magnetometer", "MMC56X3X Magnetometer", 2,
	  "android.sensor.magnetic_field", VEC3, false },
	{ "Orientation Sensor", "Orientation Sensor", 3,
	  "android.sensor.orientation", VEC3, false },
	{ "LSM6DSR Gyroscope", "LSM6DSR Gyroscope", 4,
	  "android.sensor.gyroscope", VEC3, false },
	{ "TMD3719 Ambient Light", "TMD3719 Ambient Light", 5,
	  "android.sensor.light", SCALAR, true },
	{ "ICP10101 Pressure Sensor", "ICP10101 Pressure Sensor", 6,
	  "android.sensor.pressure", SCALAR, false },
	{ "TMD3719 Proximity", "TMD3719 Proximity", 8,
	  "android.sensor.proximity", SCALAR, true, MODE_ON_CHANGE },
	{ "Gravity Sensor", "Gravity Sensor", 9,
	  "android.sensor.gravity", VEC3, false },
	{ "Linear Acceleration Sensor", "Linear Acceleration Sensor", 10,
	  "android.sensor.linear_acceleration", VEC3, false },
	{ "Rotation Vector Sensor", "Rotation Vector Sensor", 11,
	  "android.sensor.rotation_vector", DATA5, false },
	{ "Game Rotation Vector Sensor", "Game Rotation Vector Sensor", 15,
	  "android.sensor.game_rotation_vector", DATA4, false },
	{ "Geomagnetic Rotation Vector Sensor",
	  "Geomagnetic Rotation Vector Sensor", 20,
	  "android.sensor.geomagnetic_rotation_vector", DATA5, false },
};
#define NSENSORS ((int)G_N_ELEMENTS(sensors))
/* Android handle = index + 1 */
#define HANDLE(s)	((int)((s) - sensors) + 1)

/* ---- FlatBuffers: same tiny builder/reader as oriole_usf.py ---- */

enum { K_I32, K_I64, K_BYTES };

typedef struct {
	int idx, kind;
	int64_t v;
	const void *data;
	uint32_t len;
} FbField;

/* fields sorted by idx; returns the size or 0 if it doesn't fit */
static size_t fb_table(uint8_t *buf, size_t cap, const FbField *f, int n)
{
	int nslots = n ? f[n - 1].idx + 1 : 0;
	size_t vt_size = 4 + 2 * nslots;
	size_t tbl_pos = 4 + vt_size;
	uint8_t inl[64];
	size_t ilen = 0, total, tbl_size;
	uint16_t slot[8] = { 0 };
	struct { size_t off; const void *d; uint32_t len; } vecs[4];
	int nv = 0;

	tbl_pos += (8 - tbl_pos % 8) % 8;
	for (int i = 0; i < n; i++) {
		size_t size = f[i].kind == K_I64 ? 8 : 4;

		while ((tbl_pos + 4 + ilen) % size)
			inl[ilen++] = 0;
		slot[f[i].idx] = 4 + ilen;
		if (f[i].kind == K_BYTES) {
			vecs[nv].off = slot[f[i].idx];
			vecs[nv].d = f[i].data;
			vecs[nv++].len = f[i].len;
			memset(inl + ilen, 0, 4);
		} else if (f[i].kind == K_I64) {
			memcpy(inl + ilen, &f[i].v, 8);
		} else {
			int32_t v = f[i].v;
			memcpy(inl + ilen, &v, 4);
		}
		ilen += size;
	}
	tbl_size = 4 + ilen;
	total = tbl_pos + tbl_size;
	for (int i = 0; i < nv; i++)
		total += 3 + 4 + vecs[i].len + 3;
	if (total > cap)
		return 0;

	memset(buf, 0, cap);
	uint32_t u32 = tbl_pos;
	memcpy(buf, &u32, 4);
	uint16_t vt[2] = { vt_size, tbl_size };
	memcpy(buf + 4, vt, 4);
	memcpy(buf + 8, slot, 2 * nslots);
	int32_t soff = tbl_pos - 4;
	memcpy(buf + tbl_pos, &soff, 4);
	memcpy(buf + tbl_pos + 4, inl, ilen);
	total = tbl_pos + tbl_size;
	for (int i = 0; i < nv; i++) {
		total += (4 - total % 4) % 4;
		u32 = total - (tbl_pos + vecs[i].off);
		memcpy(buf + tbl_pos + vecs[i].off, &u32, 4);
		memcpy(buf + total, &vecs[i].len, 4);
		memcpy(buf + total + 4, vecs[i].d, vecs[i].len);
		total += 4 + vecs[i].len;
	}
	total += (4 - total % 4) % 4;
	return total;
}

typedef struct {
	const uint8_t *b;
	size_t n;
	size_t p, vt;
	uint16_t vts;
	bool ok;
} Fb;

static uint32_t rd32(const uint8_t *p) { uint32_t v; memcpy(&v, p, 4); return v; }

static bool fb_init(Fb *t, const uint8_t *b, size_t n, ssize_t pos)
{
	t->b = b, t->n = n, t->ok = false;
	if (n < 8)
		return false;
	t->p = pos < 0 ? rd32(b) : (size_t)pos;
	if (t->p + 4 > n)
		return false;
	t->vt = t->p - (int32_t)rd32(b + t->p);
	if (t->vt + 4 > n)
		return false;
	memcpy(&t->vts, b + t->vt, 2);
	return t->ok = true;
}

static size_t fb_off(const Fb *t, int i)
{
	uint16_t o = 0;
	size_t vo = 4 + 2 * i;

	if (t->ok && vo < t->vts && t->vt + vo + 2 <= t->n)
		memcpy(&o, t->b + t->vt + vo, 2);
	return o && t->p + o + 4 <= t->n ? o : 0;
}

static int64_t fb_int(const Fb *t, int i, int64_t def)
{
	size_t o = fb_off(t, i);
	return o ? (int32_t)rd32(t->b + t->p + o) : def;
}

static int64_t fb_int64(const Fb *t, int i, int64_t def)
{
	size_t o = fb_off(t, i);
	int64_t v;

	if (!o || t->p + o + 8 > t->n)
		return def;
	memcpy(&v, t->b + t->p + o, 8);
	return v;
}

static float fb_float(const Fb *t, int i)
{
	size_t o = fb_off(t, i);
	float v = 0;
	if (o)
		memcpy(&v, t->b + t->p + o, 4);
	return v;
}

static const uint8_t *fb_bytes(const Fb *t, int i, uint32_t *len)
{
	size_t o = fb_off(t, i), vp;

	if (!o)
		return NULL;
	vp = t->p + o + rd32(t->b + t->p + o);
	if (vp + 4 > t->n || vp + 4 + rd32(t->b + vp) > t->n)
		return NULL;
	*len = rd32(t->b + vp);
	return t->b + vp + 4;
}

/* ---- USF ---- */

enum { MSG_REQ = 1, MSG_RESP = 2, MSG_FRAG = 3, MSG_SAMPLE = 9 };
enum {
	REQ_GET_SERVER = 3, REQ_SENSOR_INFO = 4, REQ_SENSOR_LIST = 5,
	REQ_START_SAMPLING = 8, REQ_STOP_SAMPLING = 9,
};
static const uint8_t SENSOR_MGR_UUID[16] = {
	0x6b, 0x31, 0xcb, 0xf0, 0x74, 0x4a, 0x14, 0xa1,
	0xd4, 0xe8, 0xae, 0x8a, 0x3e, 0x09, 0xfc, 0x4d,
};

static int usf_fd = -1;
static guint usf_watch, retry_timer;
static int32_t req_id;
static uint8_t *frag;
static size_t frag_len, frag_total;

static void handle_samples(const uint8_t *p, uint32_t n);

/* one complete message into buf (fragments reassembled); returns its
 * length, 0 for "nothing complete yet", -1 on error */
static ssize_t usf_read(uint8_t **msg)
{
	static uint8_t buf[1024];
	ssize_t n = read(usf_fd, buf, sizeof(buf));
	Fb m, f;
	uint32_t clen;
	const uint8_t *fp, *chunk;

	if (n < 0)
		return errno == EAGAIN || errno == EINTR ? 0 : -1;
	if (!fb_init(&m, buf, n, -1))
		return 0;
	if (fb_int(&m, 0, 0) != MSG_FRAG) {
		*msg = buf;
		return n;
	}
	if (!(fp = fb_bytes(&m, 1, &clen)) || !fb_init(&f, fp, clen, -1))
		return 0;
	uint32_t total = fb_int(&f, 1, 0), off = fb_int(&f, 2, 0);
	if (!(chunk = fb_bytes(&f, 3, &clen)) || total > 65536)
		return 0;
	if (off == 0) {
		g_free(frag);
		frag = g_malloc(total);
		frag_len = 0, frag_total = total;
	}
	if (!frag || off != frag_len || frag_len + clen > frag_total) {
		g_clear_pointer(&frag, g_free);	/* lost a piece: drop it */
		return 0;
	}
	memcpy(frag + frag_len, chunk, clen);
	frag_len += clen;
	if (frag_len < frag_total)
		return 0;
	static uint8_t *whole;
	g_free(whole);
	whole = frag, frag = NULL;
	*msg = whole;
	return frag_total;
}

/* sample events go to handle_samples(); returns false on read error */
static bool usf_dispatch(const uint8_t *d, size_t n, int32_t want_req,
			 int *status, uint8_t *resp, uint32_t *resp_len)
{
	Fb m, r;
	uint32_t len;
	const uint8_t *p;

	if (!fb_init(&m, d, n, -1))
		return false;
	int type = fb_int(&m, 0, 0);
	if (!(p = fb_bytes(&m, 1, &len)))
		return false;
	if (type == MSG_SAMPLE) {
		handle_samples(p, len);
		return false;
	}
	if (type != MSG_RESP || !resp || !fb_init(&r, p, len, -1) ||
	    fb_int(&r, 1, -1) != want_req)
		return false;
	*status = fb_int(&r, 2, 0);
	p = fb_bytes(&r, 3, &len);
	*resp_len = p && len <= 1024 ? len : 0;
	if (*resp_len)
		memcpy(resp, p, len);
	return true;
}

/* returns the response status, or -1 on timeout/error; resp is 1024 bytes */
static int usf_request(int type, int server, const uint8_t *payload,
		       size_t plen, uint8_t *resp, uint32_t *resp_len)
{
	uint8_t inner[512], outer[600];
	size_t n;
	int status = -1;

	req_id = (req_id + 1) & 0x7fffffff;
	FbField fi[] = {
		{ 0, K_I32, type }, { 1, K_I32, req_id }, { 2, K_I32, server },
		{ 3, K_BYTES, 0, payload, plen },
	};
	if (!(n = fb_table(inner, sizeof(inner), fi, payload ? 4 : 3)))
		return -1;
	FbField fo[] = { { 0, K_I32, MSG_REQ }, { 1, K_BYTES, 0, inner, n } };
	/* aoc_channel_dev counts its own channel header in the result */
	if (!(n = fb_table(outer, sizeof(outer), fo, 2)) ||
	    write(usf_fd, outer, n) < (ssize_t)n)
		return -1;

	int64_t end = g_get_monotonic_time() + 5 * G_USEC_PER_SEC;
	for (;;) {
		int left = (end - g_get_monotonic_time()) / 1000;
		struct pollfd pfd = { usf_fd, POLLIN, 0 };
		uint8_t *msg;
		ssize_t len;

		if (left <= 0 || poll(&pfd, 1, left) <= 0)
			return -1;
		if ((len = usf_read(&msg)) < 0)
			return -1;
		if (len && usf_dispatch(msg, len, req_id, &status, resp, resp_len))
			return status;
	}
}

static int usf_simple(int type, int server, const FbField *f, int nf,
		      Fb *out, uint8_t *resp)
{
	uint8_t pl[128];
	size_t n = 0;
	uint32_t rlen = 0;
	int st;

	if (f && !(n = fb_table(pl, sizeof(pl), f, nf)))
		return -1;
	st = usf_request(type, server, f ? pl : NULL, n, resp, &rlen);
	if (st == 0 && out && !(rlen && fb_init(out, resp, rlen, -1)))
		return -1;
	return st;
}

static int usf_start(Sensor *s, int64_t period)
{
	static uint8_t resp[1024];
	Fb r;
	FbField f[] = {
		{ 0, K_I32, s->usf_mode ? s->usf_mode : MODE_CONTINUOUS },
		{ 1, K_I64, period }, { 2, K_I64, 0 },
	};
	int st = usf_simple(REQ_START_SAMPLING, s->sid, f, 3, &r, resp);

	if (st) {
		g_warning("%s: start sampling failed (%d)", s->name, st);
		return -1;
	}
	return fb_int(&r, 0, -1);
}

static void usf_stop(Sensor *s)
{
	FbField f[] = { { 0, K_I32, s->handle } };
	usf_simple(REQ_STOP_SAMPLING, s->sid, f, 1, NULL, (uint8_t[1024]){ 0 });
}

/* ---- events ---- */

#define QMAX 512
static Event queue[QMAX];
static int q_head, q_len;
static GBinderRemoteRequest *pending_poll;
static GBinderLocalObject *hal_obj;
static bool screen_on = true;
static int64_t ts_off, ts_win_min, ts_win_start;

static int64_t boottime_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_BOOTTIME, &ts);
	return ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void complete_poll(void);

static void push_event(const Event *e)
{
	if (q_len == QMAX) {		/* Android isn't polling: drop the oldest */
		q_head = (q_head + 1) % QMAX;
		q_len--;
	}
	queue[(q_head + q_len++) % QMAX] = *e;
}

/* The AoC clock runs at a fixed offset from CLOCK_BOOTTIME. Estimate it as
 * the smallest (arrival - sample time) seen, re-estimated every 10 s so a
 * jump (suspend) is followed. */
static int64_t to_boottime(uint64_t ts, int64_t now)
{
	int64_t est = now - (int64_t)ts;

	if (!ts_win_start || est < ts_off)
		ts_off = est;
	if (!ts_win_start || est < ts_win_min)
		ts_win_min = est;
	if (now - ts_win_start > 10000000000LL) {
		if (ts_win_start)
			ts_off = ts_win_min;
		ts_win_start = now, ts_win_min = est;
	}
	return (int64_t)ts + ts_off;
}

static void handle_samples(const uint8_t *p, uint32_t n)
{
	int64_t now = boottime_ns();
	Sensor *s = NULL;
	uint32_t handle;
	uint8_t count, size;
	size_t pos = 16;
	bool queued = false;

	if (n < 16)
		return;
	handle = rd32(p + 8);
	count = p[14], size = p[15];
	for (int i = 0; i < NSENSORS; i++)
		if (sensors[i].handle >= 0 && (uint32_t)sensors[i].handle == handle)
			s = &sensors[i];
	if (!s)
		return;
	for (int i = 0; i < count && pos + 8 + size <= n; i++, pos += 8 + size) {
		float v[5] = { 0 };
		uint64_t ts;
		int nv = MIN(size / 4, 5);
		Event e = { 0 };

		memcpy(&ts, p + pos, 8);
		memcpy(v, p + pos + 8, nv * 4);
		if (s->on_change && s->have_last && !memcmp(v, s->last, sizeof(v)))
			continue;
		memcpy(s->last, v, sizeof(v));
		s->have_last = true;

		e.timestamp = MIN(to_boottime(ts, now), now);
		if (e.timestamp <= s->last_ts)
			e.timestamp = s->last_ts + 1;
		s->last_ts = e.timestamp;
		e.sensorHandle = HANDLE(s);
		e.sensorType = s->type;
		switch (s->kind) {
		case VEC3:
			e.u.vec3.x = v[0], e.u.vec3.y = v[1], e.u.vec3.z = v[2];
			e.u.vec3.status = ACCURACY_HIGH;
			break;
		case SCALAR:
			e.u.data[0] = v[0];
			break;
		case DATA4:
			memcpy(e.u.data, v, 4 * sizeof(float));
			break;
		case DATA5:
			memcpy(e.u.data, v, 5 * sizeof(float));
			if (nv < 5)
				e.u.data[4] = -1;	/* heading accuracy unknown */
			break;
		}
		push_event(&e);
		queued = true;
	}
	if (queued)
		complete_poll();
}

/* ---- sampling control ---- */

static void usf_failed(void);

static void apply(Sensor *s)
{
	bool want = s->active && screen_on && usf_fd >= 0 && s->sid >= 0;
	int64_t period = CLAMP(s->period, s->min_period, s->max_period);

	if (want && s->handle >= 0 && period == s->running_period)
		return;
	if (s->handle >= 0) {
		usf_stop(s);
		dbg("%s: stopped", s->name);
		s->handle = -1;
	}
	if (!want)
		return;
	s->have_last = false;
	s->handle = usf_start(s, period);
	if (s->handle < 0) {
		usf_failed();
		return;
	}
	s->running_period = period;
	dbg("%s: started, %lld ms (handle %d)", s->name,
	    (long long)(period / 1000000), s->handle);
}

static void apply_all(void)
{
	for (int i = 0; i < NSENSORS && usf_fd >= 0; i++)
		apply(&sensors[i]);
}

static gboolean on_usf(gint fd, GIOCondition cond, gpointer data)
{
	uint8_t *msg;
	ssize_t n = usf_read(&msg);

	if (n < 0 || (cond & (G_IO_ERR | G_IO_HUP))) {
		usf_watch = 0;
		usf_failed();
		return G_SOURCE_REMOVE;
	}
	if (n)
		usf_dispatch(msg, n, -1, NULL, NULL, NULL);
	return G_SOURCE_CONTINUE;
}

static bool usf_connect(void)
{
	static uint8_t resp[1024];
	Fb r;
	const uint32_t *ids;
	uint32_t len;
	int mgr, found = 0;

	usf_fd = open(USF_DEV, O_RDWR | O_CLOEXEC);
	if (usf_fd < 0)
		return false;
	req_id = g_random_int() & 0xffff;
	FbField f[] = { { 0, K_BYTES, 0, SENSOR_MGR_UUID, 16 } };
	if (usf_simple(REQ_GET_SERVER, 1, f, 1, &r, resp) ||
	    (mgr = fb_int(&r, 0, -1)) < 0)
		goto fail;
	if (usf_simple(REQ_SENSOR_LIST, mgr, NULL, 0, &r, resp) ||
	    !(ids = (const uint32_t *)fb_bytes(&r, 0, &len)))
		goto fail;
	/* a vector of u32: fb_bytes() returned the element count as len */
	uint32_t nids = MIN(len, 128), idv[128];
	if ((const uint8_t *)ids + 4 * nids > r.b + r.n)
		goto fail;
	memcpy(idv, ids, nids * 4);
	for (int i = 0; i < NSENSORS; i++)
		sensors[i].sid = -1;
	for (uint32_t k = 0; k < nids; k++) {
		const uint8_t *name;
		uint32_t nlen;

		if (usf_simple(REQ_SENSOR_INFO, idv[k], NULL, 0, &r, resp) ||
		    !(name = fb_bytes(&r, 0, &nlen)))
			continue;
		for (int i = 0; i < NSENSORS; i++) {
			Sensor *s = &sensors[i];

			if (strlen(s->usf_name) != strnlen((const char *)name, nlen) ||
			    strncmp(s->usf_name, (const char *)name, nlen))
				continue;
			s->sid = idv[k];
			s->max_range = fb_float(&r, 2);
			s->min_period = fb_int64(&r, 3, 10000000);
			s->max_period = fb_int64(&r, 6, 200000000);
			s->resolution = fb_float(&r, 9);
			if (s->max_period < s->min_period)
				s->max_period = s->min_period;
			found++;
		}
	}
	if (!found)
		goto fail;
	g_message("USF: %d of %d sensors found", found, NSENSORS);
	fcntl(usf_fd, F_SETFL, O_NONBLOCK);
	usf_watch = g_unix_fd_add(usf_fd, G_IO_IN | G_IO_ERR | G_IO_HUP, on_usf, NULL);
	return true;
fail:
	close(usf_fd);
	usf_fd = -1;
	return false;
}

static gboolean retry_usf(gpointer data)
{
	if (!usf_connect())
		return G_SOURCE_CONTINUE;
	retry_timer = 0;
	apply_all();
	return G_SOURCE_REMOVE;
}

static void usf_failed(void)
{
	g_warning("USF error, reconnecting");
	if (usf_watch)
		g_source_remove(usf_watch);
	usf_watch = 0;
	if (usf_fd >= 0)
		close(usf_fd);
	usf_fd = -1;
	for (int i = 0; i < NSENSORS; i++)
		sensors[i].handle = -1;
	if (!retry_timer)
		retry_timer = g_timeout_add_seconds(3, retry_usf, NULL);
}

/* ---- display ---- */

static bool read_screen_on(void)
{
	glob_t g;
	bool on = true;

	if (glob("/sys/class/drm/card*-DSI-*/dpms", 0, NULL, &g) == 0) {
		char buf[16] = "";
		FILE *f = fopen(g.gl_pathv[0], "r");

		if (f) {
			on = fgets(buf, sizeof(buf), f) && !strncmp(buf, "On", 2);
			fclose(f);
		}
		globfree(&g);
	}
	return on;
}

static gboolean poll_screen(gpointer data)
{
	bool on = read_screen_on();

	if (on != screen_on) {
		screen_on = on;
		dbg("display %s", on ? "on" : "off");
		apply_all();
	}
	return G_SOURCE_CONTINUE;
}

/* ---- ISensors ---- */

static void set_hidl_string(GBinderHidlString *h, const char *s)
{
	h->data.str = s;
	h->len = strlen(s);
	h->owns_buffer = TRUE;
}

static void write_string(GBinderWriter *w, const GBinderHidlString *h,
			 guint index, gsize off)
{
	GBinderParent parent = { index, off };
	gbinder_writer_append_buffer_object_with_parent(w, h->data.str,
							 h->len + 1, &parent);
}

static void reply_sensor_list(GBinderLocalReply *reply)
{
	GBinderWriter w;
	GBinderParent vparent;
	GBinderHidlVec *vec;
	SensorInfo *info;
	int n = 0;
	guint index;

	gbinder_local_reply_init_writer(reply, &w);
	vec = gbinder_writer_new0(&w, GBinderHidlVec);
	info = gbinder_writer_malloc0(&w, NSENSORS * sizeof(*info));
	for (int i = 0; i < NSENSORS; i++) {
		Sensor *s = &sensors[i];
		SensorInfo *si = &info[n];

		if (s->sid < 0)
			continue;
		si->handle = HANDLE(s);
		set_hidl_string(&si->name, s->name);
		set_hidl_string(&si->vendor, "Google (AoC USF)");
		si->version = 1;
		si->type = s->type;
		set_hidl_string(&si->typeAsString, s->type_str);
		si->maxRange = s->max_range;
		si->resolution = s->resolution;
		si->power = 0.5f;
		si->minDelay = s->on_change ? 0 : s->min_period / 1000;
		set_hidl_string(&si->requiredPermission, "");
		si->maxDelay = s->max_period / 1000;
		si->flags = s->on_change ? SENSOR_FLAG_ON_CHANGE_MODE : 0;
		n++;
	}
	vec->data.ptr = info;
	vec->count = n;
	vec->owns_buffer = TRUE;
	vparent.index = gbinder_writer_append_buffer_object(&w, vec, sizeof(*vec));
	vparent.offset = GBINDER_HIDL_VEC_BUFFER_OFFSET;
	index = gbinder_writer_append_buffer_object_with_parent(&w, info,
						n * sizeof(*info), &vparent);
	for (int i = 0; i < n; i++) {
		gsize off = i * sizeof(*info);

		write_string(&w, &info[i].name, index,
			     off + G_STRUCT_OFFSET(SensorInfo, name));
		write_string(&w, &info[i].vendor, index,
			     off + G_STRUCT_OFFSET(SensorInfo, vendor));
		write_string(&w, &info[i].typeAsString, index,
			     off + G_STRUCT_OFFSET(SensorInfo, typeAsString));
		write_string(&w, &info[i].requiredPermission, index,
			     off + G_STRUCT_OFFSET(SensorInfo, requiredPermission));
	}
}

static int max_poll_count = 128;

/* answer the blocked poll() with what is queued */
static void complete_poll(void)
{
	GBinderLocalReply *reply;
	GBinderWriter w;
	Event *ev;
	int n;

	if (!pending_poll || !q_len)
		return;
	n = MIN(q_len, max_poll_count);
	reply = gbinder_local_object_new_reply(hal_obj);
	gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);
	gbinder_local_reply_init_writer(reply, &w);
	gbinder_writer_append_int32(&w, RESULT_OK);
	ev = gbinder_writer_malloc(&w, n * sizeof(Event));
	for (int i = 0; i < n; i++)
		ev[i] = queue[(q_head + i) % QMAX];
	q_head = (q_head + n) % QMAX;
	q_len -= n;
	gbinder_writer_append_hidl_vec(&w, ev, n, sizeof(Event));
	gbinder_writer_append_hidl_vec(&w, NULL, 0, sizeof(SensorInfo));
	gbinder_remote_request_complete(pending_poll, reply, GBINDER_STATUS_OK);
	gbinder_local_reply_unref(reply);
	gbinder_remote_request_unref(pending_poll);
	pending_poll = NULL;
}

static Sensor *by_handle(int h)
{
	return h >= 1 && h <= NSENSORS && sensors[h - 1].sid >= 0 ?
		&sensors[h - 1] : NULL;
}

static GBinderLocalReply *hal_transact(GBinderLocalObject *obj,
	GBinderRemoteRequest *req, guint code, guint flags, int *status,
	void *user_data)
{
	GBinderReader r;
	GBinderLocalReply *reply;
	GBinderWriter w;
	gint32 h = 0, i32 = 0;
	gint64 period = 0, latency = 0;
	gboolean en = FALSE;
	Sensor *s;
	int ret = RESULT_OK;

	if (g_strcmp0(gbinder_remote_request_interface(req), HIDL_IFACE)) {
		*status = GBINDER_STATUS_FAILED;
		return NULL;
	}
	gbinder_remote_request_init_reader(req, &r);
	*status = GBINDER_STATUS_OK;
	reply = gbinder_local_object_new_reply(obj);
	gbinder_local_reply_append_int32(reply, GBINDER_STATUS_OK);

	switch (code) {
	case TX_GET_SENSORS_LIST:
		reply_sensor_list(reply);
		return reply;
	case TX_POLL:
		gbinder_reader_read_int32(&r, &i32);
		if (i32 <= 0) {
			/* SensorService's "is the HAL up" probe: answer now */
			gbinder_local_reply_init_writer(reply, &w);
			gbinder_writer_append_int32(&w, RESULT_BAD_VALUE);
			gbinder_writer_append_hidl_vec(&w, NULL, 0, sizeof(Event));
			gbinder_writer_append_hidl_vec(&w, NULL, 0, sizeof(SensorInfo));
			return reply;
		}
		max_poll_count = MIN(i32, 128);
		if (pending_poll) {	/* shouldn't happen: one poll thread */
			gbinder_remote_request_unref(pending_poll);
			pending_poll = NULL;
		}
		pending_poll = gbinder_remote_request_ref(req);
		gbinder_remote_request_block(req);
		gbinder_local_reply_unref(reply);
		complete_poll();
		return NULL;
	case TX_ACTIVATE:
		gbinder_reader_read_int32(&r, &h);
		gbinder_reader_read_bool(&r, &en);
		if ((s = by_handle(h))) {
			dbg("activate %s %d", s->name, en);
			s->active = en;
			if (!s->period)
				s->period = s->max_period;
			apply(s);
		} else {
			ret = RESULT_BAD_VALUE;
		}
		break;
	case TX_BATCH:
		gbinder_reader_read_int32(&r, &h);
		gbinder_reader_read_int32(&r, &i32);
		gbinder_reader_read_int64(&r, &period);
		gbinder_reader_read_int64(&r, &latency);
		if ((s = by_handle(h))) {
			dbg("batch %s %lld ms", s->name, (long long)(period / 1000000));
			s->period = period;
			apply(s);
		} else {
			ret = RESULT_BAD_VALUE;
		}
		break;
	case TX_FLUSH:
		gbinder_reader_read_int32(&r, &h);
		if ((s = by_handle(h)) && s->active) {
			Event e = { 0 };

			e.sensorHandle = h;
			e.sensorType = SENSOR_TYPE_META_DATA;
			e.u.meta_what = META_DATA_FLUSH_COMPLETE;
			push_event(&e);
			complete_poll();
		} else {
			ret = RESULT_BAD_VALUE;
		}
		break;
	case TX_SET_OPERATION_MODE:
		gbinder_reader_read_int32(&r, &i32);
		ret = i32 == 0 ? RESULT_OK : RESULT_INVALID_OPERATION;
		break;
	case TX_UNREGISTER_DIRECT_CHANNEL:
		break;
	case TX_REGISTER_DIRECT_CHANNEL:
	case TX_CONFIG_DIRECT_REPORT:
		gbinder_local_reply_init_writer(reply, &w);
		gbinder_writer_append_int32(&w, RESULT_INVALID_OPERATION);
		gbinder_writer_append_int32(&w, -1);
		return reply;
	default:		/* INJECT_SENSOR_DATA */
		ret = RESULT_INVALID_OPERATION;
		break;
	}
	gbinder_local_reply_init_writer(reply, &w);
	gbinder_writer_append_int32(&w, ret);
	return reply;
}

/* ---- service registration ---- */

static GMainLoop *loop;
static GBinderServiceManager *sm;

static void added(GBinderServiceManager *m, int status, void *data)
{
	if (status == GBINDER_STATUS_OK) {
		g_message("registered %s/%s", HIDL_IFACE, HIDL_NAME);
	} else {
		g_warning("failed to register %s (%d)", HIDL_IFACE, status);
		g_main_loop_quit(loop);
	}
}

static void sm_presence(GBinderServiceManager *m, void *data)
{
	if (gbinder_servicemanager_is_present(sm)) {
		g_message("service manager is back");
		gbinder_servicemanager_add_service(sm, HIDL_NAME, hal_obj, added, NULL);
		return;
	}
	g_message("service manager went away, stopping sensors");
	for (int i = 0; i < NSENSORS; i++) {
		sensors[i].active = false;
		apply(&sensors[i]);
	}
	if (pending_poll) {
		gbinder_remote_request_unref(pending_poll);
		pending_poll = NULL;
	}
	q_len = 0;
}

static gboolean on_signal(gpointer data)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

/* Waydroid stops the old instance with `kill -9 $(pidof ...)`, which can
 * miss; make sure only one instance serves the container. */
static void take_pidfile(void)
{
	int fd = open(PIDFILE, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
	char buf[32];

	if (fd < 0)
		return;
	if (flock(fd, LOCK_EX | LOCK_NB) < 0) {
		ssize_t n = read(fd, buf, sizeof(buf) - 1);
		pid_t old = n > 0 ? (buf[n] = 0, atoi(buf)) : 0;

		if (old > 1) {
			g_message("stopping the old instance (%d)", old);
			kill(old, SIGTERM);
		}
		if (flock(fd, LOCK_EX) < 0)
			return;
	}
	if (ftruncate(fd, 0) == 0) {
		int n = snprintf(buf, sizeof(buf), "%d\n", getpid());
		if (pwrite(fd, buf, n, 0) != n)
			g_warning("can't write %s", PIDFILE);
	}
	/* keep fd open: the lock lives as long as we do */
}

int main(int argc, char **argv)
{
	const char *dev = "/dev/hwbinder";

	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "-v"))
			verbose = true;
		else
			dev = argv[i];
	}
	if (getenv("WAYDROID_SENSORD_DEBUG"))
		verbose = true;
	take_pidfile();
	for (int i = 0; i < NSENSORS; i++)
		sensors[i].sid = sensors[i].handle = -1;

	sm = gbinder_servicemanager_new2(dev, "hidl", "hidl");
	if (!sm) {
		g_warning("can't open %s", dev);
		return 1;
	}
	if (!usf_connect()) {
		g_message("USF sensors not ready yet, retrying");
		retry_timer = g_timeout_add_seconds(3, retry_usf, NULL);
		/* register anyway once the sensors are there: Android reads
		 * the sensor list only once */
		while (usf_fd < 0)
			g_main_context_iteration(NULL, TRUE);
	}
	screen_on = read_screen_on();
	g_timeout_add_seconds(1, poll_screen, NULL);

	loop = g_main_loop_new(NULL, FALSE);
	g_unix_signal_add(SIGTERM, on_signal, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	if (!gbinder_servicemanager_wait(sm, -1)) {
		g_warning("no service manager on %s", dev);
		return 1;
	}
	hal_obj = gbinder_servicemanager_new_local_object(sm, HIDL_IFACE,
							  hal_transact, NULL);
	gulong pid = gbinder_servicemanager_add_presence_handler(sm, sm_presence, NULL);
	gbinder_servicemanager_add_service(sm, HIDL_NAME, hal_obj, added, NULL);
	g_main_loop_run(loop);

	gbinder_servicemanager_remove_handler(sm, pid);
	for (int i = 0; i < NSENSORS; i++) {
		sensors[i].active = false;
		apply(&sensors[i]);
	}
	gbinder_local_object_unref(hal_obj);
	gbinder_servicemanager_unref(sm);
	return 0;
}
