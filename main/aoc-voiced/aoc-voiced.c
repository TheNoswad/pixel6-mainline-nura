// SPDX-License-Identifier: GPL-3.0-or-later
/*
 * aoc-voiced: keep the voice call PCMs of the Google AoC sound card running
 * while ModemManager has a call that needs audio.
 *
 * On Google Tensor phones the modem's call audio is routed inside the AoC
 * audio DSP. The voice endpoints (playback EP5, capture EP4) are "hostless":
 * no samples are ever read or written, but the AoC only opens the phone call
 * path while both PCMs are prepared and started, like the Android audio HAL
 * does with tinyalsa. The mixer routing itself (which amp, which mics) is
 * the "Voice Call" ALSA UCM verb, switched by callaudiod.
 *
 * The voice PCMs are DPCM front ends: they can only be opened once the UCM
 * verb has routed them to a backend (otherwise open fails with EINVAL), and
 * callaudiod may switch the verb before or after ModemManager reports the
 * call. So while a call wants audio, opening is retried on every mixer
 * change of the card.
 *
 * This plays the role q6voiced plays on Qualcomm phones, with separate
 * playback and capture devices and an explicit start.
 *
 *   aoc-voiced [-c CARD] [-p DEV] [-C DEV] [-r RATE]
 */

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <alsa/asoundlib.h>
#include <gio/gio.h>
#include <glib-unix.h>

#define MM_SERVICE      "org.freedesktop.ModemManager1"
#define MM_PATH         "/org/freedesktop/ModemManager1"
#define MM_IFACE_VOICE  "org.freedesktop.ModemManager1.Modem.Voice"
#define MM_IFACE_CALL   "org.freedesktop.ModemManager1.Call"

/* MMCallState */
enum {
	CALL_STATE_UNKNOWN     = 0,
	CALL_STATE_DIALING     = 1,
	CALL_STATE_RINGING_OUT = 2,
	CALL_STATE_RINGING_IN  = 3,
	CALL_STATE_ACTIVE      = 4,
	CALL_STATE_HELD        = 5,
	CALL_STATE_WAITING     = 6,
	CALL_STATE_TERMINATED  = 7,
};

struct voice_pcm {
	const char *name;
	snd_pcm_stream_t stream;
	unsigned int channels;
	snd_pcm_t *pcm;
};

static char *opt_card = "googleaocsndcar";
static int opt_playback = 4;
static int opt_capture = 11;
static unsigned int opt_rate = 48000;

static GDBusConnection *bus;
static GHashTable *calls;	/* object path -> state */
static GMainLoop *loop;
static snd_ctl_t *ctl;
static struct voice_pcm pcms[] = {
	{ "playback", SND_PCM_STREAM_PLAYBACK, 2, NULL },
	{ "capture",  SND_PCM_STREAM_CAPTURE,  1, NULL },
};

/*
 * PCMs
 */

static int pcm_start(struct voice_pcm *v, int device)
{
	snd_pcm_hw_params_t *hw;
	snd_pcm_sw_params_t *sw;
	snd_pcm_uframes_t period = opt_rate / 50, boundary;
	unsigned int rate = opt_rate;
	char name[64];
	int err;

	snprintf(name, sizeof(name), "hw:CARD=%s,DEV=%d", opt_card, device);
	err = snd_pcm_open(&v->pcm, name, v->stream, 0);
	if (err < 0) {
		/* EINVAL: no backend routed yet, retried on the next mixer change */
		if (err == -EINVAL)
			g_debug("%s: %s not routed yet", v->name, name);
		else
			g_warning("%s: open %s: %s", v->name, name, snd_strerror(err));
		v->pcm = NULL;
		return err;
	}

	snd_pcm_hw_params_alloca(&hw);
	snd_pcm_hw_params_any(v->pcm, hw);
	if ((err = snd_pcm_hw_params_set_access(v->pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0 ||
	    (err = snd_pcm_hw_params_set_format(v->pcm, hw, SND_PCM_FORMAT_S16_LE)) < 0 ||
	    (err = snd_pcm_hw_params_set_channels(v->pcm, hw, v->channels)) < 0 ||
	    (err = snd_pcm_hw_params_set_rate_near(v->pcm, hw, &rate, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_period_size_near(v->pcm, hw, &period, NULL)) < 0 ||
	    (err = snd_pcm_hw_params_set_periods(v->pcm, hw, 4, 0)) < 0 ||
	    (err = snd_pcm_hw_params(v->pcm, hw)) < 0) {
		g_warning("%s: hw params: %s", v->name, snd_strerror(err));
		goto fail;
	}

	/* nothing is ever transferred: never stop on an "xrun" */
	snd_pcm_sw_params_alloca(&sw);
	snd_pcm_sw_params_current(v->pcm, sw);
	snd_pcm_sw_params_get_boundary(sw, &boundary);
	snd_pcm_sw_params_set_stop_threshold(v->pcm, sw, boundary);
	if ((err = snd_pcm_sw_params(v->pcm, sw)) < 0) {
		g_warning("%s: sw params: %s", v->name, snd_strerror(err));
		goto fail;
	}

	/* prepare opens the AoC phone call path, start triggers the backends */
	if ((err = snd_pcm_prepare(v->pcm)) < 0 || (err = snd_pcm_start(v->pcm)) < 0) {
		g_warning("%s: start: %s", v->name, snd_strerror(err));
		goto fail;
	}
	g_message("%s %s started (%u Hz, %u ch)", v->name, name, rate, v->channels);
	return 0;

fail:
	snd_pcm_close(v->pcm);
	v->pcm = NULL;
	return err;
}

static void pcm_stop(struct voice_pcm *v)
{
	if (!v->pcm)
		return;
	snd_pcm_drop(v->pcm);
	snd_pcm_close(v->pcm);
	v->pcm = NULL;
	g_message("%s stopped", v->name);
}

static gboolean voice_wanted;

/* open whatever is not running yet */
static void voice_try_start(void)
{
	if (!voice_wanted)
		return;
	if (!pcms[0].pcm)
		pcm_start(&pcms[0], opt_playback);
	if (!pcms[1].pcm)
		pcm_start(&pcms[1], opt_capture);
}

static void voice_set(gboolean on)
{
	if (on == voice_wanted)
		return;
	voice_wanted = on;
	if (on) {
		g_message("call audio wanted");
		voice_try_start();
	} else {
		g_message("call audio off");
		pcm_stop(&pcms[1]);
		pcm_stop(&pcms[0]);
	}
}

static gboolean on_ctl_event(gint fd, GIOCondition cond, gpointer user_data)
{
	snd_ctl_event_t *ev;

	snd_ctl_event_alloca(&ev);
	while (snd_ctl_read(ctl, ev) > 0)
		;
	voice_try_start();
	return G_SOURCE_CONTINUE;
}

static int ctl_watch(void)
{
	struct pollfd pfd;
	char name[64];
	int err;

	snprintf(name, sizeof(name), "hw:CARD=%s", opt_card);
	if ((err = snd_ctl_open(&ctl, name, SND_CTL_NONBLOCK)) < 0) {
		g_printerr("open %s: %s\n", name, snd_strerror(err));
		return err;
	}
	if ((err = snd_ctl_subscribe_events(ctl, 1)) < 0 ||
	    snd_ctl_poll_descriptors(ctl, &pfd, 1) != 1) {
		g_printerr("%s: can't watch mixer events: %s\n", name, snd_strerror(err));
		return err < 0 ? err : -EIO;
	}
	g_unix_fd_add(pfd.fd, G_IO_IN, on_ctl_event, NULL);
	return 0;
}

/*
 * Call tracking
 */

static gboolean state_needs_audio(guint state)
{
	/* incoming calls ring locally; audio starts once answered */
	return state == CALL_STATE_DIALING || state == CALL_STATE_RINGING_OUT ||
	       state == CALL_STATE_ACTIVE || state == CALL_STATE_HELD;
}

static void update(void)
{
	GHashTableIter it;
	gpointer state;
	gboolean need = FALSE;

	g_hash_table_iter_init(&it, calls);
	while (g_hash_table_iter_next(&it, NULL, &state))
		if (state_needs_audio(GPOINTER_TO_UINT(state)))
			need = TRUE;
	voice_set(need);
}

static void call_set_state(const char *path, guint state)
{
	g_debug("%s: state %u", path, state);
	if (state == CALL_STATE_TERMINATED)
		g_hash_table_remove(calls, path);
	else
		g_hash_table_insert(calls, g_strdup(path), GUINT_TO_POINTER(state));
	update();
}

static void call_state_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	g_autofree char *path = user_data;
	g_autoptr(GVariant) ret = NULL;
	g_autoptr(GVariant) value = NULL;
	g_autoptr(GError) error = NULL;

	ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	if (!ret) {
		g_debug("%s: no state: %s", path, error->message);
		return;
	}
	g_variant_get(ret, "(v)", &value);
	if (g_variant_is_of_type(value, G_VARIANT_TYPE_INT32))
		call_set_state(path, (guint)g_variant_get_int32(value));
}

static void call_query(const char *path)
{
	g_dbus_connection_call(bus, MM_SERVICE, path, "org.freedesktop.DBus.Properties", "Get",
			       g_variant_new("(ss)", MM_IFACE_CALL, "State"), G_VARIANT_TYPE("(v)"),
			       G_DBUS_CALL_FLAGS_NONE, -1, NULL, call_state_ready, g_strdup(path));
}

static void on_call_state_changed(GDBusConnection *c, const char *sender, const char *path,
				  const char *iface, const char *signal, GVariant *params,
				  gpointer user_data)
{
	gint32 old_state, new_state;
	guint reason;

	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(iiu)")))
		return;
	g_variant_get(params, "(iiu)", &old_state, &new_state, &reason);
	call_set_state(path, (guint)new_state);
}

static void on_call_deleted(GDBusConnection *c, const char *sender, const char *path,
			    const char *iface, const char *signal, GVariant *params,
			    gpointer user_data)
{
	const char *call;

	if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(o)")))
		return;
	g_variant_get(params, "(&o)", &call);
	if (g_hash_table_remove(calls, call))
		update();
}

static gboolean mm_present;
static void mm_sync(void);

static gboolean mm_sync_retry(gpointer user_data)
{
	if (mm_present)
		mm_sync();
	return G_SOURCE_REMOVE;
}

/* initial sync: every Voice-capable modem's Calls property */
static void managed_objects_ready(GObject *source, GAsyncResult *res, gpointer user_data)
{
	g_autoptr(GVariant) ret = NULL;
	g_autoptr(GVariant) objects = NULL;
	g_autoptr(GError) error = NULL;
	GVariantIter it;
	const char *path;
	GVariant *ifaces;

	ret = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &error);
	if (!ret) {
		/* right after it appears ModemManager may not export its objects yet */
		g_debug("couldn't list modems yet: %s", error->message);
		g_timeout_add_seconds(2, mm_sync_retry, NULL);
		return;
	}
	objects = g_variant_get_child_value(ret, 0);
	g_variant_iter_init(&it, objects);
	while (g_variant_iter_next(&it, "{&o@a{sa{sv}}}", &path, &ifaces)) {
		g_autoptr(GVariant) voice = NULL;
		g_autoptr(GVariant) list = NULL;

		voice = g_variant_lookup_value(ifaces, MM_IFACE_VOICE, G_VARIANT_TYPE("a{sv}"));
		if (voice)
			list = g_variant_lookup_value(voice, "Calls", G_VARIANT_TYPE("ao"));
		if (list) {
			GVariantIter cit;
			const char *call;

			g_variant_iter_init(&cit, list);
			while (g_variant_iter_next(&cit, "&o", &call))
				call_query(call);
		}
		g_variant_unref(ifaces);
	}
}

static void mm_sync(void)
{
	g_dbus_connection_call(bus, MM_SERVICE, MM_PATH, "org.freedesktop.DBus.ObjectManager",
			       "GetManagedObjects", NULL, G_VARIANT_TYPE("(a{oa{sa{sv}}})"),
			       G_DBUS_CALL_FLAGS_NONE, -1, NULL, managed_objects_ready, NULL);
}

static void on_mm_appeared(GDBusConnection *c, const char *name, const char *owner,
			   gpointer user_data)
{
	g_message("ModemManager appeared");
	mm_present = TRUE;
	mm_sync();
}

static void on_mm_vanished(GDBusConnection *c, const char *name, gpointer user_data)
{
	g_message("ModemManager not running");
	mm_present = FALSE;
	g_hash_table_remove_all(calls);
	update();
}

static gboolean on_signal(gpointer user_data)
{
	g_main_loop_quit(loop);
	return G_SOURCE_REMOVE;
}

static void usage(const char *argv0)
{
	fprintf(stderr,
		"Usage: %s [-c CARD] [-p DEV] [-C DEV] [-r RATE]\n"
		"  -c, --card      ALSA card id (default %s)\n"
		"  -p, --playback  voice playback (downlink) device (default %d)\n"
		"  -C, --capture   voice capture (uplink) device (default %d)\n"
		"  -r, --rate      sample rate (default %u)\n",
		argv0, opt_card, opt_playback, opt_capture, opt_rate);
}

int main(int argc, char **argv)
{
	static const struct option longopts[] = {
		{ "card",     required_argument, NULL, 'c' },
		{ "playback", required_argument, NULL, 'p' },
		{ "capture",  required_argument, NULL, 'C' },
		{ "rate",     required_argument, NULL, 'r' },
		{ "help",     no_argument,       NULL, 'h' },
		{ NULL, 0, NULL, 0 },
	};
	g_autoptr(GError) error = NULL;
	int opt;

	while ((opt = getopt_long(argc, argv, "c:p:C:r:h", longopts, NULL)) != -1) {
		switch (opt) {
		case 'c': opt_card = optarg; break;
		case 'p': opt_playback = atoi(optarg); break;
		case 'C': opt_capture = atoi(optarg); break;
		case 'r': opt_rate = strtoul(optarg, NULL, 10); break;
		case 'h': usage(argv[0]); return 0;
		default:  usage(argv[0]); return 2;
		}
	}

	bus = g_bus_get_sync(G_BUS_TYPE_SYSTEM, NULL, &error);
	if (!bus) {
		g_printerr("no system bus: %s\n", error->message);
		return 1;
	}
	if (ctl_watch() < 0)
		return 1;
	calls = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
	loop = g_main_loop_new(NULL, FALSE);

	g_dbus_connection_signal_subscribe(bus, MM_SERVICE, MM_IFACE_CALL, "StateChanged", NULL, NULL,
					   G_DBUS_SIGNAL_FLAGS_NONE, on_call_state_changed, NULL, NULL);
	g_dbus_connection_signal_subscribe(bus, MM_SERVICE, MM_IFACE_VOICE, "CallDeleted", NULL, NULL,
					   G_DBUS_SIGNAL_FLAGS_NONE, on_call_deleted, NULL, NULL);
	g_bus_watch_name_on_connection(bus, MM_SERVICE, G_BUS_NAME_WATCHER_FLAGS_NONE,
				       on_mm_appeared, on_mm_vanished, NULL, NULL);
	g_unix_signal_add(SIGINT, on_signal, NULL);
	g_unix_signal_add(SIGTERM, on_signal, NULL);

	g_message("watching ModemManager calls (card %s, playback %d, capture %d)",
		  opt_card, opt_playback, opt_capture);
	g_main_loop_run(loop);

	voice_set(FALSE);
	g_main_loop_unref(loop);
	g_hash_table_destroy(calls);
	snd_ctl_close(ctl);
	g_object_unref(bus);
	return 0;
}
