#!/usr/bin/env python3
"""Drive a guest GUI over AT-SPI (GTK/Qt, including native Wayland).

Does not use XTest or screen coordinates. Buttons use Action.doAction;
text uses EditableText.insertText / setTextContents.
"""
from __future__ import print_function

import glob
import os
import re
import sys
import time


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
    sys.stderr.write("atspi-do: no session bus with org.a11y.Bus\n")
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
    import pyatspi
    name = acc.name or ""
    role = ""
    try:
        role = acc.getRoleName() or ""
    except Exception:
        pass
    ifaces = []
    try:
        ifaces = list(acc.get_interfaces())
    except Exception:
        pass
    x = y = w = h = 0
    try:
        if "Component" in ifaces:
            e = acc.queryComponent().getExtents(pyatspi.DESKTOP_COORDS)
            x, y, w, h = int(e.x), int(e.y), int(e.width), int(e.height)
    except Exception:
        pass
    text = name
    try:
        if "Text" in ifaces:
            text = acc.queryText().getText(0, -1) or name
    except Exception:
        pass
    out.append((acc, {
        "role": role,
        "name": name,
        "text": text,
        "ifaces": ifaces,
        "x": x, "y": y, "w": w, "h": h,
    }))
    try:
        n = acc.childCount
    except Exception:
        return
    for i in range(min(n, 4000)):
        try:
            walk(acc.getChildAtIndex(i), out, depth + 1, limit)
        except Exception:
            continue


def collect(app_needle):
    import pyatspi
    desktop = pyatspi.Registry.getDesktop(0)
    nodes = []
    apps = []
    needle = (app_needle or "").lower()
    for app in desktop:
        aname = app.name or ""
        apps.append(aname)
        if needle and needle not in aname.lower():
            continue
        walk(app, nodes)
    return apps, nodes


def sized(info):
    return int(info["w"]) >= 20 and int(info["h"]) >= 20


def _document_name(info):
    name = (info["name"] or "").strip().lower()
    return bool(re.match(r"^document\s*\d+(?:\s*\*)?$", name))


def _document_editor(info):
    """Return true only for an editor, never for a toolbar/dialog field.

    WPS exposes its font-family and font-size boxes as EditableText while its
    document canvas is a focusable frame.  Treating any EditableText as a
    document silently corrupts those controls, which is worse than failing.
    """
    role = (info["role"] or "").strip().lower()
    if role in ("document", "document text", "web document", "paragraph"):
        return True
    if _document_name(info):
        return True
    return (int(info["w"]) >= 240 and int(info["h"]) >= 100)


def choose_type_target(nodes):
    edits = []
    for acc, info in nodes:
        if "EditableText" not in info["ifaces"]:
            continue
        role = (info["role"] or "").lower()
        if role in _SKIP_TYPE_ROLES or not _document_editor(info):
            continue
        if " - writer" in (info["name"] or "").lower():
            continue
        area = max(0, int(info["w"])) * max(0, int(info["h"]))
        rank = 3 if _document_name(info) else 2 if role == "paragraph" else 1
        edits.append((rank, area, acc, info))
    if edits:
        edits.sort(key=lambda item: (item[0], item[1]), reverse=True)
        return "editable", edits[0][2], edits[0][3]

    # WPS's custom canvas is sometimes exported as a focusable frame instead
    # of EditableText.  It is still a semantic target: focus it through AT-SPI
    # and enter text through the AT-SPI keyboard event API.
    frames = []
    for acc, info in nodes:
        role = (info["role"] or "").lower()
        if role not in ("frame", "window", "document", "document text"):
            continue
        if not _document_name(info) or int(info["h"]) < 200:
            continue
        area = max(0, int(info["w"])) * max(0, int(info["h"]))
        frames.append((area, acc, info))
    if frames:
        frames.sort(key=lambda item: item[0], reverse=True)
        return "keyboard", frames[0][1], frames[0][2]
    return None, None, None


def cmd_click(needle, app):
    last_error = None
    for attempt in range(2):
        apps, nodes = collect(app)
        want = needle.lower()
        hits = []
        for acc, info in nodes:
            if "Action" not in info["ifaces"]:
                continue
            blob = " ".join([info["name"], info["text"], info["role"]]).lower()
            if want in blob:
                hits.append((acc, info))
        exact = [(a, i) for a, i in hits
                 if want in ((i["name"] or "").lower(),
                             (i["text"] or "").lower())]
        usable = [(a, i) for a, i in exact if sized(i)] or exact \
            or [(a, i) for a, i in hits if sized(i)] or hits
        if not usable:
            sys.stderr.write("atspi-do click: no Action widget matching %r (apps=%s)\n"
                             % (needle, apps))
            sys.exit(1)
        acc, info = usable[-1]
        try:
            act = acc.queryAction()
            names = [act.getName(i) for i in range(act.nActions)]
            ok = act.doAction(0)
            print("click %s role=%s name=%r size=%dx%d actions=%s ok=%s" % (
                needle, info["role"], info["name"], info["w"], info["h"],
                names, ok))
            if ok:
                return
            last_error = "action returned false"
        except Exception as exc:
            last_error = str(exc)
        if attempt == 0:
            time.sleep(0.15)
    sys.stderr.write("atspi-do click: action failed after refresh: %s\n" % last_error)
    sys.exit(1)


_SKIP_TYPE_ROLES = (
    "push button", "toggle button", "check box", "radio button",
    "menu item", "page tab", "button",
)


def cmd_type(text, app):
    apps, nodes = collect(app)
    method, acc, info = choose_type_target(nodes)
    if not acc:
        sys.stderr.write("atspi-do type: no semantic document editor (apps=%s); "
                         "refusing to type into a toolbar or dialog field\n" % apps)
        sys.exit(1)
    if method == "keyboard":
        import pyatspi
        focused = False
        try:
            action = acc.queryAction()
            for index in range(action.nActions):
                if (action.getName(index) or "").lower() in ("setfocus", "set focus"):
                    focused = bool(action.doAction(index))
                    break
        except Exception:
            pass
        if not focused:
            try:
                focused = bool(acc.queryComponent().grabFocus())
            except Exception:
                pass
        if not focused:
            sys.stderr.write("atspi-do type: document target could not take focus\n")
            sys.exit(1)
        time.sleep(0.1)
        pyatspi.Registry.generateKeyboardEvent(0, text, pyatspi.KEY_STRING)
        print("type %d chars into role=%s name=%r size=%dx%d "
              "method=atspi-keyboard focused=True" % (
                  len(text), info["role"], info["name"], info["w"], info["h"]))
        return

    et = acc.queryEditableText()
    try:
        text_iface = acc.queryText()
        n = text_iface.characterCount
        before = text_iface.getText(0, -1) or ""
    except Exception:
        n = 0
        before = ""
    ok = et.insertText(n, text, len(text))
    try:
        after = acc.queryText().getText(0, -1) or ""
    except Exception:
        after = ""
    inserted = len(after) >= len(before) + len(text) and text in after
    print("type %d chars into role=%s name=%r size=%dx%d ok=%s verified=%s" % (
        len(text), info["role"], info["name"], info["w"], info["h"],
        ok, inserted))
    if not ok or not inserted:
        sys.stderr.write("atspi-do type: EditableText did not retain inserted text "
                         "(offset=%d before=%r after=%r)\n" %
                         (n, before[:120], after[:120]))
        sys.exit(1)


def cmd_set(text, app):
    apps, nodes = collect(app)
    edits = [(acc, info) for acc, info in nodes
             if "EditableText" in info["ifaces"] and sized(info)
             and int(info["h"]) < 80]
    if not edits:
        sys.stderr.write("atspi-do set: no small EditableText (apps=%s)\n" % apps)
        sys.exit(1)
    acc, info = edits[-1]
    acc.queryEditableText().setTextContents(text)
    print("set name=%r size=%dx%d text=%r" % (
        info["name"], info["w"], info["h"], text))


def usage():
    sys.stderr.write(
        "usage: atspi-do.py click NEEDLE [APP]\n"
        "       atspi-do.py type TEXT [APP]\n"
        "       atspi-do.py set TEXT [APP]\n")
    sys.exit(2)


def main():
    if len(sys.argv) < 2:
        usage()
    ensure_session_bus()
    enable_screen_reader()
    cmd = sys.argv[1]
    if cmd == "click" and len(sys.argv) >= 3:
        cmd_click(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "")
    elif cmd == "type" and len(sys.argv) >= 3:
        cmd_type(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "")
    elif cmd == "set" and len(sys.argv) >= 3:
        cmd_set(sys.argv[2], sys.argv[3] if len(sys.argv) > 3 else "")
    else:
        usage()


if __name__ == "__main__":
    main()
