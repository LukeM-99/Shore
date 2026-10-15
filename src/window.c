#include "shore.h"
#include <string.h>

static Conf *c;
static GtkApplication *app;
static GtkWidget *win, *find_bar, *find_entry;
static WebKitWebView *view;
static WebKitWebsiteDataManager *dm;
static GRegex *url_re;
static char *home_host, *state_path;

/* ---- which links stay in the app ---- */

static gboolean host_in(const char *host, const char *domain)
{
    size_t h = strlen(host), d = strlen(domain);
    return h >= d && !g_ascii_strcasecmp(host + h - d, domain) && (h == d || host[h - d - 1] == '.');
}

/* The app's own site and its subdomains, the safe-domains, or whatever internal-url-regex
   matches. Other http(s) links, and mailto: and the like, go to the desktop's browser. */
static gboolean internal(const char *uri)
{
    if (g_str_has_prefix(uri, "about:") || g_str_has_prefix(uri, "data:") || g_str_has_prefix(uri, "blob:"))
        return TRUE;
    if (!g_str_has_prefix(uri, "http://") && !g_str_has_prefix(uri, "https://"))
        return FALSE;
    if (c->force_internal)
        return TRUE;
    if (url_re)
        return g_regex_match(url_re, uri, 0, NULL);
    GUri *u = g_uri_parse(uri, G_URI_FLAGS_NONE, NULL);
    const char *host = u ? g_uri_get_host(u) : NULL;
    gboolean in = host && home_host && host_in(host, home_host);
    for (char **d = c->safe_domains; host && !in && d && *d; d++)
        in = **d && host_in(host, *d);
    if (u)
        g_uri_unref(u);
    return in;
}

static void open_outside(const char *uri)
{
    GError *err = NULL;
    if (!g_app_info_launch_default_for_uri(uri, NULL, &err)) {
        g_warning("can't open %s: %s", uri, err->message);
        g_error_free(err);
    }
}

static gboolean on_policy(WebKitWebView *v, WebKitPolicyDecision *d, WebKitPolicyDecisionType type, gpointer u)
{
    if (type == WEBKIT_POLICY_DECISION_TYPE_RESPONSE) {
        /* WebKit drops what it can't show; a browser saves it */
        WebKitResponsePolicyDecision *r = WEBKIT_RESPONSE_POLICY_DECISION(d);
        if (!webkit_response_policy_decision_is_main_frame_main_resource(r) ||
            webkit_response_policy_decision_is_mime_type_supported(r))
            return FALSE;
        webkit_policy_decision_download(d);
        return TRUE;
    }
    WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
    /* Only links the user follows leave the app: redirects, such as a sign-in going through
       another domain and back, stay in. */
    gboolean clicked = type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION ||
                       webkit_navigation_action_get_navigation_type(a) == WEBKIT_NAVIGATION_TYPE_LINK_CLICKED;
    gboolean http = g_str_has_prefix(uri, "http://") || g_str_has_prefix(uri, "https://");
    if (*uri && !internal(uri) && (clicked || !http)) {
        open_outside(uri);
        webkit_policy_decision_ignore(d);
        return TRUE;
    }
    return FALSE;
}

/* ---- popups (new-window) ---- */

static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer u);

static void on_popup_ready(WebKitWebView *pv, gpointer u)
{
    GdkRectangle r = { 0, 0, 0, 0 };
    webkit_window_properties_get_geometry(webkit_web_view_get_window_properties(pv), &r);
    GtkWidget *w = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_transient_for(GTK_WINDOW(w), GTK_WINDOW(win));
    gtk_window_set_default_size(GTK_WINDOW(w), r.width > 0 ? r.width : 800, r.height > 0 ? r.height : 600);
    gtk_container_add(GTK_CONTAINER(w), GTK_WIDGET(pv));
    gtk_application_add_window(app, GTK_WINDOW(w));
    gtk_widget_show_all(w);
}

static void on_popup_close(WebKitWebView *pv, gpointer u)
{
    gtk_widget_destroy(gtk_widget_get_toplevel(GTK_WIDGET(pv)));
}

/* target=_blank and window.open(). Off-site ones were sent out by on_policy; the rest open in
   place, or in a real popup with new-window, for sign-ins that need window.opener. */
static GtkWidget *on_create(WebKitWebView *v, WebKitNavigationAction *a, gpointer u)
{
    if (c->new_window) {
        GtkWidget *pv = webkit_web_view_new_with_related_view(v);
        g_signal_connect(pv, "decide-policy", G_CALLBACK(on_policy), NULL);
        g_signal_connect(pv, "create", G_CALLBACK(on_create), NULL);
        g_signal_connect(pv, "ready-to-show", G_CALLBACK(on_popup_ready), NULL);
        g_signal_connect(pv, "close", G_CALLBACK(on_popup_close), NULL);
        return pv;
    }
    const char *uri = webkit_uri_request_get_uri(webkit_navigation_action_get_request(a));
    if (uri && *uri)
        webkit_web_view_load_uri(view, uri);
    return NULL;
}

/* ---- downloads ---- */

static void notify(const char *title, const char *body)
{
    GNotification *n = g_notification_new(title);
    g_notification_set_body(n, body);
    g_application_send_notification(G_APPLICATION(app), NULL, n);
    g_object_unref(n);
}

static gboolean on_decide_destination(WebKitDownload *dl, const char *suggested, gpointer u)
{
    const char *dir = g_get_user_special_dir(G_USER_DIRECTORY_DOWNLOAD);
    char *custom = NULL;
    if (c->download_dir)
        dir = custom = g_str_has_prefix(c->download_dir, "~/")
                           ? g_build_filename(g_get_home_dir(), c->download_dir + 2, NULL)
                           : g_strdup(c->download_dir);
    if (!dir)
        dir = g_get_home_dir();
    g_mkdir_with_parents(dir, 0755);
    char *base = g_path_get_basename(suggested && *suggested ? suggested : "download");
    char *path = g_build_filename(dir, base, NULL);
    /* never overwrite: name.ext, name (1).ext, ... */
    const char *dot = strrchr(base, '.');
    size_t stem = dot && dot != base ? (size_t)(dot - base) : strlen(base);
    for (int i = 1; g_file_test(path, G_FILE_TEST_EXISTS); i++) {
        g_free(path);
        char *alt = g_strdup_printf("%.*s (%d)%s", (int)stem, base, i, base + stem);
        path = g_build_filename(dir, alt, NULL);
        g_free(alt);
    }
    char *dest = g_filename_to_uri(path, NULL, NULL);
    webkit_download_set_destination(dl, dest);
    g_free(dest);
    g_free(path);
    g_free(base);
    g_free(custom);
    return TRUE;
}

static void on_download_failed(WebKitDownload *dl, GError *err, gpointer u)
{
    g_object_set_data(G_OBJECT(dl), "failed", GINT_TO_POINTER(1));
    if (!g_error_matches(err, WEBKIT_DOWNLOAD_ERROR, WEBKIT_DOWNLOAD_ERROR_CANCELLED_BY_USER))
        notify("Download failed", err->message);
}

static void on_download_finished(WebKitDownload *dl, gpointer u)
{
    if (g_object_get_data(G_OBJECT(dl), "failed"))
        return;
    char *path = g_filename_from_uri(webkit_download_get_destination(dl), NULL, NULL);
    notify("Downloaded", path ? path : webkit_download_get_destination(dl));
    g_free(path);
}

static void on_download(WebKitWebContext *ctx, WebKitDownload *dl, gpointer u)
{
    g_signal_connect(dl, "decide-destination", G_CALLBACK(on_decide_destination), NULL);
    g_signal_connect(dl, "failed", G_CALLBACK(on_download_failed), NULL);
    g_signal_connect(dl, "finished", G_CALLBACK(on_download_finished), NULL);
}

/* ---- permissions and notifications ---- */

static gboolean on_permission(WebKitWebView *v, WebKitPermissionRequest *r, gpointer u)
{
    if ((WEBKIT_IS_NOTIFICATION_PERMISSION_REQUEST(r) && c->notifications) ||
        (WEBKIT_IS_USER_MEDIA_PERMISSION_REQUEST(r) && c->media))
        webkit_permission_request_allow(r);
    else
        webkit_permission_request_deny(r);
    return TRUE;
}

/* Grant the app's own site notifications from the start, so it doesn't ask on every launch. */
static void on_init_notifications(WebKitWebContext *ctx, gpointer u)
{
    if (!c->notifications)
        return;
    GList *allowed = g_list_prepend(NULL, webkit_security_origin_new_for_uri(c->url));
    webkit_web_context_initialize_notification_permissions(ctx, allowed, NULL);
    g_list_free_full(allowed, (GDestroyNotify)webkit_security_origin_unref);
}

static void on_notification_clicked(WebKitNotification *n, gpointer u)
{
    gtk_window_present(GTK_WINDOW(win));
}

static gboolean on_show_notification(WebKitWebView *v, WebKitNotification *n, gpointer u)
{
    g_signal_connect(n, "clicked", G_CALLBACK(on_notification_clicked), NULL);
    return FALSE;  /* WebKit shows it */
}

/* ---- icon ---- */

/* The favicon becomes the app's icon, in the window and in the launcher entry, unless --icon
   gave one. A larger favicon replaces a smaller one. */
static void on_favicon(WebKitWebView *v, GParamSpec *p, gpointer u)
{
    cairo_surface_t *s = webkit_web_view_get_favicon(v);
    if (!s || c->icon || !c->name)
        return;
    char *path = icon_path(c->name);
    int w = cairo_image_surface_get_width(s), old = 0;
    gdk_pixbuf_get_file_info(path, &old, NULL);
    if (w > old && cairo_surface_write_to_png(s, path) == CAIRO_STATUS_SUCCESS) {
        gtk_window_set_icon_from_file(GTK_WINDOW(win), path, NULL);
        write_desktop(c->name, c->title);
    }
    g_free(path);
}

/* ---- find ---- */

static void find(gboolean back)
{
    WebKitFindController *fc = webkit_web_view_get_find_controller(view);
    const char *text = gtk_entry_get_text(GTK_ENTRY(find_entry));
    if (!*text)
        webkit_find_controller_search_finish(fc);
    else if (g_strcmp0(webkit_find_controller_get_search_text(fc), text))
        webkit_find_controller_search(fc, text, WEBKIT_FIND_OPTIONS_CASE_INSENSITIVE | WEBKIT_FIND_OPTIONS_WRAP_AROUND, G_MAXUINT);
    else if (back)
        webkit_find_controller_search_previous(fc);
    else
        webkit_find_controller_search_next(fc);
}

static void on_find_changed(GtkWidget *e, gpointer u) { find(FALSE); }
static void on_find_next(GtkWidget *e, gpointer u) { find(FALSE); }
static void on_find_prev(GtkWidget *e, gpointer u) { find(TRUE); }

static void on_find_mode(GObject *bar, GParamSpec *p, gpointer u)
{
    if (!gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(bar))) {
        webkit_find_controller_search_finish(webkit_web_view_get_find_controller(view));
        gtk_widget_grab_focus(GTK_WIDGET(view));
    }
}

/* ---- shortcuts ---- */

static void zoom_by(double f)
{
    double z = f ? CLAMP(webkit_web_view_get_zoom_level(view) * f, 0.3, 5.0) : c->zoom / 100.0;
    webkit_web_view_set_zoom_level(view, z);
}

static void on_cache_cleared(GObject *o, GAsyncResult *r, gpointer u)
{
    webkit_website_data_manager_clear_finish(dm, r, NULL);
    webkit_web_view_reload_bypass_cache(view);
}

static gboolean on_key(GtkWidget *w, GdkEventKey *e, gpointer u)
{
    guint m = e->state & gtk_accelerator_get_default_mod_mask();
    guint k = gdk_keyval_to_lower(e->keyval);
    gboolean ctrl = m == GDK_CONTROL_MASK, cs = m == (GDK_CONTROL_MASK | GDK_SHIFT_MASK), alt = m == GDK_MOD1_MASK;
    gboolean finding = gtk_search_bar_get_search_mode(GTK_SEARCH_BAR(find_bar));

    if (!m && k == GDK_KEY_F11) {
        GdkWindow *gw = gtk_widget_get_window(win);
        if (gdk_window_get_state(gw) & GDK_WINDOW_STATE_FULLSCREEN)
            gtk_window_unfullscreen(GTK_WINDOW(win));
        else
            gtk_window_fullscreen(GTK_WINDOW(win));
    } else if (ctrl && (k == GDK_KEY_w || k == GDK_KEY_q))
        gtk_window_close(GTK_WINDOW(win));
    else if (!c->shortcuts)
        return FALSE;
    else if ((ctrl && k == GDK_KEY_bracketleft) || (alt && k == GDK_KEY_Left))
        webkit_web_view_go_back(view);
    else if ((ctrl && k == GDK_KEY_bracketright) || (alt && k == GDK_KEY_Right))
        webkit_web_view_go_forward(view);
    else if ((ctrl && k == GDK_KEY_r) || (!m && k == GDK_KEY_F5))
        webkit_web_view_reload(view);
    else if (cs && k == GDK_KEY_r)
        webkit_web_view_reload_bypass_cache(view);
    else if ((ctrl || cs) && (k == GDK_KEY_equal || k == GDK_KEY_plus || k == GDK_KEY_KP_Add))
        zoom_by(1.1);
    else if (ctrl && (k == GDK_KEY_minus || k == GDK_KEY_KP_Subtract))
        zoom_by(1 / 1.1);
    else if (ctrl && (k == GDK_KEY_0 || k == GDK_KEY_KP_0))
        zoom_by(0);
    else if (ctrl && k == GDK_KEY_l)
        gtk_clipboard_set_text(gtk_clipboard_get(GDK_SELECTION_CLIPBOARD), webkit_web_view_get_uri(view), -1);
    else if (cs && k == GDK_KEY_h)
        webkit_web_view_load_uri(view, c->url);
    else if (ctrl && k == GDK_KEY_f) {
        gtk_search_bar_set_search_mode(GTK_SEARCH_BAR(find_bar), TRUE);
        gtk_widget_grab_focus(find_entry);
    } else if (finding && ((!m && k == GDK_KEY_F3) || (ctrl && k == GDK_KEY_g)))
        find(FALSE);
    else if (finding && ((m == GDK_SHIFT_MASK && k == GDK_KEY_F3) || (cs && k == GDK_KEY_g)))
        find(TRUE);
    else if (ctrl && k == GDK_KEY_p)
        webkit_print_operation_run_dialog(webkit_print_operation_new(view), GTK_WINDOW(win));
    else if (cs && k == GDK_KEY_Delete)
        webkit_website_data_manager_clear(dm, WEBKIT_WEBSITE_DATA_MEMORY_CACHE | WEBKIT_WEBSITE_DATA_DISK_CACHE,
                                          0, NULL, on_cache_cleared, NULL);
    else if (cs && k == GDK_KEY_i && c->debug)
        webkit_web_inspector_show(webkit_web_view_get_inspector(view));
    else
        return FALSE;
    return TRUE;
}

/* ---- window state: size, zoom and the last page, kept between runs ---- */

static GKeyFile *state_load(void)
{
    GKeyFile *k = g_key_file_new();
    if (state_path)
        g_key_file_load_from_file(k, state_path, 0, NULL);
    return k;
}

static gboolean on_delete(GtkWidget *w, GdkEvent *e, gpointer u)
{
    if (!state_path)
        return FALSE;
    GKeyFile *k = g_key_file_new();
    GdkWindowState st = gdk_window_get_state(gtk_widget_get_window(win));
    gboolean max = st & GDK_WINDOW_STATE_MAXIMIZED;
    if (!max && !(st & GDK_WINDOW_STATE_FULLSCREEN)) {
        int ww, hh;
        gtk_window_get_size(GTK_WINDOW(win), &ww, &hh);
        g_key_file_set_integer(k, "state", "width", ww);
        g_key_file_set_integer(k, "state", "height", hh);
    } else {  /* keep the last normal size */
        GKeyFile *old = state_load();
        for (const char **key = (const char *[]){ "width", "height", NULL }; *key; key++)
            if (g_key_file_has_key(old, "state", *key, NULL))
                g_key_file_set_integer(k, "state", *key, g_key_file_get_integer(old, "state", *key, NULL));
        g_key_file_unref(old);
    }
    g_key_file_set_boolean(k, "state", "maximized", max);
    g_key_file_set_double(k, "state", "zoom", webkit_web_view_get_zoom_level(view));
    const char *uri = webkit_web_view_get_uri(view);
    if (c->restore_url && !c->incognito && uri && g_str_has_prefix(uri, "http"))
        g_key_file_set_string(k, "state", "url", uri);
    g_key_file_save_to_file(k, state_path, NULL);
    g_key_file_unref(k);
    return FALSE;
}

/* ---- the window ---- */

static void activate(GApplication *a, gpointer u)
{
    if (win) {  /* a second launch brings the running one forward */
        gtk_window_present(GTK_WINDOW(win));
        return;
    }
    if (c->dark)
        g_object_set(gtk_settings_get_default(), "gtk-application-prefer-dark-theme", TRUE, NULL);

    /* Each app keeps its own cookies and storage; incognito and ad hoc windows keep nothing. */
    char *data = NULL, *cache = NULL;
    if (c->name && !c->incognito) {
        data = data_dir(c->name);
        cache = cache_dir(c->name);
        dm = webkit_website_data_manager_new("base-data-directory", data, "base-cache-directory", cache, NULL);
    } else
        dm = webkit_website_data_manager_new_ephemeral();
    if (c->proxy) {
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(c->proxy, NULL);
        webkit_website_data_manager_set_network_proxy_settings(dm, WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
    }
    WebKitWebContext *ctx = webkit_web_context_new_with_website_data_manager(dm);
    if (data) {
        char *cookies = g_build_filename(data, "cookies.sqlite", NULL);
        char *icons = g_build_filename(data, "favicons", NULL);
        webkit_cookie_manager_set_persistent_storage(webkit_website_data_manager_get_cookie_manager(dm), cookies,
                                                     WEBKIT_COOKIE_PERSISTENT_STORAGE_SQLITE);
        webkit_web_context_set_favicon_database_directory(ctx, icons);
        g_free(icons);
        g_free(cookies);
    }
    g_signal_connect(ctx, "download-started", G_CALLBACK(on_download), NULL);
    g_signal_connect(ctx, "initialize-notification-permissions", G_CALLBACK(on_init_notifications), NULL);

    /* inject: CSS and JS files, relative to ~/.config/shore */
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    for (char **f = c->inject; f && *f; f++) {
        char *path = g_path_is_absolute(*f) ? g_strdup(*f) : g_build_filename(g_get_user_config_dir(), "shore", *f, NULL);
        char *src = NULL;
        GError *err = NULL;
        if (!g_file_get_contents(path, &src, NULL, &err)) {
            g_printerr("shore: inject: %s\n", err->message);
            g_error_free(err);
        } else if (g_str_has_suffix(path, ".css")) {
            WebKitUserStyleSheet *ss = webkit_user_style_sheet_new(src, WEBKIT_USER_CONTENT_INJECT_ALL_FRAMES,
                                                                   WEBKIT_USER_STYLE_LEVEL_USER, NULL, NULL);
            webkit_user_content_manager_add_style_sheet(ucm, ss);
            webkit_user_style_sheet_unref(ss);
        } else {
            WebKitUserScript *sc = webkit_user_script_new(src, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME,
                                                          WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
            webkit_user_content_manager_add_script(ucm, sc);
            webkit_user_script_unref(sc);
        }
        g_free(src);
        g_free(path);
    }

    view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", ctx, "user-content-manager", ucm, NULL));
    WebKitSettings *ws = webkit_web_view_get_settings(view);
    if (c->user_agent)
        webkit_settings_set_user_agent(ws, c->user_agent);
    webkit_settings_set_enable_developer_extras(ws, c->debug);
    webkit_settings_set_enable_media_stream(ws, c->media);
    g_signal_connect(view, "decide-policy", G_CALLBACK(on_policy), NULL);
    g_signal_connect(view, "create", G_CALLBACK(on_create), NULL);
    g_signal_connect(view, "permission-request", G_CALLBACK(on_permission), NULL);
    g_signal_connect(view, "show-notification", G_CALLBACK(on_show_notification), NULL);
    g_signal_connect(view, "notify::favicon", G_CALLBACK(on_favicon), NULL);

    win = gtk_application_window_new(GTK_APPLICATION(a));
    if (c->title)
        gtk_window_set_title(GTK_WINDOW(win), c->title);
    else  /* ad hoc: the page's own title */
        g_object_bind_property(view, "title", win, "title", G_BINDING_DEFAULT);
    char *icon = c->name ? icon_path(c->name) : NULL;
    if (icon && g_file_test(icon, G_FILE_TEST_EXISTS))
        gtk_window_set_icon_from_file(GTK_WINDOW(win), icon, NULL);
    g_free(icon);

    GKeyFile *st = state_load();
    int w = g_key_file_get_integer(st, "state", "width", NULL), h = g_key_file_get_integer(st, "state", "height", NULL);
    gtk_window_set_default_size(GTK_WINDOW(win), w > 0 ? w : c->width, h > 0 ? h : c->height);
    if (c->maximize || g_key_file_get_boolean(st, "state", "maximized", NULL))
        gtk_window_maximize(GTK_WINDOW(win));
    if (c->fullscreen)
        gtk_window_fullscreen(GTK_WINDOW(win));
    gtk_window_set_decorated(GTK_WINDOW(win), c->decorations);
    gtk_window_set_keep_above(GTK_WINDOW(win), c->always_on_top);
    double zoom = g_key_file_get_double(st, "state", "zoom", NULL);
    webkit_web_view_set_zoom_level(view, zoom > 0 ? zoom : c->zoom / 100.0);
    char *last = c->restore_url && !c->incognito ? g_key_file_get_string(st, "state", "url", NULL) : NULL;
    g_key_file_unref(st);

    find_entry = gtk_search_entry_new();
    gtk_entry_set_width_chars(GTK_ENTRY(find_entry), 30);
    g_signal_connect(find_entry, "search-changed", G_CALLBACK(on_find_changed), NULL);
    g_signal_connect(find_entry, "activate", G_CALLBACK(on_find_next), NULL);
    g_signal_connect(find_entry, "next-match", G_CALLBACK(on_find_next), NULL);
    g_signal_connect(find_entry, "previous-match", G_CALLBACK(on_find_prev), NULL);
    find_bar = gtk_search_bar_new();
    gtk_search_bar_set_show_close_button(GTK_SEARCH_BAR(find_bar), TRUE);
    gtk_search_bar_connect_entry(GTK_SEARCH_BAR(find_bar), GTK_ENTRY(find_entry));
    gtk_container_add(GTK_CONTAINER(find_bar), find_entry);
    g_signal_connect(find_bar, "notify::search-mode-enabled", G_CALLBACK(on_find_mode), NULL);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_box_pack_start(GTK_BOX(box), GTK_WIDGET(view), TRUE, TRUE, 0);
    gtk_box_pack_end(GTK_BOX(box), find_bar, FALSE, FALSE, 0);
    gtk_container_add(GTK_CONTAINER(win), box);
    g_signal_connect(win, "key-press-event", G_CALLBACK(on_key), NULL);
    g_signal_connect(win, "delete-event", G_CALLBACK(on_delete), NULL);
    gtk_widget_show_all(win);
    gtk_widget_grab_focus(GTK_WIDGET(view));

    webkit_web_view_load_uri(view, last ? last : c->url);
    g_free(last);
    g_free(cache);
    g_free(data);
    g_object_unref(ctx);
}

int run(Conf *conf, char *argv0)
{
    c = conf;
    GUri *u = g_uri_parse(c->url, G_URI_FLAGS_NONE, NULL);
    if (u && g_uri_get_host(u)) {
        const char *h = g_uri_get_host(u);
        home_host = g_strdup(g_str_has_prefix(h, "www.") ? h + 4 : h);
    }
    if (u)
        g_uri_unref(u);
    if (c->url_regex) {
        GError *err = NULL;
        url_re = g_regex_new(c->url_regex, 0, 0, &err);
        if (!url_re) {
            g_printerr("shore: internal-url-regex: %s\n", err->message);
            return 1;
        }
    }

    /* One process per app: launching it again raises the window. The WM class shore-NAME
       matches the launcher entry's StartupWMClass, so the taskbar groups it there. */
    char *id = NULL;
    if (c->name) {
        char *prg = g_strconcat("shore-", c->name, NULL);
        g_set_prgname(prg);
        g_free(prg);
        id = g_strconcat("shore.app.", g_ascii_isdigit(c->name[0]) ? "_" : "", c->name, NULL);
        g_strdelimit(id, "-", '_');
        char *dir = data_dir(c->name);
        g_mkdir_with_parents(dir, 0700);
        state_path = g_build_filename(dir, "state", NULL);
        g_free(dir);
    }
    app = gtk_application_new(id, G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);
    char *args[] = { argv0, NULL };
    int ret = g_application_run(G_APPLICATION(app), 1, args);
    g_object_unref(app);
    g_free(id);
    return ret;
}
