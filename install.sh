#!/bin/sh
# install.sh: apt deps, build, then link Shore into ~/.local/bin.
set -e
cd "$(dirname "$(readlink -f "$0")")"

sudo apt update  # stale security indexes 404 on superseded packages
sudo apt install -y build-essential pkg-config libwebkit2gtk-4.1-dev

make

# A symlink into ~ (as Ember does): keep the clone where it is. Anything already there that
# isn't a symlink goes to ~/.dotfiles_backup/<date>/ first.
backup=~/.dotfiles_backup/$(date +%F_%H%M%S)
link() {
  if [ -e "$2" ] && [ ! -L "$2" ]; then mkdir -p "$backup"; mv "$2" "$backup/"; fi
  mkdir -p "$(dirname "$2")"
  ln -sfn "$1" "$2"
}
link "$PWD/shore" ~/.local/bin/shore

echo "Done. Make a web app with: shore add NAME URL (then start it from the app launcher, or run: shore NAME)"
