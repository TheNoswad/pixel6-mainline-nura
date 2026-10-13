// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * android.hardware.gnss-service.waydroid: an AIDL IGnss (V2) HAL for
 * Waydroid that turns the host's NMEA stream into Android locations.
 *
 * The host GNSS daemon (bcm4776-gnssd on the Pixel 6) serves NMEA on a
 * unix socket; a second listener of it is bind-mounted into the container
 * at /dev/waydroid-gnss/nmea.sock, group gps (1021) only, so apps never see
 * it: Android's location permissions stay in charge.
 *
 * The HAL connects only between start() and stop(), so the host keeps the
 * chip powered only while an Android app actually wants GPS. Sentences
 * used: GGA (altitude, HDOP), RMC (position, speed, track, date) and GST
 * (horizontal error estimate). One location is reported per epoch, once
 * the epoch's RMC has arrived and the reader has drained the socket.
 * Satellites come from GSV (NMEA 4.11, one group per signal: L1 and L5 are
 * separate entries with their carrier frequency) and GSA (used in the fix),
 * reported once per drained read, also before the first fix.
 */
#include <aidl/android/hardware/gnss/BnGnss.h>
#include <aidl/android/hardware/gnss/IGnssCallback.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define TAG "gnss-waydroid"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

using aidl::android::hardware::gnss::BnGnss;
using aidl::android::hardware::gnss::ElapsedRealtime;
using aidl::android::hardware::gnss::GnssConstellationType;
using aidl::android::hardware::gnss::GnssLocation;
using aidl::android::hardware::gnss::IAGnss;
using aidl::android::hardware::gnss::IAGnssRil;
using aidl::android::hardware::gnss::IGnss;
using aidl::android::hardware::gnss::IGnssAntennaInfo;
using aidl::android::hardware::gnss::IGnssBatching;
using aidl::android::hardware::gnss::IGnssCallback;
using aidl::android::hardware::gnss::IGnssConfiguration;
using aidl::android::hardware::gnss::IGnssDebug;
using aidl::android::hardware::gnss::IGnssGeofence;
using aidl::android::hardware::gnss::IGnssMeasurementInterface;
using aidl::android::hardware::gnss::IGnssNavigationMessageInterface;
using aidl::android::hardware::gnss::IGnssPowerIndication;
using aidl::android::hardware::gnss::IGnssPsds;
using aidl::android::hardware::gnss::measurement_corrections::IMeasurementCorrectionsInterface;
using aidl::android::hardware::gnss::visibility_control::IGnssVisibilityControl;
using ndk::ScopedAStatus;

static const char *kSocket = "/dev/waydroid-gnss/nmea.sock";

static int64_t boottime_ns()
{
	struct timespec ts;

	clock_gettime(CLOCK_BOOTTIME, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static int64_t realtime_ms()
{
	struct timespec ts;

	clock_gettime(CLOCK_REALTIME, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ------------------------------------------------------------------ */
/* NMEA */

static bool nmea_checksum_ok(const std::string &s)
{
	size_t star = s.rfind('*');
	unsigned cs = 0;

	if (s.size() < 4 || s[0] != '$' || star == std::string::npos || star + 3 > s.size())
		return false;
	for (size_t i = 1; i < star; i++)
		cs ^= (unsigned char)s[i];
	return strtoul(s.substr(star + 1, 2).c_str(), nullptr, 16) == cs;
}

/* fields between '$' and '*' */
static std::vector<std::string> nmea_fields(const std::string &s)
{
	std::vector<std::string> f;
	size_t start = 1, end = s.rfind('*');

	for (size_t i = start; i <= end; i++)
		if (i == end || s[i] == ',') {
			f.push_back(s.substr(start, i - start));
			start = i + 1;
		}
	return f;
}

/* ddmm.mmmm + hemisphere -> degrees */
static bool nmea_coord(const std::string &v, const std::string &h, double *out)
{
	if (v.empty() || h.empty())
		return false;
	double x = strtod(v.c_str(), nullptr);
	double deg = floor(x / 100);

	*out = deg + (x - deg * 100) / 60;
	if (h == "S" || h == "W")
		*out = -*out;
	return true;
}

/*
 * NMEA satellite ID (by talker / GSA system ID) -> Android constellation +
 * svid; NMEA 4.11 signal ID -> carrier frequency (0: unknown)
 */
struct SvKey {
	GnssConstellationType c = GnssConstellationType::UNKNOWN;
	int svid = 0;
	bool operator==(const SvKey &o) const { return c == o.c && svid == o.svid; }
};

static SvKey sv_key(char sys, int id)
{
	using C = GnssConstellationType;
	SvKey k;

	switch (sys) {
	case 'P':	/* GPS talker: GPS, SBAS (33-64), legacy QZSS (193-202) */
		if (id >= 1 && id <= 32)
			k = { C::GPS, id };
		else if (id >= 33 && id <= 64)
			k = { C::SBAS, id + 87 };
		else if (id >= 193 && id <= 202)
			k = { C::QZSS, id };
		break;
	case 'L': if (id >= 65 && id <= 96) k = { C::GLONASS, id - 64 }; break;
	case 'A': if (id >= 1 && id <= 36) k = { C::GALILEO, id }; break;
	case 'B': if (id >= 1 && id <= 63) k = { C::BEIDOU, id }; break;
	case 'Q': if (id >= 1 && id <= 10) k = { C::QZSS, id + 192 }; break;
	case 'I': if (id >= 1 && id <= 14) k = { C::IRNSS, id }; break;
	}
	return k;
}

/* GSA system ID -> talker letter */
static char gsa_sys(int sysid)
{
	static const char m[] = "?PLABQI";

	return sysid >= 1 && sysid <= 6 ? m[sysid] : 0;
}

static double carrier_hz(GnssConstellationType c, int sig)
{
	using C = GnssConstellationType;
	const double L1 = 1575.42e6, L5 = 1176.45e6;

	switch (c) {
	case C::GPS: case C::QZSS: case C::SBAS:
		return sig == 1 ? L1 : sig == 7 || sig == 8 ? L5 : 0;
	case C::GALILEO: return sig == 7 ? L1 : sig == 1 ? L5 : 0;
	case C::BEIDOU: return sig == 1 ? 1561.098e6 : sig == 5 ? L5 : 0;
	case C::IRNSS: return sig == 1 ? L5 : 0;
	default: return 0;	/* GLONASS: FDMA, channel unknown */
	}
}

/* one epoch, keyed by the hhmmss.ss field */
struct Epoch {
	std::string time;
	bool rmc = false, gga = false, gst = false;
	double lat = 0, lon = 0;
	bool has_speed = false, has_track = false, has_alt = false;
	double speed = 0, track = 0, alt = 0, hdop = 0, herr = 0;
	int64_t utc_ms = 0, rx_ns = 0;
};

/* ------------------------------------------------------------------ */
/* HAL */

class Gnss : public BnGnss {
public:
	ScopedAStatus setCallback(const std::shared_ptr<IGnssCallback> &cb) override
	{
		{
			std::lock_guard<std::mutex> l(cb_lock_);
			cb_ = cb;
		}
		if (cb) {
			/* SCHEDULING: minIntervalMs is honoured (by dropping epochs) */
			cb->gnssSetCapabilitiesCb(IGnssCallback::CAPABILITY_SCHEDULING);
			IGnssCallback::GnssSystemInfo info;
			info.yearOfHw = 2021;
			info.name = "waydroid-gnss (host NMEA)";
			cb->gnssSetSystemInfoCb(info);
		}
		return ScopedAStatus::ok();
	}

	ScopedAStatus close() override
	{
		stop();
		std::lock_guard<std::mutex> l(cb_lock_);
		cb_ = nullptr;
		return ScopedAStatus::ok();
	}

	ScopedAStatus start() override
	{
		std::lock_guard<std::mutex> l(run_lock_);

		if (thread_.joinable())
			return ScopedAStatus::ok();
		if (pipe2(wake_, O_CLOEXEC)) {
			LOGE("pipe: %s", strerror(errno));
			return ScopedAStatus::fromServiceSpecificError(IGnss::ERROR_GENERIC);
		}
		running_ = true;
		status(IGnssCallback::GnssStatusValue::SESSION_BEGIN);
		status(IGnssCallback::GnssStatusValue::ENGINE_ON);
		thread_ = std::thread(&Gnss::reader, this);
		LOGI("started (interval %d ms)", min_interval_ms_.load());
		return ScopedAStatus::ok();
	}

	ScopedAStatus stop() override
	{
		std::lock_guard<std::mutex> l(run_lock_);

		if (!thread_.joinable())
			return ScopedAStatus::ok();
		running_ = false;
		(void)!write(wake_[1], "x", 1);
		thread_.join();
		::close(wake_[0]);
		::close(wake_[1]);
		status(IGnssCallback::GnssStatusValue::SESSION_END);
		status(IGnssCallback::GnssStatusValue::ENGINE_OFF);
		LOGI("stopped");
		return ScopedAStatus::ok();
	}

	ScopedAStatus setPositionMode(const IGnss::PositionModeOptions &o) override
	{
		min_interval_ms_ = o.minIntervalMs > 0 ? o.minIntervalMs : 1000;
		return ScopedAStatus::ok();
	}

	ScopedAStatus startNmea() override { nmea_ = true; return ScopedAStatus::ok(); }
	ScopedAStatus stopNmea() override { nmea_ = false; return ScopedAStatus::ok(); }
	ScopedAStatus startSvStatus() override { sv_status_ = true; return ScopedAStatus::ok(); }
	ScopedAStatus stopSvStatus() override { sv_status_ = false; return ScopedAStatus::ok(); }

	/* standalone receiver on the host: no aiding to take */
	ScopedAStatus injectTime(int64_t, int64_t, int32_t) override { return ScopedAStatus::ok(); }
	ScopedAStatus injectLocation(const GnssLocation &) override { return ScopedAStatus::ok(); }
	ScopedAStatus injectBestLocation(const GnssLocation &) override { return ScopedAStatus::ok(); }
	ScopedAStatus deleteAidingData(IGnss::GnssAidingData) override { return ScopedAStatus::ok(); }

	/* no extensions: the framework treats a failed getter as "not supported" */
#define NO_EXT(name, type) \
	ScopedAStatus name(std::shared_ptr<type> *out) override \
	{ \
		*out = nullptr; \
		return ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION); \
	}
	NO_EXT(getExtensionPsds, IGnssPsds)
	NO_EXT(getExtensionGnssConfiguration, IGnssConfiguration)
	NO_EXT(getExtensionGnssMeasurement, IGnssMeasurementInterface)
	NO_EXT(getExtensionGnssPowerIndication, IGnssPowerIndication)
	NO_EXT(getExtensionGnssBatching, IGnssBatching)
	NO_EXT(getExtensionGnssGeofence, IGnssGeofence)
	NO_EXT(getExtensionGnssNavigationMessage, IGnssNavigationMessageInterface)
	NO_EXT(getExtensionAGnss, IAGnss)
	NO_EXT(getExtensionAGnssRil, IAGnssRil)
	NO_EXT(getExtensionGnssDebug, IGnssDebug)
	NO_EXT(getExtensionGnssVisibilityControl, IGnssVisibilityControl)
	NO_EXT(getExtensionGnssAntennaInfo, IGnssAntennaInfo)
	NO_EXT(getExtensionMeasurementCorrections, IMeasurementCorrectionsInterface)
#undef NO_EXT

private:
	std::shared_ptr<IGnssCallback> callback()
	{
		std::lock_guard<std::mutex> l(cb_lock_);
		return cb_;
	}

	void status(IGnssCallback::GnssStatusValue v)
	{
		if (auto cb = callback())
			cb->gnssStatusCb(v);
	}

	/* sleep up to ms, false if stop() was called */
	bool wait(int ms)
	{
		struct pollfd p = { wake_[0], POLLIN, 0 };

		poll(&p, 1, ms);
		return running_;
	}

	int connect_host()
	{
		struct sockaddr_un sa = {};
		int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);

		sa.sun_family = AF_UNIX;
		strncpy(sa.sun_path, kSocket, sizeof(sa.sun_path) - 1);
		if (fd < 0 || connect(fd, (struct sockaddr *)&sa, sizeof(sa))) {
			int e = errno;

			if (fd >= 0)
				::close(fd);
			errno = e;
			return -1;
		}
		return fd;
	}

	void reader()
	{
		int backoff = 1000, warned = 0;

		epoch_ = Epoch();
		last_report_ns_ = 0;
		svs_.clear();
		used_.clear();
		svs_pending_ = false;
		while (running_) {
			int fd = connect_host();

			if (fd < 0) {
				if (!warned++)
					LOGW("%s: %s, retrying", kSocket, strerror(errno));
				if (!wait(backoff))
					break;
				backoff = backoff < 10000 ? backoff * 2 : 10000;
				continue;
			}
			LOGI("connected to %s", kSocket);
			backoff = 1000;
			warned = 0;
			read_stream(fd);
			::close(fd);
		}
	}

	void read_stream(int fd)
	{
		std::string buf;
		char chunk[1024];

		while (running_) {
			struct pollfd p[2] = { { fd, POLLIN, 0 }, { wake_[0], POLLIN, 0 } };

			/* drained: the epoch's sentences come in one write */
			if (poll(p, 2, epoch_.rmc || svs_pending_ ? 0 : -1) == 0) {
				flush_epoch();
				flush_svs();
				continue;
			}
			if (p[1].revents)
				return;
			ssize_t n = read(fd, chunk, sizeof(chunk));
			if (n <= 0) {
				if (n < 0 && errno == EINTR)
					continue;
				LOGW("host stream closed");
				return;
			}
			buf.append(chunk, n);
			size_t nl;
			while ((nl = buf.find('\n')) != std::string::npos) {
				std::string line = buf.substr(0, nl);

				buf.erase(0, nl + 1);
				if (!line.empty() && line.back() == '\r')
					line.pop_back();
				sentence(line);
			}
			if (buf.size() > 4096)
				buf.clear();
		}
	}

	void sentence(const std::string &s)
	{
		if (!nmea_checksum_ok(s))
			return;
		if (nmea_)
			if (auto cb = callback())
				cb->gnssNmeaCb(realtime_ms(), s);

		std::vector<std::string> f = nmea_fields(s);
		if (f.size() < 2 || f[0].size() < 5)
			return;
		std::string type = f[0].substr(2), time = f[1];

		if (type == "GSV") {
			gsv(f);
			return;
		}
		if (type == "GSA") {
			gsa(f);
			return;
		}
		if (type != "GGA" && type != "RMC" && type != "GST")
			return;
		if (time != epoch_.time) {
			flush_epoch();
			epoch_ = Epoch();
			epoch_.time = time;
			epoch_.rx_ns = boottime_ns();
		}
		if (type == "RMC" && f.size() >= 10 && f[2] == "A") {
			if (!nmea_coord(f[3], f[4], &epoch_.lat) ||
			    !nmea_coord(f[5], f[6], &epoch_.lon) ||
			    f[1].size() < 6 || f[9].size() != 6)
				return;
			struct tm tm = {};
			tm.tm_hour = atoi(f[1].substr(0, 2).c_str());
			tm.tm_min = atoi(f[1].substr(2, 2).c_str());
			tm.tm_sec = atoi(f[1].substr(4, 2).c_str());
			tm.tm_mday = atoi(f[9].substr(0, 2).c_str());
			tm.tm_mon = atoi(f[9].substr(2, 2).c_str()) - 1;
			tm.tm_year = atoi(f[9].substr(4, 2).c_str()) + 100;
			double frac = strtod(f[1].c_str() + 6, nullptr);
			epoch_.utc_ms = (int64_t)timegm(&tm) * 1000 + (int64_t)llround(frac * 1000);
			epoch_.has_speed = !f[7].empty();
			epoch_.speed = strtod(f[7].c_str(), nullptr) * 0.514444;
			epoch_.has_track = !f[8].empty();
			epoch_.track = strtod(f[8].c_str(), nullptr);
			epoch_.rmc = true;
		} else if (type == "GGA" && f.size() >= 12 && atoi(f[6].c_str()) > 0) {
			epoch_.hdop = strtod(f[8].c_str(), nullptr);
			/* Android wants WGS84 ellipsoid height: MSL + geoid separation */
			epoch_.has_alt = !f[9].empty();
			epoch_.alt = strtod(f[9].c_str(), nullptr) + strtod(f[11].c_str(), nullptr);
			epoch_.gga = true;
		} else if (type == "GST" && f.size() >= 8 && !f[6].empty() && !f[7].empty()) {
			epoch_.herr = hypot(strtod(f[6].c_str(), nullptr), strtod(f[7].c_str(), nullptr));
			epoch_.gst = epoch_.herr > 0;
		}
	}

	/* $xxGSV,total,num,inview{,id,elev,azim,snr}[,signal] */
	void gsv(const std::vector<std::string> &f)
	{
		if (f.size() < 4)
			return;
		size_t groups = (f.size() - 4) / 4;
		int sig = (f.size() - 4) % 4 == 1 ? strtol(f.back().c_str(), nullptr, 16) : 0;
		svs_pending_ = true;
		for (size_t g = 0; g < groups; g++) {
			const std::string *v = &f[4 + g * 4];
			SvKey k = sv_key(f[0][1], atoi(v[0].c_str()));

			if (v[0].empty() || k.c == GnssConstellationType::UNKNOWN)
				continue;
			IGnssCallback::GnssSvInfo sv;
			sv.svid = k.svid;
			sv.constellation = k.c;
			sv.cN0Dbhz = v[3].empty() ? 0 : strtof(v[3].c_str(), nullptr);
			sv.basebandCN0DbHz = sv.cN0Dbhz;
			sv.elevationDegrees = strtof(v[1].c_str(), nullptr);
			sv.azimuthDegrees = strtof(v[2].c_str(), nullptr);
			sv.svFlag = (int)IGnssCallback::GnssSvFlags::NONE;
			double hz = sig ? carrier_hz(k.c, sig) : 0;
			if (hz > 0) {
				sv.carrierFrequencyHz = (int64_t)hz;
				sv.svFlag |= (int)IGnssCallback::GnssSvFlags::HAS_CARRIER_FREQUENCY;
			}
			svs_.push_back(sv);
		}
	}

	/* $GNGSA,mode,fix,12 x id,pdop,hdop,vdop[,system] */
	void gsa(const std::vector<std::string> &f)
	{
		char sys = f.size() >= 19 ? gsa_sys(atoi(f[18].c_str())) : f[0][1];

		if (f.size() < 15)
			return;
		for (int i = 3; i < 15; i++)
			if (!f[i].empty()) {
				/* GN talker without system ID: guess by the ID range */
				char t = sys == 'N' ? (atoi(f[i].c_str()) > 64 ? 'L' : 'P') : sys;
				SvKey k = sv_key(t, atoi(f[i].c_str()));

				if (k.c != GnssConstellationType::UNKNOWN)
					used_.push_back(k);
			}
	}

	void flush_svs()
	{
		if (!svs_pending_)
			return;
		for (auto &sv : svs_)
			for (auto &u : used_)
				if (u == SvKey{ sv.constellation, sv.svid })
					sv.svFlag |= (int)IGnssCallback::GnssSvFlags::USED_IN_FIX;
		if (sv_status_)
			if (auto cb = callback())
				cb->gnssSvStatusCb(svs_);
		svs_.clear();
		used_.clear();
		svs_pending_ = false;
	}

	void flush_epoch()
	{
		Epoch &e = epoch_;

		if (!e.rmc)
			return;
		e.rmc = false;	/* report each epoch once */
		if (last_report_ns_ &&
		    e.rx_ns - last_report_ns_ < (int64_t)(min_interval_ms_ - 100) * 1000000)
			return;

		GnssLocation loc;
		loc.gnssLocationFlags = GnssLocation::HAS_LAT_LONG;
		loc.latitudeDegrees = e.lat;
		loc.longitudeDegrees = e.lon;
		if (e.gga && e.has_alt) {
			loc.gnssLocationFlags |= GnssLocation::HAS_ALTITUDE;
			loc.altitudeMeters = e.alt;
		}
		if (e.has_speed) {
			loc.gnssLocationFlags |= GnssLocation::HAS_SPEED;
			loc.speedMetersPerSec = e.speed;
		}
		if (e.has_track) {
			loc.gnssLocationFlags |= GnssLocation::HAS_BEARING;
			loc.bearingDegrees = e.track;
		}
		/* the chip's own estimate; without one, HDOP x 5 m (typical UERE) */
		double acc = e.gst ? e.herr : e.gga && e.hdop > 0 ? e.hdop * 5 : 0;
		if (acc > 0) {
			loc.gnssLocationFlags |= GnssLocation::HAS_HORIZONTAL_ACCURACY;
			loc.horizontalAccuracyMeters = acc;
		}
		loc.timestampMillis = e.utc_ms;
		loc.elapsedRealtime.flags = ElapsedRealtime::HAS_TIMESTAMP_NS |
					    ElapsedRealtime::HAS_TIME_UNCERTAINTY_NS;
		loc.elapsedRealtime.timestampNs = e.rx_ns;
		/* fix is computed on the chip, flushed once a second */
		loc.elapsedRealtime.timeUncertaintyNs = 1e9;
		last_report_ns_ = e.rx_ns;
		if (auto cb = callback())
			cb->gnssLocationCb(loc);
	}

	std::mutex cb_lock_, run_lock_;
	std::shared_ptr<IGnssCallback> cb_;
	std::thread thread_;
	std::atomic<bool> running_{ false }, nmea_{ false }, sv_status_{ true };
	std::atomic<int> min_interval_ms_{ 1000 };
	int wake_[2] = { -1, -1 };
	/* reader thread only */
	Epoch epoch_;
	int64_t last_report_ns_ = 0;
	std::vector<IGnssCallback::GnssSvInfo> svs_;
	std::vector<SvKey> used_;
	bool svs_pending_ = false;
};

int main()
{
	ABinderProcess_setThreadPoolMaxThreadCount(1);
	ABinderProcess_startThreadPool();

	auto gnss = ndk::SharedRefBase::make<Gnss>();
	std::string instance = std::string(IGnss::descriptor) + "/default";
	binder_status_t st = AServiceManager_addService(gnss->asBinder().get(), instance.c_str());

	if (st != STATUS_OK) {
		LOGE("addService %s: %d", instance.c_str(), st);
		return 1;
	}
	LOGI("%s registered", instance.c_str());
	ABinderProcess_joinThreadPool();
	return 1;
}
