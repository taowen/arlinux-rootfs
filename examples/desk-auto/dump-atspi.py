#!/usr/bin/env python3
"""Dump the AT-SPI tree of a running guest GUI (LyX, GTK, stock Qt)."""
from __future__ import print_function

import glob
import json
import os
import sys


def ensure_session_bus():
    if os.environ.get("DBUS_SESSION_BUS_ADDRESS"):
        return
    import dbus
    sockets = []
    for path in glob.glob("/tmp/dbus-*"):
        if os.path.isdir(path) or not os.path.exists(path):
            continue
        sockets.append(path)
    sockets.sort(key=lambda p: os.path.getmtime(p), reverse=True)
    for path in sockets:
        os.environ["DBUS_SESSION_BUS_ADDRESS"] = "unix:path=" + path
        try:
            probe = dbus.bus.BusConnection(os.environ["DBUS_SESSION_BUS_ADDRESS"])
            try:
                probe.get_name_owner("org.a11y.Bus")
                return
            finally:
                probe.close()
        except Exception:
            continue
    sys.stderr.write("dump-atspi: no session bus with org.a11y.Bus\n")
    sys.exit(2)


def enable_screen_reader():
    try:
        import dbus
        bus = dbus.SessionBus()
        obj = bus.get_object("org.a11y.Bus", "/org/a11y/bus")
        props = dbus.Interface(obj, "org.freedesktop.DBus.Properties")
        props.Set("org.a11y.Status", "IsEnabled",
                  dbus.Boolean(True, variant_level=1))
        props.Set("org.a11y.Status", "ScreenReaderEnabled",
                  dbus.Boolean(True, variant_level=1))
    except Exception:
        pass


def walk(acc, out, depth=0, limit=4000):
    if acc is None or len(out) >= limit or depth > 40:
        return
    name = acc.name or ""
    role = acc.getRoleName() or ""
    desc = ""
    try:
        desc = acc.description or ""
    except Exception:
        pass
    try:
        interfaces = list(acc.get_interfaces())
    except Exception:
        interfaces = []
    text = name
    try:
        if "Text" in interfaces:
            value = acc.queryText()
            text = value.getText(0, min(value.characterCount, 4096)) or name
    except Exception:
        pass
    actions = []
    try:
        if "Action" in interfaces:
            action = acc.queryAction()
            actions = [action.getName(i) for i in range(action.nActions)]
    except Exception:
        pass
    x = y = w = h = 0
    visible = True
    try:
        if "Component" in acc.get_interfaces():
            import pyatspi
            e = acc.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
            x, y, w, h = int(e.x), int(e.y), int(e.width), int(e.height)
            st = acc.queryComponent().getLayer()
            del st
        states = acc.getState().getStates()
        import pyatspi
        visible = pyatspi.STATE_SHOWING in states or pyatspi.STATE_VISIBLE in states
    except Exception:
        pass
    out.append({
        "role": role,
        "name": name,
        "description": desc,
        "text": text,
        "interfaces": interfaces,
        "actions": actions,
        "accelerator": "",
        "x": x,
        "y": y,
        "w": w,
        "h": h,
        "visible": bool(visible),
    })
    try:
        n = acc.childCount
    except Exception:
        return
    if n > 4000:
        n = 4000
    for i in range(n):
        try:
            child = acc.getChildAtIndex(i)
        except Exception:
            continue
        walk(child, out, depth + 1, limit)


def main():
    ensure_session_bus()
    enable_screen_reader()
    needle = (os.environ.get("APP") or (sys.argv[1] if len(sys.argv) > 1 else "")).lower()
    import pyatspi
    desktop = pyatspi.Registry.getDesktop(0)
    widgets = []
    apps = []
    for app in desktop:
        aname = (app.name or "")
        apps.append(aname)
        if needle and needle not in aname.lower():
            continue
        walk(app, widgets)
    print(json.dumps({"apps": apps, "widgets": widgets, "count": len(widgets)},
                     ensure_ascii=False))


if __name__ == "__main__":
    main()
