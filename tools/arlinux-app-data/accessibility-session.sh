#!/bin/sh
# Publish the AT-SPI bus before starting the desktop application.
set -eu

# Start the same standard input bridge for every desktop using this session.
# Waiting for activation avoids launching toolkit clients before IBus is ready.
dbus-send --session --print-reply --dest=org.arlinux.HostedInput \
    /org/arlinux/HostedInput org.freedesktop.DBus.Peer.Ping >/dev/null

# Native Wayland clients use the compositor text-input protocol. Start upstream's
# adapter after HostedInput has registered its engine; otherwise ibus-ui-gtk3
# exits when it observes a session with no global engine yet.
if [ -x /usr/lib/arlinux/ibus/libexec/ibus-ui-gtk3 ]; then
    env -u GTK_IM_MODULE -u QT_IM_MODULE GDK_BACKEND=wayland /usr/lib/arlinux/ibus/libexec/ibus-ui-gtk3 \
        --enable-wayland-im >"$XDG_RUNTIME_DIR/ibus-wayland.log" 2>&1 &
fi
if command -v dbus-send >/dev/null 2>&1; then
    tries=0
    address=
    while [ -z "$address" ] && [ "$tries" -lt 50 ]; do
        address=$(dbus-send --session --print-reply --reply-timeout=1000 --dest=org.a11y.Bus \
            /org/a11y/bus org.a11y.Bus.GetAddress 2>/dev/null |
            sed -n 's/^[[:space:]]*string "\(.*\)"$/\1/p' | head -n 1) || true
        [ -n "$address" ] || sleep 0.1
        tries=$((tries + 1))
    done
    [ -n "$address" ] || { echo 'Desktop accessibility bus did not become ready' >&2; exit 1; }
    export AT_SPI_BUS_ADDRESS="$address" NO_AT_BRIDGE=0
    # Accessibility is a desktop dependency, not an optional application feature.
    dbus-send --bus="$address" --print-reply --reply-timeout=3000 \
        --dest=org.a11y.atspi.Registry /org/a11y/atspi/accessible/root \
        org.freedesktop.DBus.Peer.Ping >/dev/null
    if [ -n "${address:-}" ] && [ -n "${DISPLAY:-}" ] && command -v xprop >/dev/null 2>&1; then
        tries=0
        until xprop -root -f AT_SPI_BUS 8s -set AT_SPI_BUS "$address" >/dev/null 2>&1; do
            tries=$((tries + 1))
            [ "$tries" -ge 50 ] && break
            sleep 0.1
        done
    fi
    # dbus-send defaults to a signal. Properties.Set needs a method call;
    # --print-reply also waits until accessibility is enabled before exec.
    dbus-send --session --print-reply --dest=org.a11y.Bus /org/a11y/bus \
        org.freedesktop.DBus.Properties.Set string:org.a11y.Status \
        string:IsEnabled variant:boolean:true >/dev/null
    dbus-send --session --print-reply --dest=org.a11y.Bus /org/a11y/bus \
        org.freedesktop.DBus.Properties.Set string:org.a11y.Status \
        string:ScreenReaderEnabled variant:boolean:true >/dev/null
fi

# Publish only a fully initialized session to out-of-process launchers. Atomic
# replacement prevents readers from observing an empty or partial address.
bus_file=$(mktemp "$XDG_RUNTIME_DIR/.desktop-session-bus.XXXXXX")
printf '%s\n' "$DBUS_SESSION_BUS_ADDRESS" > "$bus_file"
mv -f "$bus_file" "$XDG_RUNTIME_DIR/desktop-session-bus"

# X resources are session state. Load the user's file, not an APK-owned DPI policy.
if [ -n "${DISPLAY:-}" ] && [ -f "$HOME/.Xresources" ] && command -v xrdb >/dev/null 2>&1; then
    xrdb -merge "$HOME/.Xresources"
fi

# One app drawer for every desktop, with this session's input and accessibility bus.
python3 /usr/lib/arlinux/guest/yibu-launcher.py > "$XDG_RUNTIME_DIR/apps.log" 2>&1 &
exec "$@"
