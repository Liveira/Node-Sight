#!/bin/sh
# Create a per-user launcher or explicit login autostart entry. No sudo is used.
set -eu
mode=${1:---desktop}
project=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
binary=${2:-"$project/build/NodeSight"}
case "$mode" in
    --desktop) folder=${XDG_DATA_HOME:-"$HOME/.local/share"}/applications ;;
    --autostart|--remove-autostart) folder=${XDG_CONFIG_HOME:-"$HOME/.config"}/autostart ;;
    *) printf '%s\n' 'Usage: linux-shortcut.sh [--desktop|--autostart|--remove-autostart] [/absolute/path/NodeSight]' >&2; exit 1 ;;
esac
target=$folder/node-sight.desktop
if [ "$mode" = --remove-autostart ]; then
    # This exact user-level file is owned by this helper; no directory is removed.
    [ ! -f "$target" ] || rm -- "$target"
    exit 0
fi
[ -x "$binary" ] || { printf '%s\n' 'Build Node-Sight first, or supply its executable path.' >&2; exit 1; }
binary=$(readlink -f -- "$binary")
# Desktop-entry Exec escaping is separate from shell quoting; never evaluate user text.
escaped=$(printf '%s' "$binary" | sed 's/\\/\\\\/g; s/"/\\"/g; s/%/%%/g')
mkdir -p -- "$folder"
printf '[Desktop Entry]\nType=Application\nName=Node-Sight\nComment=Native multi-server resource widget\nExec="%s"\nIcon=utilities-system-monitor\nTerminal=false\nCategories=System;Monitor;\nX-GNOME-Autostart-enabled=true\n' "$escaped" > "$target"
chmod 600 -- "$target"
printf '%s\n' "$target"
