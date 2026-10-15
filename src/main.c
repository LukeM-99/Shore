#include "shore.h"
#include <glib/gstdio.h>
#include <stdio.h>
#include <string.h>

/* The keys a .conf may hold; type is b(ool), i(nt), s(tring) or l(ist, ;-separated). */
static const struct { const char *key; char type; } keys[] = {
    { "url", 's' }, { "title", 's' }, { "icon", 's' }, { "width", 'i' }, { "height", 'i' },
    { "maximize", 'b' }, { "fullscreen", 'b' }, { "decorations", 'b' }, { "always-on-top", 'b' },
    { "zoom", 'i' }, { "dark", 'b' }, { "user-agent", 's' }, { "proxy", 's' }, { "incognito", 'b' },
    { "inject", 'l' }, { "safe-domains", 'l' }, { "internal-url-regex", 's' },
    { "force-internal", 'b' }, { "new-window", 'b' }, { "download-dir", 's' }, { "restore-url", 'b' },
    { "notifications", 'b' }, { "media", 'b' }, { "shortcuts", 'b' }, { "debug", 'b' },
};

static const char usage[] =
    "Usage:\n"
    "  shore add NAME URL [--key=value ...]   make a web app (or change one: shore add NAME --key=value)\n"
    "  shore NAME                             run it\n"
    "  shore URL                              open a URL in a throwaway window\n"
    "  shore list                             list your web apps\n"
    "  shore remove NAME                      remove one, with its cookies and data\n"
    "\n"
    "NAME is lowercase letters, digits and dashes. Keys (see the README):\n"
    "  --title=  --icon=FILE  --width=  --height=  --maximize  --fullscreen  --decorations=false\n"
    "  --always-on-top  --zoom=PERCENT  --dark  --user-agent=  --proxy=URL  --incognito\n"
    "  --inject=a.css;b.js  --safe-domains=a.com;b.com  --internal-url-regex=  --force-internal\n"
    "  --new-window  --download-dir=  --restore-url=false  --notifications=false  --media\n"
    "  --shortcuts=false  --debug\n";

/* ---- paths ---- */

char *conf_path(const char *name)
{
    char *base = g_strconcat(name, ".conf", NULL);
    char *p = g_build_filename(g_get_user_config_dir(), "shore", base, NULL);
    g_free(base);
    return p;
}

char *data_dir(const char *name) { return g_build_filename(g_get_user_data_dir(), "shore", name, NULL); }
char *cache_dir(const char *name) { return g_build_filename(g_get_user_cache_dir(), "shore", name, NULL); }
char *icon_path(const char *name) { return g_build_filename(g_get_user_data_dir(), "shore", name, "icon.png", NULL); }

static char *desktop_path(const char *name)
{
    char *base = g_strconcat("shore-", name, ".desktop", NULL);
    char *p = g_build_filename(g_get_user_data_dir(), "applications", base, NULL);
    g_free(base);
    return p;
}

static gboolean valid_name(const char *s)
{
    if (!*s)
        return FALSE;
    for (; *s; s++)
        if (!g_ascii_islower(*s) && !g_ascii_isdigit(*s) && *s != '-')
            return FALSE;
    return TRUE;
}

/* The launcher entry rofi lists. Its icon is the site's favicon once the app has run once. */
void write_desktop(const char *name, const char *title)
{
    GKeyFile *k = g_key_file_new();
    const char *g = G_KEY_FILE_DESKTOP_GROUP;
    char *exec = g_strconcat("shore ", name, NULL), *wmclass = g_strconcat("shore-", name, NULL);
    char *icon = icon_path(name);
    g_key_file_set_string(k, g, "Type", "Application");
    g_key_file_set_string(k, g, "Name", title);
    g_key_file_set_string(k, g, "Comment", "Web app (Shore)");
    g_key_file_set_string(k, g, "Exec", exec);
    g_key_file_set_string(k, g, "Icon", g_file_test(icon, G_FILE_TEST_EXISTS) ? icon : "web-browser");
    g_key_file_set_boolean(k, g, "Terminal", FALSE);
    g_key_file_set_string(k, g, "Categories", "Network;");
    g_key_file_set_string(k, g, "StartupWMClass", wmclass);
    char *path = desktop_path(name), *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0755);
    GError *err = NULL;
    if (!g_key_file_save_to_file(k, path, &err)) {
        g_printerr("shore: %s\n", err->message);
        g_error_free(err);
    }
    g_free(dir);
    g_free(path);
    g_free(icon);
    g_free(wmclass);
    g_free(exec);
    g_key_file_unref(k);
}

/* ---- shore add ---- */

/* --icon=FILE: copied into the app's data dir as a PNG of at most 256px, so the clone or the
   original file can move. */
static gboolean copy_icon(const char *name, const char *file)
{
    GError *err = NULL;
    GdkPixbuf *pb = gdk_pixbuf_new_from_file_at_size(file, 256, 256, &err);
    if (!pb) {
        g_printerr("shore: %s\n", err->message);
        g_error_free(err);
        return FALSE;
    }
    char *dir = data_dir(name), *path = icon_path(name);
    g_mkdir_with_parents(dir, 0700);
    gboolean ok = gdk_pixbuf_save(pb, path, "png", &err, NULL);
    if (!ok) {
        g_printerr("shore: %s\n", err->message);
        g_error_free(err);
    }
    g_free(path);
    g_free(dir);
    g_object_unref(pb);
    return ok;
}

static int cmd_add(int argc, char **argv)
{
    if (argc < 1 || !valid_name(argv[0])) {
        g_printerr("%s", usage);
        return 1;
    }
    const char *name = argv[0];
    char *path = conf_path(name);
    GKeyFile *k = g_key_file_new();
    gboolean existed = g_key_file_load_from_file(k, path, G_KEY_FILE_KEEP_COMMENTS, NULL);
    int ret = 1;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!g_str_has_prefix(a, "--")) {
            char *url = strstr(a, "://") ? g_strdup(a) : g_strconcat("https://", a, NULL);
            g_key_file_set_string(k, "shore", "url", url);
            g_free(url);
            continue;
        }
        const char *eq = strchr(a, '=');
        char *key = eq ? g_strndup(a + 2, eq - a - 2) : g_strdup(a + 2);
        const char *val = eq ? eq + 1 : "true";
        char type = 0;
        for (size_t j = 0; j < G_N_ELEMENTS(keys); j++)
            if (!strcmp(keys[j].key, key))
                type = keys[j].type;
        gboolean bad = !type || (type == 'b' && strcmp(val, "true") && strcmp(val, "false")) ||
                       (type == 'i' && !g_ascii_string_to_signed(val, 10, 1, 100000, NULL, NULL));
        if (bad) {
            g_printerr("shore: %s %s\n", type ? "bad value in" : "unknown option", a);
            g_free(key);
            goto out;
        }
        if (!strcmp(key, "icon")) {
            if (!copy_icon(name, val)) {
                g_free(key);
                goto out;
            }
        }
        g_key_file_set_value(k, "shore", key, val);
        g_free(key);
    }

    char *url = g_key_file_get_string(k, "shore", "url", NULL);
    if (!url) {
        g_printerr("shore: %s needs a URL: shore add %s https://...\n", name, name);
        goto out;
    }
    g_free(url);
    if (!g_key_file_has_key(k, "shore", "title", NULL)) {
        char *title = g_strdup(name);
        title[0] = g_ascii_toupper(title[0]);
        g_key_file_set_string(k, "shore", "title", title);
        g_free(title);
    }

    char *dir = g_path_get_dirname(path);
    g_mkdir_with_parents(dir, 0700);
    g_free(dir);
    GError *err = NULL;
    if (!g_key_file_save_to_file(k, path, &err)) {
        g_printerr("shore: %s\n", err->message);
        g_error_free(err);
        goto out;
    }
    char *title = g_key_file_get_string(k, "shore", "title", NULL);
    write_desktop(name, title);
    g_free(title);
    printf("%s %s. Run it with: shore %s (or from the app launcher)\nSettings: %s\n",
           existed ? "Updated" : "Made", name, name, path);
    ret = 0;
out:
    g_key_file_unref(k);
    g_free(path);
    return ret;
}

/* ---- shore list, shore remove ---- */

static int cmd_list(void)
{
    char *dir = g_build_filename(g_get_user_config_dir(), "shore", NULL);
    GDir *d = g_dir_open(dir, 0, NULL);
    const char *f;
    while (d && (f = g_dir_read_name(d))) {
        if (!g_str_has_suffix(f, ".conf"))
            continue;
        char *path = g_build_filename(dir, f, NULL);
        GKeyFile *k = g_key_file_new();
        char *url = g_key_file_load_from_file(k, path, 0, NULL) ? g_key_file_get_string(k, "shore", "url", NULL) : NULL;
        printf("%-20.*s %s\n", (int)(strlen(f) - 5), f, url ? url : "(no url)");
        g_free(url);
        g_key_file_unref(k);
        g_free(path);
    }
    if (d)
        g_dir_close(d);
    g_free(dir);
    return 0;
}

static int cmd_remove(int argc, char **argv)
{
    if (argc != 1 || !valid_name(argv[0])) {
        g_printerr("%s", usage);
        return 1;
    }
    char *conf = conf_path(argv[0]);
    if (!g_file_test(conf, G_FILE_TEST_EXISTS)) {
        g_printerr("shore: no web app called %s (shore list shows them)\n", argv[0]);
        g_free(conf);
        return 1;
    }
    char *desktop = desktop_path(argv[0]), *data = data_dir(argv[0]), *cache = cache_dir(argv[0]);
    g_unlink(conf);
    g_unlink(desktop);
    char *rm[] = { "rm", "-rf", "--", data, cache, NULL };
    g_spawn_sync(NULL, rm, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    printf("Removed %s, its launcher entry, cookies and data.\n", argv[0]);
    g_free(cache);
    g_free(data);
    g_free(desktop);
    g_free(conf);
    return 0;
}

/* ---- shore NAME ---- */

static gboolean get_bool(GKeyFile *k, const char *key, gboolean def)
{
    GError *err = NULL;
    gboolean v = g_key_file_get_boolean(k, "shore", key, &err);
    if (err)
        g_error_free(err);
    return err ? def : v;
}

static int get_int(GKeyFile *k, const char *key, int def)
{
    GError *err = NULL;
    int v = g_key_file_get_integer(k, "shore", key, &err);
    if (err)
        g_error_free(err);
    return err || v < 1 ? def : v;
}

static gboolean load(const char *name, Conf *c)
{
    char *path = conf_path(name);
    GKeyFile *k = g_key_file_new();
    GError *err = NULL;
    if (!g_key_file_load_from_file(k, path, 0, &err)) {
        if (g_error_matches(err, G_FILE_ERROR, G_FILE_ERROR_NOENT))
            g_printerr("shore: no web app called %s. Make it with: shore add %s URL\n", name, name);
        else
            g_printerr("shore: %s: %s\n", path, err->message);
        g_error_free(err);
        g_free(path);
        return FALSE;
    }
    g_free(path);
    c->name = g_strdup(name);
    c->url = g_key_file_get_string(k, "shore", "url", NULL);
    c->title = g_key_file_get_string(k, "shore", "title", NULL);
    c->icon = g_key_file_get_string(k, "shore", "icon", NULL);
    c->user_agent = g_key_file_get_string(k, "shore", "user-agent", NULL);
    c->proxy = g_key_file_get_string(k, "shore", "proxy", NULL);
    c->url_regex = g_key_file_get_string(k, "shore", "internal-url-regex", NULL);
    c->download_dir = g_key_file_get_string(k, "shore", "download-dir", NULL);
    c->inject = g_key_file_get_string_list(k, "shore", "inject", NULL, NULL);
    c->safe_domains = g_key_file_get_string_list(k, "shore", "safe-domains", NULL, NULL);
    c->width = get_int(k, "width", 1200);
    c->height = get_int(k, "height", 780);
    c->zoom = get_int(k, "zoom", 100);
    c->maximize = get_bool(k, "maximize", FALSE);
    c->fullscreen = get_bool(k, "fullscreen", FALSE);
    c->decorations = get_bool(k, "decorations", TRUE);
    c->always_on_top = get_bool(k, "always-on-top", FALSE);
    c->dark = get_bool(k, "dark", FALSE);
    c->incognito = get_bool(k, "incognito", FALSE);
    c->force_internal = get_bool(k, "force-internal", FALSE);
    c->new_window = get_bool(k, "new-window", FALSE);
    c->restore_url = get_bool(k, "restore-url", TRUE);
    c->notifications = get_bool(k, "notifications", TRUE);
    c->media = get_bool(k, "media", FALSE);
    c->shortcuts = get_bool(k, "shortcuts", TRUE);
    c->debug = get_bool(k, "debug", FALSE);
    g_key_file_unref(k);
    if (!c->url) {
        g_printerr("shore: %s has no url. Set one with: shore add %s URL\n", name, name);
        return FALSE;
    }
    return TRUE;
}

int main(int argc, char **argv)
{
    if (argc < 2 || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        fputs(usage, argc < 2 ? stderr : stdout);
        return argc < 2;
    }
    if (!strcmp(argv[1], "add"))
        return cmd_add(argc - 2, argv + 2);
    if (!strcmp(argv[1], "list"))
        return cmd_list();
    if (!strcmp(argv[1], "remove"))
        return cmd_remove(argc - 2, argv + 2);

    Conf c = { .width = 1200, .height = 780, .zoom = 100, .decorations = TRUE, .incognito = TRUE,
               .shortcuts = TRUE };
    if (strstr(argv[1], "://"))
        c.url = argv[1];
    else if (!valid_name(argv[1])) {
        g_printerr("%s", usage);
        return 1;
    } else if (!load(argv[1], &c))
        return 1;
    return run(&c, argv[0]);
}
