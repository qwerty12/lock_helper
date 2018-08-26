// cc -Wall -O2 -s `pkg-config --cflags --libs gio-unix-2.0` CaffeinatedLid.c -o InhibitLidClose

/*
	This hack shuts down KDE's PowerDevil so that closing my laptop's lid without the charger plugged in doesn't put the laptop to sleep.
	Sadly, PD ignores any inhibitors (its own, as per Caffeine; and systemd-logind's lid ones unless held as root). Changing PD's settings is a route I wanted to avoid

	This:
		* shuts down PD via D-Bus
		* holds a systemd handle-lid-switch inhibitor to ensure systemd doesn't obey HandleLidSwitch=suspend
		* starts the cbatticon battery tray icon program - KDE's one disappears when PD is gone
		* locks the computer when the lid is closed (since PD can't do that anymore)

	When another instance of this program is started, or the AC adapter plugged in, this program automatically quits and starts PD again
	cbatticon is configured to run this program when its tray icon is left-clicked
*/

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

static GApplication *app = NULL;
static GDBusProxy *upower_proxy = NULL;
static GDBusProxy *logind_proxy = NULL;
static gint logind_fd = 0;

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
    GVariant *v;
    GVariantDict dict;

    g_variant_dict_init(&dict, changed_properties);

    if (g_variant_dict_contains(&dict, "OnBattery")) {
        v = g_variant_dict_lookup_value(&dict, "OnBattery", G_VARIANT_TYPE_BOOLEAN);
        ac_connected = !g_variant_get_boolean(v);
        g_variant_unref(v);
    }

    if (ac_connected) {
        g_application_quit(app);
        return;
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

    pk_engine_inhibit();

    g_application_hold(application);
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigint, NULL);
}

static void cleanup()
{
    pk_engine_uninhibit();
    g_clear_object(&logind_proxy);
    g_clear_object(&upower_proxy);
    g_clear_object(&app);
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
