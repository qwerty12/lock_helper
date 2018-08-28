#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/ioctl.h>
#include <linux/vt.h>
#include <fcntl.h>

#include <X11/Xlib.h>
#include <X11/XKBlib.h>
#include <X11/extensions/XKBrules.h>

#include <glib.h>
#include <glib-unix.h>
#include <glib/gstdio.h>
#include <gio/gio.h>

#include <pulse/pulseaudio.h>
#include <pulse/glib-mainloop.h>

#define SYSRQ_PATH "/proc/sys/kernel/sysrq"
#define MAGIC_TERMINATE_OPTION "terminate:ctrl_alt_bksp"

static uid_t orig_user;
static GMainLoop *loop = NULL;
static GDBusProxy *screensaver_proxy = NULL;
static int term = -1;

static char orig_sysrq[4];
static gboolean modify_sysrq;

static gboolean modify_x11_layout_options;
static gchar *extra_x11_layout_options = NULL;

static gboolean pulse_ready = FALSE;
static pa_glib_mainloop *pa_loop = NULL;
static pa_context *pa_ctx = NULL;
static gchar *default_sink = NULL;

// Fucking GNOME...
static GDBusProxy *gnome_session_main_proxy = NULL;
static GDBusProxy *gnome_session_client_proxy = NULL;

static void deinit_pulse()
{
    if (pa_ctx) {
        pa_context_disconnect(pa_ctx);
        g_clear_pointer(&pa_ctx, pa_context_unref);
    }

    g_clear_pointer(&pa_loop, pa_glib_mainloop_free);
    g_clear_pointer(&default_sink, g_free);
}

static void pa_server_info_callback(pa_context *context, const pa_server_info *i, void *userdata)
{
    if (i->default_sink_name) {
        if (GPOINTER_TO_INT(userdata))
            pa_operation_unref(pa_context_set_sink_mute_by_name(context, i->default_sink_name, 1, NULL, NULL));

        if (!default_sink || g_strcmp0(default_sink, i->default_sink_name)) {
            g_free(default_sink);
            default_sink = g_strdup(i->default_sink_name);
        }
    }
}

static void context_state_callback(pa_context *context, void *userdata G_GNUC_UNUSED)
{
    pulse_ready = pa_context_get_state(context) == PA_CONTEXT_READY;
}

static void mute_sound(gboolean attempt_now)
{
    // Many thanks to https://kdekorte.blogspot.com/2010/11/getting-default-volume-from-pulseaudio.html
    if (pulse_ready) {
        if (attempt_now && default_sink)
            pa_operation_unref(pa_context_set_sink_mute_by_name(pa_ctx, default_sink, 1, NULL, NULL));
        else
            pa_operation_unref(pa_context_get_server_info(pa_ctx, pa_server_info_callback, GINT_TO_POINTER(TRUE)));
    }
}

static void init_pulse()
{
    if (!pa_loop)
        pa_loop = pa_glib_mainloop_new(NULL);

    if (!pa_ctx) {
        if ((pa_ctx = pa_context_new(pa_glib_mainloop_get_api(pa_loop), "LockHelper"))) {
            pa_context_connect(pa_ctx, NULL, PA_CONTEXT_NOAUTOSPAWN, NULL);
            pa_context_set_state_callback(pa_ctx, context_state_callback, NULL);
        }
    }
}

static void gnome_session_unregister();

static void child_setup(gpointer user_data G_GNUC_UNUSED)
{
    if (setuid(orig_user) != 0)
        exit(EXIT_FAILURE);
}

static void gnome_session_all_is_ok()
{
    g_variant_unref(g_dbus_proxy_call_sync(gnome_session_client_proxy, "EndSessionResponse", g_variant_new ("(bs)", TRUE, ""), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL));
}

static void gnome_session_on_signal(GDBusProxy *proxy G_GNUC_UNUSED, gchar *sender_name G_GNUC_UNUSED, gchar *signal_name, GVariant *parameters G_GNUC_UNUSED, gpointer user_data G_GNUC_UNUSED)
{
    if (!g_strcmp0(signal_name, "Stop")) {
        gnome_session_unregister();
        g_main_loop_quit(loop);
    } else if (!g_strcmp0(signal_name, "QueryEndSession")) {
        pa_operation_unref(pa_context_get_server_info(pa_ctx, pa_server_info_callback, GINT_TO_POINTER(FALSE)));
        gnome_session_all_is_ok();
    } else if (!g_strcmp0(signal_name, "EndSession")) {
        gchar *argv[] = { "/home/faheem/bin/xkillall", NULL };
        mute_sound(TRUE);
        g_spawn_sync(NULL, argv, NULL, G_SPAWN_DEFAULT, child_setup, NULL, NULL, NULL, NULL, NULL);
        do
            g_main_context_iteration(NULL, TRUE);
        while (g_main_context_pending(NULL));

        gnome_session_all_is_ok();
        gnome_session_unregister();
        g_main_loop_quit(loop);
    }
}

static void gnome_session_unregister()
{
    gchar *client_id = NULL;

    if (gnome_session_client_proxy) {
        client_id = g_strdup(g_dbus_proxy_get_object_path(gnome_session_client_proxy));
        g_signal_handlers_disconnect_by_func(gnome_session_client_proxy, gnome_session_on_signal, NULL);
        g_clear_object(&gnome_session_client_proxy);
    }

    if (gnome_session_main_proxy) {
        if (client_id)
            g_variant_unref(g_dbus_proxy_call_sync(gnome_session_main_proxy, "UnregisterClient", g_variant_new ("(o)", client_id), G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL));
        g_clear_object(&gnome_session_main_proxy);
    }

    g_free(client_id);
}

void gnome_session_register()
{
    const gchar *id = g_getenv("DESKTOP_AUTOSTART_ID");
    if (!id)
        return;

    gnome_session_main_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES | G_DBUS_PROXY_FLAGS_DO_NOT_CONNECT_SIGNALS | G_DBUS_PROXY_FLAGS_DO_NOT_AUTO_START, NULL, "org.gnome.SessionManager", "/org/gnome/SessionManager", "org.gnome.SessionManager", NULL, NULL);
    if (gnome_session_main_proxy) {
        GVariant *res = g_dbus_proxy_call_sync(gnome_session_main_proxy, "RegisterClient", g_variant_new ("(ss)", "pk.qwerty12.lock_helper", id), G_DBUS_CALL_FLAGS_NO_AUTO_START, -1, NULL, NULL);
        if (res) {
            gchar *client_id = NULL;
            g_variant_get(res, "(o)", &client_id);
            if (client_id) {
                gnome_session_client_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL, "org.gnome.SessionManager", client_id, "org.gnome.SessionManager.ClientPrivate", NULL, NULL);

                if (gnome_session_client_proxy)
                    g_signal_connect(gnome_session_client_proxy, "g-signal", G_CALLBACK(gnome_session_on_signal), NULL);

                g_free(client_id);
            }
            g_variant_unref(res);
        }

        if (!gnome_session_client_proxy)
            g_clear_object(&gnome_session_main_proxy);
    }

    g_unsetenv("DESKTOP_AUTOSTART_ID");
}

static gboolean on_sigint(gpointer data_ptr G_GNUC_UNUSED)
{
    g_main_loop_quit(loop);
    return G_SOURCE_REMOVE;
}

static gboolean read_sysrq()
{
    int fd = g_open(SYSRQ_PATH, O_RDONLY);
    if (fd == -1) {
        perror("Failed to open() " SYSRQ_PATH " for reading");
        return FALSE;
    }

    ssize_t nread = read(fd, orig_sysrq, sizeof(orig_sysrq));
    if (nread == -1) {
        perror("Failed to read() " SYSRQ_PATH);
        g_close(fd, NULL);
        return FALSE;
    }
    g_close(fd, NULL);

    orig_sysrq[nread] = '\0';

    return TRUE;
}

static void write_sysrq(const char *val)
{
    int fd = g_open(SYSRQ_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Failed to open() " SYSRQ_PATH " for writing");
        return;
    }

    if (write(fd, val, strlen(val)) == -1)
        perror("Failed to write() " SYSRQ_PATH);

    g_close(fd, NULL);
}

static gboolean must_we_mess_with_x11s_layout(gchar **extra_options)
{
    gboolean has_terminate_ctrl_alt_bksp = FALSE;
    int major = XkbMajorVersion, minor = XkbMinorVersion;

    Display *dpy = XkbOpenDisplay(NULL, NULL, NULL, &major, &minor, NULL);
    if (dpy) {
        XkbRF_VarDefsRec vd;

        if (XkbRF_GetNamesProp(dpy, NULL, &vd)) {
            if (vd.options) {
                if (!strcmp(vd.options, MAGIC_TERMINATE_OPTION))
                    has_terminate_ctrl_alt_bksp = TRUE;
                else {
                    const size_t mto_len = sizeof(MAGIC_TERMINATE_OPTION) - 1;

                    char *mto_pos;
                    while ((mto_pos = strstr(vd.options, MAGIC_TERMINATE_OPTION))) {
                        size_t mto_len2 = strlen(mto_pos);
                        has_terminate_ctrl_alt_bksp = TRUE;

                        if (mto_len2 > mto_len) {
                            //memset(mto_pos, '\0', mto_len);
                            memmove(mto_pos, mto_pos + mto_len + (mto_pos[mto_len] == ','), mto_len2 - mto_len);
                        } else {
                            if (*(mto_pos - 1) == ',')
                                *(mto_pos - 1) = '\0';
                        }
                    }

                    *extra_options = g_strdup(vd.options);
                }
                free(vd.options);
            }

            if (vd.model)
                free(vd.model);
            if (vd.layout)
                free(vd.layout);
            if (vd.variant)
                free(vd.variant);
        }
        XCloseDisplay(dpy);
    }

    return has_terminate_ctrl_alt_bksp;
}

static void mess_with_x11s_layout(gboolean remove)
{
    pid_t pid = fork();

    if (pid == 0) {
        if (setuid(orig_user) != -1) {
            // Taken from the Mutter source code
            int major = XkbMajorVersion, minor = XkbMinorVersion;

            Display *dpy = XkbOpenDisplay(NULL, NULL, NULL, &major, &minor, NULL);
            if (dpy) {
                XkbRF_VarDefsRec xkb_var_defs;
                char *rules = NULL;

                if (XkbRF_GetNamesProp(dpy, &rules, &xkb_var_defs) && rules) {
                    gchar *rules_file_path = rules[0] != '/' ? g_build_filename(XKB_BASE, "rules", rules, NULL) : g_strdup(rules);
                    g_clear_pointer(&rules, free);

                    if (xkb_var_defs.options)
                        free(xkb_var_defs.options);

                    if (remove)
                        xkb_var_defs.options = g_strdup(extra_x11_layout_options);
                    else
                        xkb_var_defs.options = extra_x11_layout_options ? g_strconcat(MAGIC_TERMINATE_OPTION, ",", extra_x11_layout_options, NULL) : g_strdup(MAGIC_TERMINATE_OPTION);

                    XkbRF_RulesRec *xkb_rules = XkbRF_Load(rules_file_path, NULL, True, True);
                    if (xkb_rules) {
                        XkbComponentNamesRec xkb_comp_names = { 0 };
                        XkbRF_GetComponents(xkb_rules, &xkb_var_defs, &xkb_comp_names);

                        XkbDescRec *xkb_desc = XkbGetKeyboardByName(dpy,
                                                                    XkbUseCoreKbd,
                                                                    &xkb_comp_names,
                                                                    XkbGBN_AllComponentsMask,
                                                                    XkbGBN_AllComponentsMask &
                                                                    (~XkbGBN_GeometryMask), True);

                        if (xkb_desc) {
                            XkbFreeKeyboard(xkb_desc, 0, True);
                            gchar *rules_name = g_path_get_basename(rules_file_path);
                            XkbRF_SetNamesProp(dpy, rules_name, &xkb_var_defs);
                            g_free(rules_name);
                        }

                        if (xkb_comp_names.keymap)
                            free(xkb_comp_names.keymap);
                        if (xkb_comp_names.keycodes)
                            free(xkb_comp_names.keycodes);
                        if (xkb_comp_names.types)
                            free(xkb_comp_names.types);
                        if (xkb_comp_names.compat)
                            free(xkb_comp_names.compat);
                        if (xkb_comp_names.symbols)
                            free(xkb_comp_names.symbols);
                        if (xkb_comp_names.geometry)
                            free(xkb_comp_names.geometry);

                        XkbRF_Free(xkb_rules, True);
                    }

                    g_free(xkb_var_defs.options);
                    g_free(rules_file_path);
                    if (xkb_var_defs.model)
                        free(xkb_var_defs.model);
                    if (xkb_var_defs.layout)
                        free(xkb_var_defs.layout);
                    if (xkb_var_defs.variant)
                        free(xkb_var_defs.variant);
                }

                XCloseDisplay(dpy);
            }
        }
        exit(EXIT_SUCCESS);
    } else if (pid > 0) {
        int status;
        waitpid(pid, &status, 0);
    }
}

static void on_screensaver(GDBusProxy *proxy G_GNUC_UNUSED, gchar *sender_name G_GNUC_UNUSED, gchar *signal_name, GVariant *parameters, gpointer user_data G_GNUC_UNUSED)
{
    if (!g_strcmp0(signal_name, "ActiveChanged")) {
        gboolean locked;
        g_variant_get(parameters, "(b)", &locked);

        if (modify_sysrq)
            write_sysrq(locked ? "0" : orig_sysrq);
        if (modify_x11_layout_options)
            mess_with_x11s_layout(locked);
        if (locked)
            mute_sound(FALSE);
    }
}

static void cleanup()
{
    if (term != -1) {
        g_close(term, NULL);
        term = -1;
    }
    setuid(orig_user);
    seteuid(orig_user);

    gnome_session_unregister();
    deinit_pulse();
    if (screensaver_proxy) {
        g_signal_handlers_disconnect_by_func(screensaver_proxy, on_screensaver, NULL);
        g_clear_object(&screensaver_proxy);
    }
    g_clear_pointer(&loop, g_main_loop_unref);
    g_clear_pointer(&extra_x11_layout_options, g_free);
}

int main()
{
    // Drop privs to connect to user's session bus: thanks, https://stackoverflow.com/a/6732456
    orig_user = getuid();
    if (seteuid(orig_user) == -1) {
        perror("failed to drop privs");
        return EXIT_FAILURE;
    }

    if (!read_sysrq())
        return EXIT_FAILURE;
    modify_sysrq = orig_sysrq[0] != '0';

    modify_x11_layout_options = must_we_mess_with_x11s_layout(&extra_x11_layout_options);

    if (!(screensaver_proxy = g_dbus_proxy_new_for_bus_sync(G_BUS_TYPE_SESSION, G_DBUS_PROXY_FLAGS_DO_NOT_LOAD_PROPERTIES, NULL, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver", "org.freedesktop.ScreenSaver", NULL, NULL))) {
        g_printerr("Failed to connect to Screensaver interface on user's session\n");
        return EXIT_FAILURE;
    }
    g_signal_connect(screensaver_proxy, "g-signal", G_CALLBACK(on_screensaver), NULL);

    loop = g_main_loop_new(NULL, FALSE);
    g_unix_signal_add(SIGINT, on_sigint, NULL);
    g_unix_signal_add(SIGTERM, on_sigint, NULL);

    init_pulse();
    gnome_session_register();

    if (seteuid(0) == -1) {
        perror("Failed to regain root privs");
        return EXIT_FAILURE;
    }
    setuid(0);

    g_main_loop_run(loop);

    cleanup();
    return EXIT_SUCCESS;
}
