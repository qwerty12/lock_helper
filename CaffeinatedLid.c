// cc -Wall -O2 -s `pkg-config --cflags --libs gio-unix-2.0 gtk+-3.0 appindicator3-0.1` CaffeinatedLid.c -o InhibitLidClose

/*
	This holds a systemd handle-lid-switch inhibitor to ensure systemd doesn't obey HandleLidSwitch=suspend

	When another instance of this program is started, or the AC adapter is plugged in, this program automatically disables itself
*/

#include <glib.h>
#include <glib/gstdio.h>
#include <glib-unix.h>
#include <gio/gio.h>
#include <gio/gunixfdlist.h>

#include <gtk/gtk.h>
#include <libappindicator/app-indicator.h>

static GApplication *app = NULL;
static GDBusProxy *upower_proxy = NULL;
static GDBusProxy *logind_proxy = NULL;
static gint logind_fd = 0;
static AppIndicator *indicator = NULL;
static GtkWidget *start_menu_item = NULL;

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

    if (start_menu_item) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(start_menu_item), "Stop");
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
    }

    g_debug("opened logind fd %i", logind_fd);
}

static void pk_engine_uninhibit()
{
    if (logind_fd == 0)
		return;
	g_close(logind_fd, NULL);
	logind_fd = 0;

    if (start_menu_item) {
        gtk_menu_item_set_label(GTK_MENU_ITEM(start_menu_item), "Start");
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);
    }

    g_debug("closed logind fd %i", logind_fd);
}

static void pk_engine_toggleinhibition(GtkMenuItem *menuitem G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    if (logind_fd == 0)
        pk_engine_inhibit();
    else
        pk_engine_uninhibit();
}

static void on_ac_connected(GDBusProxy *proxy G_GNUC_UNUSED, GVariant *changed_properties, GStrv invalidated_properties G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED) {
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
        pk_engine_uninhibit();
        return;
    }
}

static void upower_init()
{
    upower_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "org.freedesktop.UPower", "/org/freedesktop/UPower", "org.freedesktop.UPower", NULL, NULL);
    g_signal_connect(upower_proxy, "g-properties-changed", G_CALLBACK(on_ac_connected), NULL);
}

static void indicator_init()
{
    GtkWidget *menu = gtk_menu_new();
    GtkWidget *menu_item;

    menu_item = start_menu_item = gtk_menu_item_new_with_label("Start");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
    g_object_add_weak_pointer(G_OBJECT(start_menu_item), (gpointer*)&start_menu_item);
    g_signal_connect(menu_item, "activate", G_CALLBACK(pk_engine_toggleinhibition), NULL);

    menu_item = gtk_separator_menu_item_new ();
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);

    menu_item = gtk_menu_item_new_with_label("Exit");
    gtk_menu_shell_append(GTK_MENU_SHELL(menu), menu_item);
    g_signal_connect_swapped(menu_item, "activate", G_CALLBACK(g_application_quit), app);

    indicator = app_indicator_new("indicator-caffeinatedlid", "my-caffeine-off-symbolic", APP_INDICATOR_CATEGORY_SYSTEM_SERVICES);
    app_indicator_set_attention_icon(indicator, "caffeine-cup-full");
    app_indicator_set_title(indicator, "CaffeinatedLid");
    app_indicator_set_secondary_activate_target(indicator, start_menu_item);
    app_indicator_set_menu(indicator, GTK_MENU(menu));
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ACTIVE);

    gtk_widget_show_all(menu);
}

static void cleanup()
{
    if (indicator) {
        app_indicator_set_status(indicator, APP_INDICATOR_STATUS_PASSIVE);
        g_clear_object(&indicator);
    }
    pk_engine_uninhibit();
    g_clear_object(&logind_proxy);
    if (upower_proxy) {
        g_signal_handlers_disconnect_by_func(upower_proxy, on_ac_connected, NULL);
        g_clear_object(&upower_proxy);
    }
    g_clear_object(&app);
}

static gboolean on_sigint(gpointer data_ptr G_GNUC_UNUSED)
{
    g_application_quit(app);
    return G_SOURCE_REMOVE;
}

static void activate(GApplication *app, gpointer user_data G_GNUC_UNUSED)
{
    static gboolean already_activated = FALSE;
    if (!already_activated)
        already_activated = TRUE;
    else {
        pk_engine_toggleinhibition(NULL, NULL);
        return;
    }

    if (!(logind_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SYSTEM, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS, NULL, "org.freedesktop.login1", "/org/freedesktop/login1", "org.freedesktop.login1.Manager", NULL, NULL))) {
        g_printerr("Failed to obtain logind proxy\n");
        return;
    }

    upower_init();
    indicator_init();

    g_application_hold(app);
    g_unix_signal_add(SIGTERM, on_sigint, NULL);
    g_unix_signal_add(SIGINT, on_sigint, NULL);
}

int main()
{
    gint status;

    app = G_APPLICATION (gtk_application_new("pk.qwerty12.CaffeinatedLid", G_APPLICATION_FLAGS_NONE));
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    status = g_application_run(app, 0, NULL);

    if (g_application_get_is_remote(app))
        g_object_unref(app);
    else
        cleanup();

    return status;
}
