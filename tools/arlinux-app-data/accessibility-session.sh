#!/bin/sh
# Publish the AT-SPI bus before starting the desktop application.
set -eu

if command -v dbus-send >/dev/null 2>&1; then
    tries=0
    address=
    while [ -z "$address" ] && [ "$tries" -lt 50 ]; do
        address=$(dbus-send --session --print-reply --dest=org.a11y.Bus \
            /org/a11y/bus org.a11y.Bus.GetAddress 2>/dev/null |
            sed -n 's/^[[:space:]]*string "\(.*\)"$/\1/p' | head -n 1) || true
        [ -n "$address" ] || sleep 0.1
        tries=$((tries + 1))
    done
    if [ -n "${address:-}" ] && [ -n "${DISPLAY:-}" ] && command -v xprop >/dev/null 2>&1; then
        tries=0
        until xprop -root -f AT_SPI_BUS 8s -set AT_SPI_BUS "$address" >/dev/null 2>&1; do
            tries=$((tries + 1))
            [ "$tries" -ge 50 ] && break
            sleep 0.1
        done
    fi
    dbus-send --session --dest=org.a11y.Bus /org/a11y/bus \
        org.freedesktop.DBus.Properties.Set string:org.a11y.Status \
        string:IsEnabled variant:boolean:true >/dev/null 2>&1 || true
    dbus-send --session --dest=org.a11y.Bus /org/a11y/bus \
        org.freedesktop.DBus.Properties.Set string:org.a11y.Status \
        string:ScreenReaderEnabled variant:boolean:true >/dev/null 2>&1 || true
fi

exec "$@"
