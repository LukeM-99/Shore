# Shore

Turn a website into a desktop app, as [Pake](https://github.com/tw93/Pake) does, for [Kindling](https://github.com/LukeM-99/Kindling). `shore add notion notion.so` gives Notion its own window, launcher entry, icon and login, kept apart from your browser and from every other web app.

On Linux, Pake is the system's WebKitGTK wrapped in Rust, and it builds a package for each site. Shore is the same WebKitGTK engine with a small C program around it. One 60 KB binary runs every app, so there is no Rust, no Node and no build step per site. Debian's security updates to WebKitGTK reach every app at once.

## Requirements

- Debian 12 (bookworm) or Devuan 5 (daedalus), or newer, with `apt`.
- X11. It has not been tried on Wayland.

## Install

```
git clone https://github.com/LukeM-99/Shore.git shore
cd shore
./install.sh        # then: shore add NAME URL
```

Read `install.sh` before you run it. It runs `sudo apt install` for the build tools and `libwebkit2gtk-4.1-dev`, builds Shore with `make`, and symlinks `~/.local/bin/shore` to the clone. A file already there that is not a symlink is moved to `~/.dotfiles_backup/<date>/` first. Keep the clone where it is.

## Uninstall

```
for a in $(shore list | cut -d' ' -f1); do shore remove $a; done
rm -f ~/.local/bin/shore
```

## Using it

```
shore add notion notion.so            # make a web app
shore add notion --width=1400 --dark  # change one
shore notion                          # run it (or pick Notion in the app launcher)
shore list
shore remove notion                   # removes it, with its cookies and data
shore https://example.com             # a throwaway window that keeps nothing
```

`add` writes `~/.config/shore/NAME.conf` and a launcher entry, `~/.local/share/applications/shore-NAME.desktop`, so the app shows up in rofi. The first time an app runs, the site's favicon becomes its icon, unless you gave one with `--icon`. Some sites offer only a small favicon; `--icon=picture.png` fixes that.

Each app runs once: starting it again brings the open window forward. It reopens at the page, size and zoom it was closed with.

### Options

Pass them to `add` as `--key=value` (just `--key` means `true`), or edit the `.conf` file.

| Key | |
|---|---|
| `title` | Window and launcher name. Defaults to the NAME, capitalised |
| `icon` | An image file, copied in as the app's icon |
| `width`, `height` | First window size (default 1200 × 780). Later runs use the size it was closed at |
| `maximize`, `fullscreen` | Start that way |
| `decorations=false` | No title bar or border |
| `always-on-top` | Keep the window above the others |
| `zoom` | Page zoom in percent (default 100) |
| `dark` | Ask for the GTK theme's dark variant. Sites that follow `prefers-color-scheme` go dark too |
| `user-agent` | Pretend to be another browser |
| `proxy` | `http://`, `https://` or `socks5://` proxy URL |
| `incognito` | Keep no cookies, storage or history |
| `safe-domains` | More domains whose links stay in the app, e.g. `--safe-domains='slack.com;slack-edge.com'` |
| `internal-url-regex` | A regex for the URLs that stay in the app; replaces the domain rules |
| `force-internal` | Keep every link in the app |
| `new-window` | Let the site open real popups, for sign-ins that need them |
| `download-dir` | Where downloads go (default `~/Downloads`) |
| `restore-url=false` | Always start at the app's URL |
| `notifications=false` | Don't allow the site's notifications (they're on by default) |
| `media` | Allow camera and microphone, for calls |
| `inject` | CSS or JS files to add to every page, relative to `~/.config/shore`: `--inject='notion.css;notion.js'` |
| `shortcuts=false` | Leave the keys below to the page (except F11, Ctrl+W and Ctrl+Q) |
| `debug` | Enable the web inspector (Ctrl+Shift+I) |

**Which links stay in the app.** By default these stay: the app's own site and its subdomains (`notion.so` also covers `www.notion.so`), and the safe domains. Other links you click open in your usual browser. So do `mailto:` links and the like. Redirects stay in, so a sign-in that passes through another domain still works.

**Downloads** go to `download-dir`, never over an existing file (`file (1).zip`). A notification says when each one is done.

**Notifications** from the site go through dunst. Clicking one brings the app forward. The app's own site has permission from the start, so it doesn't ask again on every launch.

## Shortcuts

| Key | |
|---|---|
| Alt+← / Alt+→, Ctrl+[ / Ctrl+] | back / forward |
| Ctrl+R, F5 | reload |
| Ctrl+Shift+R | reload, skipping the cache |
| Ctrl+= / Ctrl+- / Ctrl+0 | zoom in / out / reset |
| Ctrl+F | find. Enter or F3 for the next match, Shift+F3 for the previous one, Esc to close |
| Ctrl+L | copy the page's URL |
| Ctrl+Shift+H | go to the app's home page |
| Ctrl+P | print |
| Ctrl+Shift+Delete | clear the cache and reload (you stay signed in) |
| F11 | fullscreen |
| Ctrl+W, Ctrl+Q | close |

## Where things are

| | |
|---|---|
| `~/.config/shore/NAME.conf` | the app's options |
| `~/.local/share/shore/NAME/` | its cookies, storage, icon and saved window state |
| `~/.cache/shore/NAME/` | its web cache |
| `~/.local/share/applications/shore-NAME.desktop` | its launcher entry |

## How it works

- C, GTK3 and WebKitGTK 4.1. About 900 lines in `src/`: `main.c` is the command line and the config, `window.c` the browser window.
- Each app runs as its own GApplication (`shore.app.NAME`), which makes it single-instance. Its WM class is `shore-NAME`, so the taskbar matches it to its launcher entry.
- Each app has its own WebKit data directory, so cookies and logins are never shared between apps or with your browser.

Pake features Shore leaves out:

- **Tray icon.** It may come later.
- **Global activation shortcut.** Bind `shore NAME` to a key in Openbox's `rc.xml` instead. Running it again raises the window.
- **Per-site packages** (`.deb`, AppImage, rpm). Shore needs none.
- **macOS and Windows options.**

## Licence

GPL-3.0, the same as Kindling. See `LICENSE`.
