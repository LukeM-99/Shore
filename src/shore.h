#pragma once
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

/* One web app, from ~/.config/shore/<name>.conf. Ad hoc windows (shore URL) have no name. */
typedef struct {
    char *name, *title, *url, *icon, *user_agent, *proxy, *url_regex, *download_dir;
    char **inject, **safe_domains;
    int width, height, zoom;
    gboolean maximize, fullscreen, decorations, always_on_top, dark, incognito, force_internal,
        new_window, restore_url, notifications, media, shortcuts, debug;
} Conf;

char *conf_path(const char *name);
char *data_dir(const char *name);
char *cache_dir(const char *name);
char *icon_path(const char *name);
void write_desktop(const char *name, const char *title);

int run(Conf *c, char *argv0);
