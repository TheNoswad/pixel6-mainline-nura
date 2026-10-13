// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * bcm4776-gnssd: GNSS for the Broadcom BCM4776 (Pixel 6) without Broadcom
 * userspace. Only the chip firmware is used (SensorHub.patch and the ESW
 * "GLL" patch, cut from the stock image); the chip computes the fixes
 * itself (on-chip micro-nav), this daemon only drives the session and turns
 * the position reports into NMEA.
 *
 * NMEA (GGA + RMC + GST + GSV + GSA, 1 Hz) is served on a unix stream socket, by default
 * gnss-share's /run/gnss-share.sock, which geoclue reads with
 * [network-nmea] nmea-socket=. The chip is powered only while at least one
 * client is connected (plus a short linger), and the socket can be passed
 * in by systemd (socket activation); otherwise it is created 0660, group
 * geoclue.
 *
 * Layers, chip side (bbd driver: /dev/ttyBCM, /dev/bbd_control):
 *   SSI frames   b0 00 | seq | len | flags | fields | payload | crc | b0 01,
 *                b0 escaped, CRC-8 poly 0x4d over len..payload, nibble swap
 *   TLVs         id u8 | len u8 (bit 7: 15-bit) | value; 05 memory write,
 *                1e host->chip / 1f chip->host MeIf stream pieces
 *   MeIf         c0 c1 | id u16 | len u16 | csum u16 | data | seq u16 | c0 c4,
 *                c0 escaped as c0 c0
 * Boot: autobaud, sync, SensorHub.patch writes, start, resync. Then the ESW
 * patch goes through the chip ROM loader (MeIf 0x8002 passthrough; skipped
 * if the chip still holds it), then the on-chip session is started and a
 * batch flush is requested every second; each flush returns one 0x8101
 * COMPACTMEAS message carrying a POSITION blob.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <grp.h>
#include <math.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define FW_DIR		"/lib/firmware/brcm/bcm4776/"
#define TTY		"/dev/ttyBCM"
#define CTL		"/dev/bbd_control"
#define NSTANDBY	"/sys/bus/spi/devices/spi10.0/nstandby"
#define SOCKET		"/run/gnss-share.sock"
#define SOCKET_GROUP	"geoclue"

#define MAX_CLIENTS	16
#define MAX_LISTEN	4
#define LINGER_MS	10000	/* keep the chip on after the last client left */
#define WATCHDOG_MS	15000	/* restart if the running chip goes quiet */
#define GPS_EPOCH	315964800	/* 1980-01-06 */
#define GPS_LEAP	18		/* GPS - UTC */

static const char *opt_patch = FW_DIR "SensorHub.patch";
static const char *opt_gll = FW_DIR "gll-patch-4776_A0.bin";
static const char *opt_socket = SOCKET;
static const char *opt_nstandby = NSTANDBY;
static int verbose;
static FILE *trace;	/* -t: raw chip I/O, JSONL like the lab tools */
static volatile sig_atomic_t quit;

static void logm(const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vfprintf(stderr, fmt, ap);
	va_end(ap);
	fputc('\n', stderr);
}

#define dbg(...) do { if (verbose) logm(__VA_ARGS__); } while (0)

static void trace_io(const char *dir, const uint8_t *p, size_t n)
{
	struct timespec ts;

	if (!trace)
		return;
	clock_gettime(CLOCK_REALTIME, &ts);
	fprintf(trace, "{\"t\": %lld.%06ld, \"dir\": \"%s\", \"hex\": \"",
		(long long)ts.tv_sec, ts.tv_nsec / 1000, dir);
	while (n--)
		fprintf(trace, "%02x", *p++);
	fputs("\"}\n", trace);
}

static int64_t now_ms(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000LL + ts.tv_nsec / 1000000;
}

static void sleep_ms(int ms)
{
	struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };

	nanosleep(&ts, NULL);
}

static void put16(uint8_t *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put32(uint8_t *p, uint32_t v) { put16(p, v); put16(p + 2, v >> 16); }
static unsigned get16(const uint8_t *p) { return p[0] | p[1] << 8; }
static uint32_t get32(const uint8_t *p) { return get16(p) | (uint32_t)get16(p + 2) << 16; }

static int write_file(const char *path, const char *s, size_t n)
{
	int fd = open(path, O_WRONLY | O_CLOEXEC);
	int ret = -1;

	if (fd >= 0) {
		ret = write(fd, s, n) == (ssize_t)n ? 0 : -1;
		close(fd);
	}
	if (ret)
		logm("%s: %s", path, strerror(errno));
	return ret;
}

static int nstandby(int on)
{
	return write_file(opt_nstandby, on ? "1" : "0", 1);
}

/* bbd_control commands are NUL-terminated strings */
static int ctl(const char *cmd)
{
	return write_file(CTL, cmd, strlen(cmd) + 1);
}

/* ------------------------------------------------------------------ */
/* NMEA clients */

static int listen_fds[MAX_LISTEN];
static int nlisten;
static bool socket_activated;
static int clients[MAX_CLIENTS];
static int nclients;
static int64_t last_client_ms;

static void client_drop(int i)
{
	close(clients[i]);
	clients[i] = clients[--nclients];
	if (!nclients)
		last_client_ms = now_ms();
	logm("nmea client left (%d)", nclients);
}

static void client_accept(int listen_fd)
{
	int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

	if (fd < 0)
		return;
	if (nclients == MAX_CLIENTS) {
		close(fd);
		return;
	}
	clients[nclients++] = fd;
	logm("nmea client connected (%d)", nclients);
}

static void client_read(int i)
{
	char buf[256];
	ssize_t n = read(clients[i], buf, sizeof(buf));

	if (n == 0 || (n < 0 && errno != EAGAIN && errno != EINTR))
		client_drop(i);
}

static void nmea_send(const char *s, size_t n)
{
	for (int i = nclients - 1; i >= 0; i--) {
		ssize_t w = send(clients[i], s, n, MSG_NOSIGNAL | MSG_DONTWAIT);

		/* a client that can't keep up loses lines, a dead one goes */
		if (w < 0 && errno != EAGAIN && errno != EINTR)
			client_drop(i);
	}
}

static int listen_socket(void)
{
	const char *e = getenv("LISTEN_FDS"), *p = getenv("LISTEN_PID");
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	struct group *gr;
	int fd;

	/* systemd may pass several sockets (e.g. bcm4776-gnssd-waydroid.socket),
	 * all serve the same stream */
	if (e && p && atoi(e) >= 1 && atoi(p) == getpid()) {
		nlisten = atoi(e) < MAX_LISTEN ? atoi(e) : MAX_LISTEN;
		for (int i = 0; i < nlisten; i++) {
			fd = 3 + i;	/* SD_LISTEN_FDS_START */
			fcntl(fd, F_SETFD, FD_CLOEXEC);
			fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
			listen_fds[i] = fd;
		}
		socket_activated = true;
		return 0;
	}
	if (strlen(opt_socket) >= sizeof(sa.sun_path))
		return -1;
	strcpy(sa.sun_path, opt_socket);
	fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	unlink(opt_socket);
	if (fd < 0 || bind(fd, (struct sockaddr *)&sa, sizeof(sa)) || listen(fd, 8)) {
		logm("%s: %s", opt_socket, strerror(errno));
		return -1;
	}
	/* like gnss-share: readable by geoclue, which does the access control */
	gr = getgrnam(SOCKET_GROUP);
	if (gr && chown(opt_socket, -1, gr->gr_gid))
		logm("%s: chown: %s", opt_socket, strerror(errno));
	chmod(opt_socket, gr ? 0660 : 0600);
	listen_fds[nlisten++] = fd;
	return 0;
}

/* ------------------------------------------------------------------ */
/* SSI link */

static const uint8_t *crc_table(void)
{
	static uint8_t t[256];

	if (!t[1])
		for (int i = 0; i < 256; i++) {
			uint8_t c = i;

			for (int b = 0; b < 8; b++)
				c = c & 0x80 ? (c << 1) ^ 0x4d : c << 1;
			t[i] = c;
		}
	return t;
}

static uint8_t crc8(const uint8_t *p, size_t n)
{
	const uint8_t *t = crc_table();
	uint8_t c = 0;

	while (n--)
		c = t[c ^ *p++];
	return c >> 4 | c << 4;
}

/* chip -> host message handling state */
static struct {
	int fd;
	uint8_t seq, rel;
	bool acked[256];	/* reliable counters acked by the chip */
	bool tlv_seen[256];	/* TLV ids in chip payloads */
	bool sync_seen;
	/* frame reassembly */
	uint8_t frame[8192];
	size_t flen;
	int fstate;
	/* MeIf stream reassembly */
	uint8_t msg[16384];
	size_t mlen;
	int mstate;		/* 0 outside, 1 after c0, 2 inside, 3 inside after c0 */
	uint32_t c2h_bytes;
} L = { .fd = -1 };

static void on_meif(const uint8_t *m, size_t n);

static void meif_byte(uint8_t c)
{
	switch (L.mstate) {
	case 0:
		if (c == 0xc0)
			L.mstate = 1;
		return;
	case 1:
		L.mstate = c == 0xc1 ? 2 : c == 0xc0 ? 1 : 0;
		L.mlen = 0;
		return;
	case 2:
		if (c == 0xc0) {
			L.mstate = 3;
			return;
		}
		break;
	case 3:
		if (c == 0xc4) {
			on_meif(L.msg, L.mlen);
			L.mstate = 0;
			return;
		}
		if (c == 0xc1) {	/* unterminated message: restart */
			L.mstate = 2;
			L.mlen = 0;
			return;
		}
		L.mstate = 2;
		if (c != 0xc0) {
			L.mstate = 0;
			return;
		}
		break;
	}
	if (L.mlen < sizeof(L.msg))
		L.msg[L.mlen++] = c;
	else
		L.mstate = 0;
}

static void on_payload(const uint8_t *p, size_t n)
{
	if (n && p[0] == 0x01)
		L.sync_seen = true;
	for (size_t i = 0; i + 2 <= n;) {
		unsigned id = p[i], len = p[i + 1];

		i += 2;
		if (len & 0x80) {
			if (i >= n)
				return;
			len = (len & 0x7f) << 8 | p[i++];
		}
		if (i + len > n)
			return;
		L.tlv_seen[id] = true;
		if (id == 0x1f && len >= 2)
			for (size_t j = 2; j < len; j++)
				meif_byte(p[i + j]);
		i += len;
	}
}

static void on_frame(const uint8_t *f, size_t n)
{
	static const unsigned bits[] = { 0x01, 0x02, 0x04, 0x08, 0x10, 0x20,
					 0x40, 0x100, 0x200 };
	unsigned fld[0x201] = { 0 }, fl, len;
	bool have[0x201] = { false };
	size_t k = 3;

	if (n < 4 || crc8(f + 1, n - 2) != f[n - 1]) {
		dbg("bad frame (%zu bytes)", n);
		return;
	}
	len = f[1];
	fl = f[2];
	if (fl & 0x80) {
		fl |= f[k++] << 8;
	}
	for (unsigned b = 0; b < sizeof(bits) / sizeof(*bits); b++)
		if (fl & bits[b]) {
			if (k >= n - 1)
				return;
			fld[bits[b]] = f[k++];
			have[bits[b]] = true;
		}
	if (have[0x40])
		len |= fld[0x40] << 8;
	if (have[0x01] && have[0x04])
		L.acked[fld[0x04]] = true;
	if (len && k + len <= n - 1)
		on_payload(f + k, len);
}

static void ssi_rx(const uint8_t *p, size_t n)
{
	enum { OUT, OUT_ESC, IN, IN_ESC };

	for (size_t i = 0; i < n; i++) {
		uint8_t c = p[i];

		switch (L.fstate) {
		case OUT:
			if (c == 0xb0)
				L.fstate = OUT_ESC;
			continue;
		case OUT_ESC:
			L.fstate = c == 0x00 ? IN : c == 0xb0 ? OUT_ESC : OUT;
			L.flen = 0;
			continue;
		case IN:
			if (c == 0xb0) {
				L.fstate = IN_ESC;
				continue;
			}
			break;
		case IN_ESC:
			L.fstate = IN;
			if (c == 0x00) {		/* new frame */
				L.flen = 0;
				continue;
			}
			if (c == 0x01) {		/* end of frame */
				on_frame(L.frame, L.flen);
				L.fstate = OUT;
				continue;
			}
			c = c == 3 ? 0xb0 : c == 4 ? 0x11 : c == 5 ? 0x13 : c;
			break;
		}
		if (L.flen < sizeof(L.frame))
			L.frame[L.flen++] = c;
		else
			L.fstate = OUT;
	}
}

/* one round of I/O on the chip and the clients */
static void pump(int timeout_ms)
{
	struct pollfd pf[1 + MAX_LISTEN + MAX_CLIENTS];
	int n = 0, nc = nclients, nl = nlisten;

	pf[n++] = (struct pollfd){ .fd = L.fd, .events = POLLIN };
	for (int i = 0; i < nl; i++)
		pf[n++] = (struct pollfd){ .fd = listen_fds[i], .events = POLLIN };
	for (int i = 0; i < nc; i++)
		pf[n++] = (struct pollfd){ .fd = clients[i], .events = POLLIN };
	if (poll(pf, n, timeout_ms) <= 0)
		return;
	/* clients first: reading the chip can drop clients (nmea_send) */
	for (int i = nc - 1; i >= 0; i--)
		if (pf[1 + nl + i].revents)
			client_read(i);
	for (int i = 0; i < nl; i++)
		if (pf[1 + i].revents)
			client_accept(listen_fds[i]);
	if (L.fd >= 0 && pf[0].revents) {
		uint8_t buf[4096];
		ssize_t r;

		while ((r = read(L.fd, buf, sizeof(buf))) > 0) {
			trace_io("rx", buf, r);
			ssi_rx(buf, r);
		}
	}
}

static bool wait_for(volatile bool *flag, int timeout_ms)
{
	int64_t end = now_ms() + timeout_ms;

	while (!*flag && !quit) {
		int64_t left = end - now_ms();

		if (left <= 0)
			break;
		pump(left > 20 ? 20 : left);
	}
	return *flag;
}

static void pump_for(int ms)
{
	int64_t end = now_ms() + ms;

	while (!quit && now_ms() < end)
		pump(20);
}

static int ssi_write(const uint8_t *p, size_t n)
{
	trace_io("tx", p, n);
	while (n) {
		ssize_t w = write(L.fd, p, n);

		if (w < 0 && errno == EAGAIN) {
			struct pollfd pf = { .fd = L.fd, .events = POLLOUT };

			poll(&pf, 1, 100);
			continue;
		}
		if (w < 0) {
			logm("%s: write: %s", TTY, strerror(errno));
			return -1;
		}
		p += w;
		n -= w;
	}
	return 0;
}

/* one SSI frame; flags 0x02 = reliable (waits for the chip's ack) */
static int ssi_send(const uint8_t *payload, size_t n, unsigned flags)
{
	static uint8_t body[2 + 16 + 65536], out[2 * sizeof(body) + 8];
	unsigned fl = flags | (n > 0xff ? 0x40 : 0);
	size_t b = 0, o = 0;
	uint8_t rel = L.rel;

	if (n > 0xffff)
		return -1;
	if (fl & 0xff00)
		fl |= 0x80;
	body[b++] = n & 0xff;
	body[b++] = fl & 0xff;
	if (fl & 0x80)
		body[b++] = fl >> 8;
	if (fl & 0x02)
		body[b++] = rel;
	if (fl & 0x40)
		body[b++] = n >> 8;
	if (fl & 0x100)
		body[b++] = 0;
	if (fl & 0x200)
		body[b++] = 0;
	memcpy(body + b, payload, n);
	b += n;

	out[o++] = 0xb0;
	out[o++] = 0x00;
	for (size_t i = 0; i <= b + 1; i++) {
		uint8_t c = i == 0 ? L.seq : i <= b ? body[i - 1] : crc8(body, b);

		if (c == 0xb0 || c == 0x11 || c == 0x13) {
			out[o++] = 0xb0;
			c = c == 0xb0 ? 3 : c == 0x11 ? 4 : 5;
		}
		out[o++] = c;
	}
	out[o++] = 0xb0;
	out[o++] = 0x01;
	L.seq++;

	if (!(fl & 0x02))
		return ssi_write(out, o);
	L.acked[rel] = false;
	L.rel++;
	if (ssi_write(out, o))
		return -1;
	if (!wait_for(&L.acked[rel], 1000)) {
		if (!quit)
			logm("no ack for reliable frame %u", rel);
		return -1;
	}
	return 0;
}

static int ssi_reliable(const uint8_t *p, size_t n)
{
	return ssi_send(p, n, 0x02);
}

static size_t tlv(uint8_t *out, unsigned id, const void *v, size_t n)
{
	size_t h = 2;

	out[0] = id;
	if (n < 0x80) {
		out[1] = n;
	} else {
		out[1] = 0x80 | n >> 8;
		out[2] = n & 0xff;
		h = 3;
	}
	if (n)
		memcpy(out + h, v, n);
	return h + n;
}

/* ------------------------------------------------------------------ */
/* MeIf messages (GlPe_MeIfImpl / GlPePexHostEncoder in stock gpsd) */

enum {	/* GLCP_REQUEST states */
	ST_INIT = 0, ST_READY = 1, ST_STOPPED = 3, ST_STARTING = 4, ST_RUNNING = 5,
	ST_PASSTHROUGH = 10, ST_ONCHIP_STATE_GET = 11,
};

static unsigned meif_seq;
static int meif_gap_ms;	/* pause after each message (the session needs it) */

static int meif_send(unsigned id, const uint8_t *data, size_t n)
{
	static uint8_t m[4096], s[2 * sizeof(m) + 4], t[1003 + 3];
	size_t ml = 0, sl = 0;
	unsigned sum = 0;

	if (n + 8 > sizeof(m))
		return -1;
	put16(m, id);
	put16(m + 2, n + 6);
	put16(m + 4, 0);
	memcpy(m + 6, data, n);
	for (size_t i = 0; i < n + 6; i++)
		sum += m[i];
	put16(m + 4, sum);
	put16(m + 6 + n, meif_seq++ & 0xff);
	ml = n + 8;

	s[sl++] = 0xc0;
	s[sl++] = 0xc1;
	for (size_t i = 0; i < ml; i++) {
		if (m[i] == 0xc0)
			s[sl++] = 0xc0;
		s[sl++] = m[i];
	}
	s[sl++] = 0xc0;
	s[sl++] = 0xc4;

	/* TLV 1e: u16 len | up to 1000 bytes of the stream, one per frame */
	for (size_t o = 0; o < sl; o += 1000) {
		size_t k = sl - o < 1000 ? sl - o : 1000;
		uint8_t v[1002];

		put16(v, k);
		memcpy(v + 2, s + o, k);
		if (ssi_reliable(t, tlv(t, 0x1e, v, k + 2)))
			return -1;
	}
	if (meif_gap_ms)
		pump_for(meif_gap_ms);
	return 0;
}

static int glcp_request(unsigned state)
{
	uint8_t d[2];

	put16(d, state);
	return meif_send(0x0003, d, sizeof(d));
}

/* 0006 GLCP_SET_CONFIGURATION, stock values 0 / 0x7f / 3 */
static int config_set(void)
{
	static const uint8_t d[42] = {
		0x03, 0x00, 0x05, 0x00, 0x10, 0x00, 0xe8, 0x03, 0x01, 0x00,
		0x00, 0x03, 0x00, 0x00, 0x0a, 0x00, 0x3c, 0x00, 0x06, 0x80,
		0x08, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x80, 0x10, 0x00,
		0x7f, 0x00, 0x00, 0x00, 0x03, 0x00, 0x00, 0x00,
	};

	return meif_send(0x0006, d, sizeof(d));
}

/* 0006 short form, sent with REQUEST(PASSTHROUGH) before the ESW patch */
static int config_pre(void)
{
	static const uint8_t d[18] = {
		0x01, 0x00, 0x01, 0x80, 0x10, 0x00, 0x7f, 0x00, 0x00, 0x00,
		0x03, 0x00, 0x00, 0x00,
	};

	return meif_send(0x0006, d, sizeof(d));
}

/* 0006 measurement config (SetMeasConfig): interval 1000 ms */
static int meas_config(bool running)
{
	uint8_t d[18] = {
		0x01, 0x00, 0x05, 0x00, 0x10, 0x00, 0xe8, 0x03, 0x01, 0x00,
		0x00, 0x03, 0x00, 0x00, 0x0a, 0x00, 0x3c, 0x00,
	};

	d[10] = running ? 2 : 0;
	return meif_send(0x0006, d, sizeof(d));
}

/* 0010 GLCP_CONFIG_PARAMS: u16 0x8000 | u32 0 | string | 4 - len % 4 zeros */
static int config_params(const char *str)
{
	uint8_t d[256] = { 0x00, 0x80 };
	size_t n = strlen(str);

	memcpy(d + 6, str, n);
	return meif_send(0x0010, d, 6 + n + 4 - (n & 3));
}

/* 0017 on-chip config: u16 len | string | 6 - len % 4 zeros */
static int onchip_config(const char *str)
{
	uint8_t d[512] = { 0 };
	size_t n = strlen(str);

	put16(d, n);
	memcpy(d + 2, str, n);
	return meif_send(0x0017, d, 2 + n + 6 - (n & 3));
}

/* 8187 PEX_TIME_INJECT_GPS_TIME: week u16 | ms u32 | uncertainty ms u32 */
static int time_inject(void)
{
	struct timespec ts;
	uint8_t d[10];
	int64_t ms;

	clock_gettime(CLOCK_REALTIME, &ts);
	ms = (ts.tv_sec - GPS_EPOCH + GPS_LEAP) * 1000LL + ts.tv_nsec / 1000000;
	put16(d, ms / 604800000);
	put32(d + 2, ms % 604800000);
	put32(d + 6, 1000);
	return meif_send(0x8187, d, sizeof(d));
}

/* 818e PEX request info: id u16 | 8 bytes | u16 | u32 | 6 zeros */
static int req_info(unsigned id, bool arg)
{
	static const uint8_t a[8] = { 0x01, 0x00, 0x01, 0x00, 0x64, 0x00, 0x03, 0x00 };
	uint8_t d[22] = { 0 };

	put16(d, id);
	if (arg)
		memcpy(d + 2, a, sizeof(a));
	return meif_send(0x818e, d, sizeof(d));
}

/* 8183 PEX_MICRO_NAV_CONFIG: 2 zeros | flags u32 | ignore u16 | u16 50 | u16 | u16 */
static int micronav_config(uint32_t flags)
{
	uint8_t d[14] = { 0 };

	put32(d + 2, flags);
	put16(d + 8, 50);
	return meif_send(0x8183, d, sizeof(d));
}

/* 0009 GLCP_BATCHING sub 0x4004 (Batching), on u8 and zeros otherwise */
static int batching(bool on)
{
	uint8_t d[26] = { 0 };

	put32(d + 8, 0x40040001);
	put16(d + 12, 0x10);
	d[14] = on;
	return meif_send(0x0009, d, sizeof(d));
}

/* 0009 sub 0x4005 SendBatchMeas: flush u8 | pad | count u16 */
static int batch_meas(bool flush, unsigned count)
{
	uint8_t d[18] = { 0 };

	put32(d + 8, 0x40050001);
	put16(d + 12, 8);
	d[14] = flush;
	put16(d + 16, count);
	return meif_send(0x0009, d, sizeof(d));
}

/* 8002 GLCP_PASSTHROUGH: u16 len | raw chip ROM loader bytes | pad to even */
static int passthrough(const void *raw, size_t n)
{
	uint8_t d[2 + 1024 + 1] = { 0 };

	if (n > 1024)
		return -1;
	put16(d, n);
	memcpy(d + 2, raw, n);
	return meif_send(0x8002, d, (2 + n + 1) & ~1u);
}

/* ------------------------------------------------------------------ */
/* chip -> host */

static struct {
	bool state_report;	/* 0004 */
	int state;		/* new state from the last 0004 */
	bool esw_started;	/* e0 framing seen after the loader start */
	bool loader;		/* collect 8002 replies */
	uint8_t lrx[8192];	/* ROM loader reply bytes */
	size_t llen;
	int64_t last_msg;
	unsigned nfix;
	int64_t session_start;
} R;

/* ROM loader byte-sum reply: fe 00 fd 4a addr u32 sum u24 check fc */
static bool loader_sum(uint32_t addr, uint32_t *sum)
{
	for (size_t j = 0; j + 13 <= R.llen; j++)
		if (!memcmp(R.lrx + j, "\xfe\x00\xfd\x4a", 4) &&
		    get32(R.lrx + j + 4) == addr) {
			*sum = R.lrx[j + 8] | R.lrx[j + 9] << 8 | R.lrx[j + 10] << 16;
			return true;
		}
	return false;
}

/* MSB-first reader over u32 little-endian words (GlBitBuffer) */
struct bits {
	const uint8_t *p;
	size_t nbits, pos;
};

static uint32_t bits_u(struct bits *b, int w)
{
	uint32_t v = 0;

	for (int i = 0; i < w; i++, b->pos++) {
		size_t word = b->pos / 32, bit = 31 - b->pos % 32;
		uint32_t x = 0;

		if (b->pos < b->nbits)
			x = get32(b->p + word * 4) >> bit & 1;
		v = v << 1 | x;
	}
	return v;
}

static int32_t bits_s(struct bits *b, int w)
{
	uint32_t v = bits_u(b, w);

	return w < 32 && v >> (w - 1) ? (int32_t)(v - (1u << w)) : (int32_t)v;
}

#define MAX_SATS	64

/*
 * One satellite signal of a POSITION blob (GlMicroNavFixStatus, as read by
 * GlPeBatchMgr::UpdateGlFixStatusFromMicroNavFixStatus):
 *   id     bits 0-5 PRN/slot, bits 6/7 used in the fix
 *   flags  bits 0-3 constellation (SAT_*), bit 4 elevation/azimuth valid,
 *          bits 6/7 per-satellite data masks
 *   cn0    bits 0-5 C/N0 dB-Hz, bits 6-7 band (0 L1/E1/B1I/G1, 0x80 the
 *          L5/E5a/B2a signal of a dual-band satellite)
 */
struct sat {
	uint8_t id, flags, cn0;
	int elev, azim;
};

enum { SAT_GPS, SAT_SBAS, SAT_GLONASS, SAT_QZSS, SAT_BEIDOU = 5, SAT_GALILEO, SAT_NAVIC };

struct fix {
	unsigned reason, remaining, type, flags, err, nsat, nused;
	struct sat sat[MAX_SATS];
	double geoid, hdop, lat, lon, speed, track;
	int alt;
	unsigned utc_days;
	uint32_t utc_ms;
};

/* GlPeBatchMgr::DecodePositionBlob, version >= 1 layout */
static bool decode_compactmeas(const uint8_t *d, size_t n, struct fix *f)
{
	struct bits b;
	unsigned paylen, ver = 0, x[11];
	uint8_t words[1024] = { 0 };

	memset(f, 0, sizeof(*f));
	if (n < 2)
		return false;
	paylen = get16(d);
	if (paylen > n - 2 || paylen > sizeof(words) - 4)
		return false;
	memcpy(words, d + 2, paylen);
	b = (struct bits){ words, ((paylen + 3) & ~3u) * 8, 0 };
	f->reason = bits_u(&b, 4);
	f->remaining = bits_u(&b, 16);
	f->type = bits_u(&b, 3);
	if (f->type != 3)
		return true;
	bits_u(&b, 16);
	bits_u(&b, 32), bits_u(&b, 32), bits_u(&b, 32);	/* request ids */
	if (bits_u(&b, 2))
		return false;
	if (bits_u(&b, 1))
		ver = bits_u(&b, 8);
	if (ver < 1)
		return false;
	for (int i = 0; i < 11; i++)
		x[i] = bits_u(&b, 1);
	/* order: f1 f2 f0 f3 f5 f4 altType f6 f7 f9 f8 */
	f->flags = x[2] | x[0] << 1 | x[1] << 2 | x[3] << 3 | x[5] << 4 |
		   x[4] << 5 | x[7] << 6 | x[8] << 7 | x[10] << 8 | x[9] << 9;
	f->err = bits_u(&b, 10);
	bits_u(&b, 32);
	f->geoid = bits_s(&b, 16) / 64.0;
	f->hdop = bits_s(&b, 16) / 64.0;
	bits_s(&b, 16);
	if (f->flags & 1) {
		f->lat = bits_s(&b, 32) / 8388608.0;
		f->lon = bits_s(&b, 32) / 8388608.0;
		if (f->flags & 2) {
			f->alt = bits_s(&b, 16);
			if (ver > 2)
				bits_u(&b, 16);
		}
	}
	if (f->flags & 0x10) {
		f->speed = bits_s(&b, 16) / 64.0;
		if (ver > 2)
			bits_u(&b, 16);
	}
	if (f->flags & 8) {
		f->track = bits_s(&b, 16) / 64.0;
		if (ver > 2)
			bits_u(&b, 16);
	}
	bits_s(&b, 16), bits_s(&b, 16), bits_s(&b, 16), bits_s(&b, 32);
	if (f->flags & 0x20) {
		f->utc_days = bits_u(&b, 16);
		f->utc_ms = bits_u(&b, 32);
	}
	if (f->flags & 0x40) {
		f->nsat = bits_u(&b, 8);
		f->nused = ver >= 2 ? bits_u(&b, 8) : 0;
		for (unsigned i = 0; i < f->nsat; i++) {
			struct sat s;

			s.id = bits_u(&b, 8);
			s.flags = bits_u(&b, 8);
			s.elev = bits_s(&b, 16) / 64;
			s.azim = bits_s(&b, 16) / 64;
			s.cn0 = bits_u(&b, 8);
			if (i < MAX_SATS)
				f->sat[i] = s;
		}
		if (f->nsat > MAX_SATS)
			f->nsat = MAX_SATS;
	}
	return b.pos <= b.nbits;
}

static void nmea_add(char *out, size_t *len, size_t cap, const char *fmt, ...)
{
	char body[160];
	unsigned cs = 0;
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(body, sizeof(body), fmt, ap);
	va_end(ap);
	if (n < 0 || n >= (int)sizeof(body))
		return;
	for (char *c = body; *c; c++)
		cs ^= (unsigned char)*c;
	n = snprintf(out + *len, cap - *len, "$%s*%02X\r\n", body, cs);
	if (n > 0 && *len + n < cap)
		*len += n;
}

static void nmea_coord(char *s, size_t n, double v, int deg_digits, char pos, char neg)
{
	double a = fabs(v);
	int d = a;

	snprintf(s, n, "%0*d%08.5f,%c", deg_digits, d, (a - d) * 60, v >= 0 ? pos : neg);
}

/*
 * GGA + RMC + GST; the blob altitude is ellipsoidal (GGA: alt - geoid sep).
 * GST carries the chip's horizontal error estimate (err, metres), split
 * evenly over latitude and longitude so that hypot(lat, lon) = err.
 */
static void fix_to_nmea(const struct fix *f, char *out, size_t *plen, size_t cap)
{
	char hms[16], dmy[8], lat[24], lon[24], alt[48] = ",,,";
	char spd[16] = "", trk[16] = "";
	unsigned ms = f->utc_ms;
	time_t day = (time_t)f->utc_days * 86400;
	struct tm tm;

	snprintf(hms, sizeof(hms), "%02u%02u%05.2f", ms / 3600000, ms / 60000 % 60,
		 ms % 60000 / 1000.0);
	gmtime_r(&day, &tm);
	strftime(dmy, sizeof(dmy), "%d%m%y", &tm);
	nmea_coord(lat, sizeof(lat), f->lat, 2, 'N', 'S');
	nmea_coord(lon, sizeof(lon), f->lon, 3, 'E', 'W');
	if (f->flags & 2)
		snprintf(alt, sizeof(alt), "%.1f,M,%.1f,M", f->alt - f->geoid, f->geoid);
	if (f->flags & 0x10)
		snprintf(spd, sizeof(spd), "%.2f", f->speed * 1.943844);
	if (f->flags & 8)
		snprintf(trk, sizeof(trk), "%.1f", f->track);
	nmea_add(out, plen, cap, "GPGGA,%s,%s,%s,1,%02u,%.1f,%s,,",
		 hms, lat, lon, f->nused ? f->nused : f->nsat, f->hdop, alt);
	nmea_add(out, plen, cap, "GPRMC,%s,A,%s,%s,%s,%s,%s,,,A",
		 hms, lat, lon, spd, trk, dmy);
	if (f->err)
		nmea_add(out, plen, cap, "GPGST,%s,,,,,%.1f,%.1f,",
			 hms, f->err * M_SQRT1_2, f->err * M_SQRT1_2);
}

/*
 * NMEA 4.11 satellite sentences, one talker per constellation:
 * - GSV per signal (L1 and the L5 band of dual-band satellites in separate
 *   groups, signal ID last), elevation/azimuth empty when the chip has none;
 * - GSA per constellation (system ID last) listing the satellites the chip
 *   used, mode 1 without a fix.
 */
static const struct gnss_sys {
	const char *talker;
	int sysid, nmea_base;	/* NMEA satellite ID = PRN/slot + nmea_base */
	int sig_l1, sig_l5;	/* NMEA signal IDs, 0: not emitted */
} gnss_sys[8] = {
	[SAT_GPS] = { "GP", 1, 0, 1, 7 },
	[SAT_SBAS] = { "GP", 1, 32, 1, 0 },
	[SAT_GLONASS] = { "GL", 2, 64, 1, 0 },
	[SAT_QZSS] = { "GQ", 5, 0, 1, 7 },
	[SAT_BEIDOU] = { "GB", 4, 0, 1, 5 },
	[SAT_GALILEO] = { "GA", 3, 0, 7, 1 },
	[SAT_NAVIC] = { "GI", 6, 0, 1, 0 },
};

static int sat_signal(const struct sat *s)
{
	const struct gnss_sys *g = &gnss_sys[s->flags & 7];

	if ((s->flags & 0xf) > SAT_NAVIC || !g->talker || !(s->id & 0x3f))
		return 0;
	switch (s->cn0 & 0xc0) {
	case 0x00: return g->sig_l1;
	case 0x80: return g->sig_l5;
	default: return 0;
	}
}

static void sats_to_nmea(const struct fix *f, bool fixed, char *out, size_t *plen, size_t cap)
{
	char body[160];

	for (int c = 0; c <= SAT_NAVIC; c++) {
		const struct gnss_sys *g = &gnss_sys[c];
		int used[MAX_SATS], nused = 0;

		if (!g->talker || c == SAT_SBAS)
			continue;	/* SBAS goes with GPS */
		for (int band = 0; band < 2; band++) {
			const struct sat *list[MAX_SATS];
			int n = 0, sig = band ? g->sig_l5 : g->sig_l1;

			if (!sig)
				continue;
			for (unsigned i = 0; i < f->nsat; i++) {
				const struct sat *s = &f->sat[i];
				int sc = s->flags & 0xf;

				if ((sc == c || (c == SAT_GPS && sc == SAT_SBAS)) &&
				    sat_signal(s) == sig && (s->cn0 & 0x80) == band * 0x80)
					list[n++] = s;
			}
			for (int i = 0; i < n; i += 4) {
				int bl = snprintf(body, sizeof(body), "%sGSV,%d,%d,%02d", g->talker,
						  (n + 3) / 4, i / 4 + 1, n);

				for (int j = i; j < i + 4 && j < n; j++) {
					const struct sat *s = list[j];
					int id = (s->id & 0x3f) + gnss_sys[s->flags & 7].nmea_base;

					if (s->flags & 0x10)
						bl += snprintf(body + bl, sizeof(body) - bl, ",%02d,%02d,%03d,%02d",
							       id, s->elev, (s->azim + 360) % 360, s->cn0 & 0x3f);
					else
						bl += snprintf(body + bl, sizeof(body) - bl, ",%02d,,,%02d",
							       id, s->cn0 & 0x3f);
				}
				snprintf(body + bl, sizeof(body) - bl, ",%X", sig);
				nmea_add(out, plen, cap, "%s", body);
			}
		}
		for (unsigned i = 0; i < f->nsat; i++) {
			const struct sat *s = &f->sat[i];
			int sc = s->flags & 0xf, id;
			bool dup = false;

			if (!(s->id & 0xc0) || !(sc == c || (c == SAT_GPS && sc == SAT_SBAS)))
				continue;
			id = (s->id & 0x3f) + gnss_sys[sc & 7].nmea_base;
			for (int j = 0; j < nused; j++)
				dup |= used[j] == id;
			if (!dup)
				used[nused++] = id;
		}
		for (int i = 0; i < nused; i += 12) {
			int bl = snprintf(body, sizeof(body), "GNGSA,A,%d", fixed ? 3 : 1);

			for (int j = i; j < i + 12; j++)
				bl += j < nused ? snprintf(body + bl, sizeof(body) - bl, ",%02d", used[j])
						: snprintf(body + bl, sizeof(body) - bl, ",");
			if (fixed)
				snprintf(body + bl, sizeof(body) - bl, ",,%.1f,,%d", f->hdop, g->sysid);
			else
				snprintf(body + bl, sizeof(body) - bl, ",,,,%d", g->sysid);
			nmea_add(out, plen, cap, "%s", body);
		}
	}
}

static void on_compactmeas(const uint8_t *d, size_t n)
{
	/* one send per epoch: readers can take a read as one epoch */
	char out[4096];
	size_t len = 0;
	struct fix f;

	if (!decode_compactmeas(d, n, &f)) {
		dbg("undecodable position report");
		return;
	}
	if (f.type != 3 || !(f.flags & 1) || !(f.flags & 0x20)) {
		dbg("no fix (blob type %u, %u signals)", f.type, f.nsat);
		/* satellites in view while acquiring */
		if (f.type == 3)
			sats_to_nmea(&f, false, out, &len, sizeof(out));
		if (len)
			nmea_send(out, len);
		return;
	}
	if (!R.nfix++)
		logm("first fix after %.1f s: err %u m, %u sats, hdop %.1f",
		     (now_ms() - R.session_start) / 1000.0, f.err, f.nsat, f.hdop);
	/* no coordinates in the log */
	dbg("fix utc %02u:%02u:%02u err %u m sats %u/%u hdop %.1f",
	    f.utc_ms / 3600000, f.utc_ms / 60000 % 60, f.utc_ms / 1000 % 60,
	    f.err, f.nused, f.nsat, f.hdop);
	fix_to_nmea(&f, out, &len, sizeof(out));
	sats_to_nmea(&f, true, out, &len, sizeof(out));
	nmea_send(out, len);
}

static void on_meif(const uint8_t *m, size_t n)
{
	unsigned id, len, sum = 0;

	if (n < 8)
		return;
	id = get16(m);
	len = get16(m + 2);
	if (len + 2 != n || len < 6) {
		dbg("meif 0x%04x: length %u in a %zu-byte message", id, len, n);
		return;
	}
	for (size_t i = 0; i < len; i++)
		if (i < 4 || i >= 6)
			sum += m[i];
	if ((sum & 0xffff) != get16(m + 4))
		dbg("meif 0x%04x: checksum 0x%04x, expected 0x%04x", id, get16(m + 4), sum & 0xffff);
	R.last_msg = now_ms();
	m += 6;
	n = len - 6;
	switch (id) {
	case 0x0004:	/* state report: previous u16 | new u16 | ... */
		R.state_report = true;
		if (n >= 4) {
			R.state = get16(m + 2);
			dbg("chip state %u -> %u", get16(m), R.state);
		}
		break;
	case 0x000d:	/* chip debug printf */
		dbg("chip: %.*s", (int)n, (const char *)m);
		break;
	case 0x8002:	/* ROM loader replies; may span several messages */
		if (R.loader && n >= 2) {
			unsigned k = get16(m);

			if (k > n - 2)
				k = n - 2;
			if (R.llen + k <= sizeof(R.lrx)) {
				memcpy(R.lrx + R.llen, m + 2, k);
				R.llen += k;
			}
			if (memmem(R.lrx, R.llen, "\xe0\x00", 2))
				R.esw_started = true;
		}
		break;
	case 0x8101:
		on_compactmeas(m, n);
		break;
	}
}

/* ------------------------------------------------------------------ */
/* boot: SensorHub.patch (replaces stock lhd) */

static uint8_t *read_all(const char *path, size_t *len)
{
	FILE *f = fopen(path, "rbe");
	uint8_t *buf = NULL;
	size_t cap = 0, n = 0, r;

	if (!f) {
		logm("%s: %s", path, strerror(errno));
		return NULL;
	}
	do {
		if (n == cap) {
			uint8_t *nb = realloc(buf, cap = cap ? cap * 2 : 65536);

			if (!nb) {
				free(buf);
				fclose(f);
				return NULL;
			}
			buf = nb;
		}
		r = fread(buf + n, 1, cap - n, f);
		n += r;
	} while (r);
	fclose(f);
	*len = n;
	return buf;
}

static int hexval(int c)
{
	return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 :
	       c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

/* <Key=Value>... <PatchAddress=0x...> ... <Blob=HEX> -> binary blob */
static uint8_t *sensorhub_patch(uint32_t *entry, size_t *blen)
{
	size_t n, o = 0;
	uint8_t *t = read_all(opt_patch, &n), *blob;
	char *s, *e;
	int hi = -1;

	if (!t)
		return NULL;
	t = realloc(t, n + 1);
	t[n] = 0;
	s = strstr((char *)t, "<PatchAddress=");
	e = strstr((char *)t, "<Blob=");
	if (!s || !e) {
		logm("%s: not a SensorHub patch", opt_patch);
		free(t);
		return NULL;
	}
	*entry = strtoul(s + 14, NULL, 16);
	blob = (uint8_t *)e;	/* decode in place */
	for (e += 6; *e && *e != '>'; e++) {
		int v = hexval(*e);

		if (v < 0)
			continue;
		if (hi < 0) {
			hi = v;
		} else {
			blob[o++] = hi << 4 | v;
			hi = -1;
		}
	}
	memmove(t, blob, o);
	*blen = o;
	return t;
}

static int regw(uint8_t *out, uint32_t addr, uint32_t v)
{
	uint8_t d[10];

	put32(d, addr);
	put16(d + 4, 4);
	put32(d + 6, v);
	return tlv(out, 0x05, d, sizeof(d));
}

static int rom_call(uint8_t *out, uint32_t addr)
{
	uint8_t d[20] = { 0 };

	put32(d, addr);
	return tlv(out, 0x37, d, sizeof(d));
}

static int chip_boot(void)
{
	static const uint8_t autobaud[36] = {
		[0 ... 35] = 0x80
	}, sync[] = { 0x00, 0x01 };
	uint8_t t[8192], *blob;
	size_t blen, k;
	uint32_t entry;
	int64_t t0;
	unsigned nw = 0;

	blob = sensorhub_patch(&entry, &blen);
	if (!blob)
		return -1;
	for (size_t i = 0; i + 3 <= blen; i += 3 + get16(blob + i + 1))
		if (blob[i] != 0xf1 || i + 3 + get16(blob + i + 1) > blen) {
			logm("%s: unsupported record 0x%02x", opt_patch, blob[i]);
			goto fail;
		}

	memset(L.acked, 0, sizeof(L.acked));
	L.seq = L.rel = 0;
	L.fstate = L.mstate = 0;
	if (nstandby(0))
		goto fail;
	sleep_ms(25);
	if (nstandby(1))
		goto fail;
	sleep_ms(25);
	if (ctl("BBD:PassThru=1") || ssi_write(autobaud, 16))
		goto fail;
	sleep_ms(140);
	if (ssi_write(autobaud, 36))
		goto fail;
	sleep_ms(100);

	L.sync_seen = false;
	if (ssi_send(sync, sizeof(sync), 0x300) || !wait_for(&L.sync_seen, 1000)) {
		logm("chip: no sync reply");
		goto fail;
	}
	L.tlv_seen[0x03] = false;
	k = tlv(t, 0x46, "\x00\x03", 2);
	k += tlv(t + k, 0x00, NULL, 0);
	if (ssi_reliable(t, k) || !wait_for(&L.tlv_seen[0x03], 1000)) {
		logm("chip: no version reply");
		goto fail;
	}

	/* f1 records: addr u32 | data -> TLV 05 addr u32 | len u16 | data */
	t0 = now_ms();
	for (size_t i = 0; i + 3 <= blen; i += 3 + get16(blob + i + 1), nw++) {
		unsigned len = get16(blob + i + 1);
		uint8_t v[4096 + 6];

		if (len < 4 || len - 4 > 4096)
			goto fail;
		memcpy(v, blob + i + 3, 4);
		put16(v + 4, len - 4);
		memcpy(v + 6, blob + i + 7, len - 4);
		if (ssi_reliable(t, tlv(t, 0x05, v, len + 2)))
			goto fail;
	}
	logm("SensorHub patch: %u writes in %.2f s", nw, (now_ms() - t0) / 1000.0);

	/* start it, same bundle as stock lhd: ROM call 0x14f3d, 34 0000,
	 * 34 0300, register writes 0x404100e8..fc, jump to the entry */
	k = rom_call(t, 0x00014f3d);
	k += tlv(t + k, 0x34, "\x00\x00", 2);
	k += tlv(t + k, 0x34, "\x03\x00", 2);
	k += regw(t + k, 0x404100fc, 0xbcbc600d);
	k += regw(t + k, 0x404100e8, 0x00000100);
	k += regw(t + k, 0x404100ec, 0);
	k += regw(t + k, 0x404100e8, 0xffffffff);
	k += regw(t + k, 0x404100ec, 0xffffffff);
	k += regw(t + k, 0x404100fc, 0);
	k += rom_call(t + k, entry);
	if (ssi_send(t, k, 0))
		goto fail;
	sleep_ms(150);
	L.rel = 0;
	L.sync_seen = false;
	if (ssi_send(sync, sizeof(sync), 0x300) || !wait_for(&L.sync_seen, 1000)) {
		logm("chip: no resync after patch start");
		goto fail;
	}
	L.tlv_seen[0x3b] = false;
	if (ssi_reliable(t, tlv(t, 0x3a, NULL, 0)) || !wait_for(&L.tlv_seen[0x3b], 1000))
		logm("chip: no banner");
	if (ssi_send(t, tlv(t, 0x2b, "\x01\x00\xff\xdf\x11\x00", 6), 0) ||
	    ssi_reliable(t, tlv(t, 0x34, "\x07\x02", 2)))
		goto fail;
	if (ctl("BBD:PassThru=0") || ctl("ESW:READY"))
		goto fail;
	free(blob);
	return 0;
fail:
	free(blob);
	return -1;
}

/* ------------------------------------------------------------------ */
/* ESW ("GLL") patch through the chip ROM loader (replaces the stock gpsd
 * GlMeSrdAsicInitPatchLoader). File: "BCMGLLP1" | u16 siglen | sig |
 * u32 n | n x (u32 addr | u32 size | data), written by the cutter. */

#define MAX_SEGS 8
struct seg {
	uint32_t addr, size;
	const uint8_t *data;
};

static const struct {
	uint32_t addr, len;
} retention[] = { { 0x5fdf0, 0x200 }, { 0x68000, 0x1000 }, { 0x69000, 0x1000 } };

static uint32_t bytesum(const uint8_t *p, size_t n)
{
	uint32_t s = 0;

	while (n--)
		s += *p++;
	return s & 0xffffff;
}

static int rom_query(uint32_t addr, unsigned len)
{
	uint8_t q[11] = { 0xfe, 0x00, 0xfd, 0x4a };

	put32(q + 4, addr);
	put16(q + 8, len);
	q[10] = 0xfc;
	return passthrough(q, sizeof(q));
}

static int rom_write(uint32_t addr, const uint8_t *data, size_t n)
{
	uint8_t h[8] = { 0xfe, n / 4 - 1, 0xfd, 0x4c };

	put32(h + 4, addr);
	if (passthrough(h, sizeof(h)) || passthrough(data, n) ||
	    passthrough("\xfc", 1))
		return -1;
	return rom_query(addr, n);
}

static bool wait_loader(bool (*done)(void *), void *arg, int timeout_ms)
{
	int64_t end = now_ms() + timeout_ms;

	while (!quit && !done(arg) && now_ms() < end)
		pump(10);
	return done(arg);
}

struct sumset {
	uint32_t addr[128];
	int n;
};

static bool have_sums(void *arg)
{
	struct sumset *s = arg;
	uint32_t v;

	for (int i = 0; i < s->n; i++)
		if (!loader_sum(s->addr[i], &v))
			return false;
	return true;
}

static bool have_esw(void *arg)
{
	return R.esw_started;
}

static int load_gll_patch(void)
{
	static const uint8_t start[] = { 0xff, 0x00, 0xfd, 0x05, 0x55, 0xaa, 0x00, 0xff, 0xfc };
	struct seg segs[MAX_SEGS];
	struct sumset ss = { .n = 0 };
	size_t n, o;
	uint8_t *b = read_all(opt_gll, &n), rec[12] = { 0 };
	unsigned nseg, bad = 0;
	bool retained = true;
	int64_t t0;
	int ret = -1;

	if (!b)
		return -1;
	if (n < 14 || memcmp(b, "BCMGLLP1", 8))
		goto badfile;
	o = 10 + get16(b + 8);
	if (o + 4 > n || (nseg = get32(b + o)) > MAX_SEGS)
		goto badfile;
	o += 4;
	for (unsigned i = 0; i < nseg; i++) {
		if (o + 8 > n)
			goto badfile;
		segs[i] = (struct seg){ get32(b + o), get32(b + o + 4), b + o + 8 };
		o += 8 + segs[i].size;
		if (o > n || (segs[i].size & 3))
			goto badfile;
	}

	meif_seq = 1;
	R.state_report = false;
	if (glcp_request(ST_PASSTHROUGH) || !wait_for(&R.state_report, 2000)) {
		logm("chip: no state report for PASSTHROUGH");
		goto out;
	}
	if (config_pre())
		goto out;

	R.llen = 0;
	R.loader = true;
	R.esw_started = false;
	{
		uint8_t ab[16];

		memset(ab, 0x80, sizeof(ab));
		if (passthrough(ab, sizeof(ab)) ||
		    passthrough("\xff\x00\xfd\x0f\x01\x33\x0e\x10\xfc", 9))
			goto out;
	}
	sleep_ms(20);
	if (passthrough("\xff\x00\xfd\x09\x00\x00\x00\xc0\xfc", 9) ||
	    passthrough("\xff\x00\xfd\x09\x54\x0a\x00\xc0\xfc", 9))
		goto out;
	for (unsigned i = 0; i < 3; i++) {
		if (rom_query(retention[i].addr, retention[i].len))
			goto out;
		ss.addr[ss.n++] = retention[i].addr;
	}
	wait_loader(have_sums, &ss, 2000);
	for (unsigned i = 0; i < 3; i++) {
		uint32_t got, want = 0xffffffff;

		for (unsigned j = 0; j < nseg; j++)
			if (segs[j].addr <= retention[i].addr &&
			    retention[i].addr + retention[i].len <= segs[j].addr + segs[j].size)
				want = bytesum(segs[j].data + retention[i].addr - segs[j].addr,
					       retention[i].len);
		if (!loader_sum(retention[i].addr, &got) || got != want)
			retained = false;
	}

	if (retained) {
		logm("ESW patch retained");
		if (passthrough(start, sizeof(start)))
			goto out;
	} else {
		t0 = now_ms();
		R.llen = 0;
		ss.n = 0;
		for (unsigned i = 0; i < nseg; i++) {
			uint32_t blk = segs[i].size <= 0x210 ? segs[i].size : 0x3fc;

			for (uint32_t off = 0; off < segs[i].size; off += blk) {
				uint32_t k = segs[i].size - off < blk ? segs[i].size - off : blk;

				if (ss.n == (int)(sizeof(ss.addr) / sizeof(*ss.addr)) ||
				    rom_write(segs[i].addr + off, segs[i].data + off, k))
					goto out;
				ss.addr[ss.n++] = segs[i].addr + off;
			}
		}
		/* retention record: 6 zeros, 0x7f, 3, u32 BE 1 (as stock gpsd) */
		rec[6] = 0x7f;
		rec[7] = 3;
		rec[11] = 1;
		if (rom_write(0x5fff0, rec, sizeof(rec)) || passthrough(start, sizeof(start)))
			goto out;
		wait_loader(have_sums, &ss, 3000);
		for (unsigned i = 0, s = 0; i < nseg; i++) {
			uint32_t blk = segs[i].size <= 0x210 ? segs[i].size : 0x3fc;

			for (uint32_t off = 0; off < segs[i].size; off += blk, s++) {
				uint32_t k = segs[i].size - off < blk ? segs[i].size - off : blk, got;

				if (!loader_sum(ss.addr[s], &got) ||
				    got != bytesum(segs[i].data + off, k))
					bad++;
			}
		}
		logm("ESW patch: %d blocks in %.2f s, %u bad", ss.n,
		     (now_ms() - t0) / 1000.0, bad);
		if (bad)
			goto out;
	}
	if (!wait_loader(have_esw, NULL, 2000))
		logm("warning: no ESW start packet seen");
	ret = 0;
out:
	R.loader = false;
	free(b);
	return ret;
badfile:
	logm("%s: not a GLL patch file", opt_gll);
	free(b);
	return -1;
}

/* ------------------------------------------------------------------ */
/* on-chip session (stock gpsd's on-chip start/stop sequence) */

static const char onchip_cfg[] =
	"lna_mask:3;rf_mode:2;l5Ext_bias:930;gps_carrier_type:"
	"32332323331213212311113333312323;bds_carrier_type:"
	"222222222222222222333333333333333333333333333333333333322222222;"
	"PPS:on:1,0,30000,2000;PWR:on;WN:2216;Auto:BDS|NIC;";

/* REQUEST(state) and wait until the chip reports it: commands sent before
 * a transition has finished are lost and the chip resets its link */
static bool wait_state(int state, int timeout_ms)
{
	int64_t end = now_ms() + timeout_ms;

	while (R.state != state && !quit && now_ms() < end)
		pump(10);
	return R.state == state;
}

static int glcp_request_wait(unsigned state, int timeout_ms)
{
	R.state = -1;
	if (glcp_request(state))
		return -1;
	if (!wait_state(state, timeout_ms)) {
		if (!quit)
			logm("chip: no state report for request %u", state);
		return -1;
	}
	return 0;
}

static int session_start(void)
{
	int ret = -1;

	meif_seq = 1;
	meif_gap_ms = 5;	/* as stock gpsd */
	R.state = -1;
	if (glcp_request(ST_INIT) || !wait_state(ST_READY, 1000)) {
		logm("chip: not READY after INIT");
		goto out;
	}
	if (glcp_request(ST_ONCHIP_STATE_GET) || config_set() ||
	    config_params("lte_band_13:1;meif_4771:1;") ||
	    onchip_config(onchip_cfg) || glcp_request_wait(ST_STARTING, 2000))
		goto out;
	if (config_set() || time_inject() || req_info(0xffff, false) ||
	    req_info(0x0000, true) || req_info(0xfffe, true) ||
	    micronav_config(0x0012031f) || glcp_request(ST_RUNNING) ||
	    batching(true) || batch_meas(false, 0) || meas_config(true))
		goto out;
	/* RUNNING is reported after the measurement config */
	if (!wait_state(ST_RUNNING, 1000)) {
		logm("chip: not RUNNING");
		goto out;
	}
	ret = 0;
out:
	meif_gap_ms = 0;
	return ret;
}

static int session_stop(void)
{
	int ret;

	meif_gap_ms = 5;
	ret = req_info(0xffff, false) || req_info(0xfffe, false) ||
	      micronav_config(0x00120013) || batching(false) ||
	      batch_meas(false, 0) || meas_config(false) ? -1 : 0;
	/* don't give up on a missing report here, the chip goes off anyway */
	if (!ret)
		glcp_request_wait(ST_STARTING, 1000);
	if (!ret)
		ret = glcp_request_wait(ST_STOPPED, 1000);
	meif_gap_ms = 0;
	return ret;
}

static bool running;

static void chip_off(bool graceful)
{
	uint8_t t[4];

	if (graceful) {
		session_stop();
		pump_for(500);
		ssi_reliable(t, tlv(t, 0x1c, "\x00", 1));
		ctl("GPSD:CORE_OFF");
	}
	nstandby(0);
	if (L.fd >= 0)
		close(L.fd);
	L.fd = -1;
	running = false;
}

static int chip_on(void)
{
	uint8_t t[4];

	L.fd = open(TTY, O_RDWR | O_NONBLOCK | O_CLOEXEC | O_NOCTTY);
	if (L.fd < 0) {
		logm("%s: %s", TTY, strerror(errno));
		return -1;
	}
	if (chip_boot() || ctl("GPSD:CORE_ON") ||
	    ssi_reliable(t, tlv(t, 0x1c, "\x01", 1)) || load_gll_patch())
		goto fail;
	R.nfix = 0;
	R.session_start = R.last_msg = now_ms();
	if (session_start())
		goto fail;
	logm("session started");
	running = true;
	return 0;
fail:
	chip_off(false);
	return -1;
}

static void on_signal(int sig)
{
	quit = 1;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"usage: %s [-v] [-t trace.jsonl] [-p SensorHub.patch] [-g gll-patch.bin] [-s socket] [-n nstandby]\n"
		"  defaults: %s, %s, %s, %s\n",
		argv0, opt_patch, opt_gll, opt_socket, opt_nstandby);
}

int main(int argc, char **argv)
{
	struct sigaction sa = { .sa_handler = on_signal };
	int64_t next_flush = 0, retry_at = 0;
	int backoff = 2000, c;

	while ((c = getopt(argc, argv, "vt:p:g:s:n:h")) != -1)
		switch (c) {
		case 'v': verbose = 1; break;
		case 't':
			trace = fopen(optarg, "we");
			if (!trace) {
				perror(optarg);
				return 1;
			}
			setvbuf(trace, NULL, _IOLBF, 0);
			break;
		case 'p': opt_patch = optarg; break;
		case 'g': opt_gll = optarg; break;
		case 's': opt_socket = optarg; break;
		case 'n': opt_nstandby = optarg; break;
		default: usage(argv[0]); return c == 'h' ? 0 : 1;
		}
	setvbuf(stderr, NULL, _IOLBF, 0);
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);
	signal(SIGPIPE, SIG_IGN);
	if (listen_socket())
		return 1;
	nstandby(0);
	if (socket_activated)
		logm("serving NMEA on %d socket%s from systemd", nlisten, nlisten > 1 ? "s" : "");
	else
		logm("serving NMEA on %s", opt_socket);

	while (!quit) {
		int64_t t = now_ms();

		if (!running && nclients && t >= retry_at) {
			logm("starting GNSS (%d client%s)", nclients, nclients > 1 ? "s" : "");
			if (chip_on()) {
				logm("chip start failed, retrying in %d s", backoff / 1000);
				retry_at = now_ms() + backoff;
				backoff = backoff < 60000 ? backoff * 2 : 60000;
				continue;
			}
			backoff = 2000;
			next_flush = now_ms() + 1000;
		} else if (running && !nclients && t - last_client_ms >= LINGER_MS) {
			logm("no clients, stopping GNSS (%u fixes)", R.nfix);
			chip_off(true);
		} else if (running && t - R.last_msg >= WATCHDOG_MS) {
			logm("chip went quiet, restarting");
			chip_off(true);
			continue;
		} else if (running && t >= next_flush) {
			next_flush += 1000;
			if (next_flush < t)
				next_flush = t + 1000;
			/* one position report per flush */
			if (batch_meas(true, 25)) {
				logm("link error, restarting");
				chip_off(false);
				continue;
			}
		}
		if (running)
			pump(next_flush - now_ms() > 0 ? next_flush - now_ms() : 0);
		else
			pump(nclients ? 200 : 1000);
	}
	if (running) {
		logm("exiting, stopping GNSS (%u fixes)", R.nfix);
		quit = 0;	/* let the stop sequence wait for its acks */
		chip_off(true);
	} else {
		nstandby(0);
	}
	if (!socket_activated)
		unlink(opt_socket);
	return 0;
}
