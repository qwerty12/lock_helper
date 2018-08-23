// cc -Wall -O2 -s `pkg-config --cflags --libs gio-unix-2.0 libpulse-mainloop-glib` CaffeinatedLid.c -o InhibitLidClose

/*
	This hack shuts down KDE's PowerDevil so that closing my laptop's lid without the charger plugged in doesn't put the laptop to sleep.
	Sadly, PD ignores any inhibitors (its own, as per Caffeine; and systemd-logind's lid ones unless held as root). Changing PD's settings is a route I wanted to avoid

	This:
		* shuts down PD via D-Bus
		* holds a systemd handle-lid-switch inhibitor to ensure systemd doesn't obey HandleLidSwitch=suspend
		* starts the cbatticon battery tray icon program - KDE's one disappears when PD is gone
		* mutes the sound via PulseAudio and locks the computer when the lid is closed (since PD can't do that anymore)

	When another instance of this program is started, or the AC adapter plugged in, this program automatically quits and starts PD again
	cbatticon is configured to run this program when its tray icon is left-clicked
*/

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

static GApplication *app = NULL;
static GDBusProxy *upower_proxy = NULL;
static GDBusProxy *logind_proxy = NULL;
static gint logind_fd = 0;
static GSubprocess *cbatticon_subprocess = NULL;

static gboolean pulse_ready = FALSE;
static pa_glib_mainloop *pa_loop = NULL;
static pa_context *pa_ctx = NULL;

static void pa_server_info_callback(pa_context *context, const pa_server_info *i, void *userdata G_GNUC_UNUSED)
{
	if (i->default_sink_name)
	    pa_operation_unref(pa_context_set_sink_mute_by_name(context, i->default_sink_name, 1, NULL, NULL));
}

static void context_state_callback(pa_context *context, void *userdata G_GNUC_UNUSED)
{
	pulse_ready = pa_context_get_state(context) == PA_CONTEXT_READY;
}

static void deinit_pulse()
{
	if (pa_ctx) {
		pa_context_disconnect(pa_ctx);
		g_clear_pointer(&pa_ctx, pa_context_unref);
	}

	g_clear_pointer(&pa_loop, pa_glib_mainloop_free);
}

static void mute_sound()
{
	// Many thanks to https://kdekorte.blogspot.com/2010/11/getting-default-volume-from-pulseaudio.html
	if (pulse_ready)
		pa_operation_unref(pa_context_get_server_info(pa_ctx, pa_server_info_callback, NULL));
}

static void init_pulse()
{
	if (!pa_loop)
		pa_loop = pa_glib_mainloop_new(NULL);

	if (!pa_ctx) {
    	if ((pa_ctx = pa_context_new(pa_glib_mainloop_get_api(pa_loop), g_application_get_application_id(app)))) {
	    	pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);
	    	pa_context_set_state_callback(pa_ctx, context_state_callback, NULL);
    	}
    }
}

static void cbatticon_start()
{
    if (cbatticon_subprocess)
        return;

    gchar *me = g_file_read_link("/proc/self/exe", NULL);
    cbatticon_subprocess = g_subprocess_new(G_SUBPROCESS_FLAGS_STDOUT_SILENCE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, NULL, "/usr/bin/cbatticon", "-n", me ? "-x" : NULL, me ? me : NULL, NULL);
    g_free(me);
}

static void cbatticon_close()
{
    if (!cbatticon_subprocess)
        return;

    g_subprocess_send_signal(cbatticon_subprocess, SIGTERM);
    g_clear_pointer(&cbatticon_subprocess, g_object_unref);
}

static gboolean powerdevil_running()
{
    gboolean ret = FALSE;
    g_autoptr(GDBusProxy) dbus_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "org.freedesktop.DBus", "/", "org.freedesktop.DBus", NULL, NULL);
    g_autoptr(GVariant) res = NULL;

    if (dbus_proxy)
        if ((res = g_dbus_proxy_call_sync(dbus_proxy, "NameHasOwner", g_variant_new("(s)", "local.org_kde_powerdevil"), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL)))
            g_variant_get(res, "(b)", &ret);

    return ret;
}

static void child_setup(gpointer user_data G_GNUC_UNUSED)
{
    setsid();
    setpgid(0, 0);
}

static void powerdevil_start()
{
    if (powerdevil_running())
        return;

    gchar *argv[] = { "/usr/lib/org_kde_powerdevil", NULL };
    g_spawn_async(g_get_home_dir(), argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD, child_setup, NULL, NULL, NULL);
}

static void powerdevil_close()
{
    if (!powerdevil_running())
        return;

    GDBusProxy *powerdevil_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "local.org_kde_powerdevil", "/MainApplication", "org.qtproject.Qt.QCoreApplication", NULL, NULL);
    if (!powerdevil_proxy) {
        g_printerr("Failed to obtain PowerDevil proxy\n");
        return;
    }

    g_variant_unref(g_dbus_proxy_call_sync(powerdevil_proxy, "quit", NULL, G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL));
    g_object_unref(powerdevil_proxy);
}

static void lock_originating_session()
{
    static GDBusProxy *this_session = NULL;
    if (!this_session)
        this_session = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "org.freedesktop.login1", "/org/freedesktop/login1/session/self", "org.freedesktop.login1.Session", NULL, NULL);

    g_variant_unref(g_dbus_proxy_call_sync(this_session, "Lock", NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL));
}

// Stolen from the PackageKit source
static void pk_engine_inhibit()
{
    g_autoptr(GError) error = NULL;
	g_autoptr(GUnixFDList) out_fd_list = NULL;
	g_autoptr(GVariant) res = NULL;

	/* already inhibited */
	if (logind_fd != 0)
		return;

	/* block suspend */
	res = g_dbus_proxy_call_with_unix_fd_list_sync(logind_proxy,
							"Inhibit",
							g_variant_new ("(ssss)",
								       "handle-lid-switch:sleep",
								       g_application_get_application_id(app),
								       "Prevent suspend on lid close when on AC",
								       "block"),
							G_DBUS_CALL_FLAGS_NONE,
							-1,
							NULL, /* fd_list */
							&out_fd_list,
							NULL, /* GCancellable */
							&error);

	if (res == NULL) {
		g_warning("Failed to Inhibit using logind: %s", error->message);
		return;
	}

	/* keep fd as cookie */
	if (g_unix_fd_list_get_length(out_fd_list) != 1) {
		g_warning("invalid response from logind");
		return;
	}
	logind_fd = g_unix_fd_list_get(out_fd_list, 0, NULL);
	g_debug("opened logind fd %i", logind_fd);
}

static void pk_engine_uninhibit()
{
    if (logind_fd == 0)
		return;
	g_debug("closed logind fd %i", logind_fd);
	g_close(logind_fd, NULL);
	logind_fd = 0;
}

static void lid_closed_or_ac_connected(GDBusProxy *proxy G_GNUC_UNUSED, GVariant *changed_properties, GStrv invalidated_properties G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
    gboolean ac_connected = FALSE;
    gboolean lid_closed = FALSE;
    GVariant *v;
    GVariantDict dict;

    g_variant_dict_init(&dict, changed_properties);

    if (g_variant_dict_contains(&dict, "OnBattery")) {
        v = g_variant_dict_lookup_value(&dict, "OnBattery", G_VARIANT_TYPE_BOOLEAN);
        ac_connected = !g_variant_get_boolean(v);
        g_variant_unref(v);
    }

    if (g_variant_dict_contains(&dict, "LidIsClosed")) {
        v = g_variant_dict_lookup_value(&dict, "LidIsClosed", G_VARIANT_TYPE_BOOLEAN);
        lid_closed = g_variant_get_boolean(v);
        g_variant_unref(v);
    }

    if (ac_connected) {
        g_application_quit(app);
        return;
    }

    if (lid_closed) {
        lock_originating_session();
        mute_sound();
    }
}

static gboolean upower_init()
{
    upower_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "org.freedesktop.UPower", "/org/freedesktop/UPower", "org.freedesktop.UPower", NULL, NULL);
    g_return_val_if_fail(upower_proxy != NULL, FALSE);
    g_signal_connect(upower_proxy, "g-properties-changed", G_CALLBACK(lid_closed_or_ac_connected), NULL);
    return TRUE;
}

static gboolean on_sigint(gpointer data_ptr G_GNUC_UNUSED)
{
    g_application_quit(app);
    return G_SOURCE_REMOVE;
}

static void activate(GApplication *application, gpointer user_data G_GNUC_UNUSED)
{
    static gboolean already_activated = FALSE;
    if (!already_activated)
        already_activated = TRUE;
    else {
        g_application_quit(application);
        return;
    }

    if (!upower_init())
        return;

    if (!(logind_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", NULL, NULL))) {
        g_printerr("Failed to obtain logind proxy\n");
        return;
    }

    powerdevil_close();
    pk_engine_inhibit();
    cbatticon_start();
    init_pulse();

    g_application_hold(application);
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigint, NULL);
}

static void cleanup()
{
    cbatticon_close();

    deinit_pulse();
    pk_engine_uninhibit();
    g_clear_pointer(&logind_proxy, g_object_unref);
    g_clear_pointer(&upower_proxy, g_object_unref);
    g_clear_pointer(&app, g_object_unref);

    powerdevil_start();
}

int main()
{
    gint status;

    app = g_application_new("pk.qwerty12.CaffeinatedLid", G_APPLICATION_FLAGS_NONE);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    status = g_application_run(app, 0, NULL);

    if (g_application_get_is_remote(app))
        g_object_unref(app);
    else
        cleanup();

    return status;
}
