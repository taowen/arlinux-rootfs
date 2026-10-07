#!/usr/bin/env bash
# Optional: a Geany look that reads well on a phone or in a screen recording.
# Installs the Pocket Dark colour scheme, larger fonts and hides the symbol
# sidebar. Geany rewrites its settings on exit, so close Geany first.
set -euo pipefail
cd "$(dirname "$0")"
if pgrep -x geany >/dev/null; then
    echo "Close Geany first; it overwrites geany.conf when it exits." >&2
    exit 1
fi
config=${XDG_CONFIG_HOME:-$HOME/.config}/geany
mkdir -p "$config/colorschemes"
install -m 644 geany/pocket-dark.conf "$config/colorschemes/pocket-dark.conf"
touch "$config/geany.conf"
grep -q '^\[geany\]' "$config/geany.conf" || printf '[geany]\n' >> "$config/geany.conf"
set_key() {
    if grep -q "^$1=" "$config/geany.conf"; then
        sed -i "s|^$1=.*|$1=$2|" "$config/geany.conf"
    else
        sed -i "/^\[geany\]/a $1=$2" "$config/geany.conf"
    fi
}
set_key color_scheme pocket-dark.conf
set_key editor_font "Monospace 15"
set_key msgwin_font "Monospace 12"
set_key tagbar_font "Sans 12"
set_key sidebar_visible false
echo "Geany: Pocket Dark, Monospace 15. Restore with Preferences or View > Change Color Scheme."
