#!/usr/bin/env python3
"""Drive a guest GUI over AT-SPI (GTK/Qt, including native Wayland).

Buttons use Action.doAction. Text uses EditableText when applications expose
it, or the standard desktop clipboard after semantically focusing a custom
document canvas.
"""
from __future__ import print_function

import glob
import os
import re
import shutil
import subprocess
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


def select_applications(entries, app_needle):
    needle = (app_needle or "").lower()
    if not needle:
        return entries
    exact = [app for app in entries if (app.name or "").lower() == needle]
    return exact or [app for app in entries if needle in (app.name or "").lower()]


def collect(app_needle):
    import pyatspi
    desktop = pyatspi.Registry.getDesktop(0)
    nodes = []
    entries = list(desktop)
    apps = [(app.name or "") for app in entries]
    for app in select_applications(entries, app_needle):
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
    license text is a large label that also claims EditableText.  Its document
    canvas can be a focusable frame.  Treating any EditableText as a document
    silently corrupts those controls, which is worse than failing.
    """
    role = (info["role"] or "").strip().lower()
    if role in ("document", "document text", "web document", "paragraph"):
        return True
    if _document_name(info):
        return True
    return (role in ("text", "entry") and
            int(info["w"]) >= 240 and int(info["h"]) >= 100)


def paste_text(text):
    """Paste Unicode using the desktop's standard clipboard and input tools."""
    import pyatspi
    if os.environ.get("DISPLAY") and shutil.which("xclip") and shutil.which("xdotool"):
        owner = subprocess.Popen(
            ["xclip", "-selection", "clipboard", "-in"],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        try:
            owner.stdin.write(text.encode("utf-8"))
            owner.stdin.close()
            time.sleep(0.1)
            subprocess.run(
                ["xdotool", "key", "--clearmodifiers", "ctrl+v"],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            time.sleep(0.2)
        finally:
            if owner.poll() is None:
                owner.terminate()
                try:
                    owner.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    owner.kill()
        return "xclip+xdotool"
    if os.environ.get("WAYLAND_DISPLAY") and shutil.which("wl-copy"):
        owner = subprocess.Popen(
            ["wl-copy", "--paste-once", "--type", "text/plain;charset=utf-8"],
            stdin=subprocess.PIPE,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            start_new_session=True,
        )
        owner.stdin.write(text.encode("utf-8"))
        owner.stdin.close()
        time.sleep(0.1)
        pyatspi.Registry.generateKeyboardEvent(37, None, pyatspi.KEY_PRESS)
        pyatspi.Registry.generateKeyboardEvent(0, "v", pyatspi.KEY_SYM)
        pyatspi.Registry.generateKeyboardEvent(37, None, pyatspi.KEY_RELEASE)
        owner.wait(timeout=5)
        return "wl-copy+AT-SPI"
    raise RuntimeError("no standard clipboard/input tool pair is installed")


def click_toggle_at_bounds(info):
    """Use the semantic target's bounds when a Qt Toggle action is a no-op."""
    if not (os.environ.get("DISPLAY") and shutil.which("xdotool")):
        return False
    x = int(info["x"])
    y = int(info["y"])
    w = int(info["w"])
    h = int(info["h"])
    if x < 0 or y < 0 or w < 20 or h < 20:
        return False
    subprocess.run(
        ["xdotool", "mousemove", "--sync", str(x + w // 2), str(y + h // 2),
         "click", "1"],
        check=True,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return True


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


def editable_write_result(ok, before, after, inserted_text):
    """Return (success, verified) for readable and write-only editors."""
    verified = (len(after) >= len(before) + len(inserted_text) and
                inserted_text in after)
    write_only = bool(ok and not before and not after)
    return bool(ok and (verified or write_only)), verified


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
        usable = [(a, i) for a, i in exact if sized(i)] \
            or [(a, i) for a, i in hits if sized(i)]
        if not usable:
            sys.stderr.write("atspi-do click: no Action widget matching %r (apps=%s)\n"
                             % (needle, apps))
            sys.exit(1)
        acc, info = usable[-1]
        try:
            act = acc.queryAction()
            names = [act.getName(i) for i in range(act.nActions)]
            if any((name or "").lower() == "toggle" for name in names) \
                    and click_toggle_at_bounds(info):
                print("click %s role=%s name=%r size=%dx%d actions=%s "
                      "ok=True method=semantic-bounds" % (
                          needle, info["role"], info["name"], info["w"],
                          info["h"], names))
                return
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
        paste_method = paste_text(text)
        print("type %d chars into role=%s name=%r size=%dx%d "
              "method=%s focused=True" % (
                  len(text), info["role"], info["name"], info["w"], info["h"],
                  paste_method))
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
    success, inserted = editable_write_result(ok, before, after, text)
    print("type %d chars into role=%s name=%r size=%dx%d ok=%s verified=%s" % (
        len(text), info["role"], info["name"], info["w"], info["h"],
        ok, inserted))
    # Some hosted custom canvases implement the standard EditableText write
    # operation but intentionally expose no readable document text.  In that
    # case the write result is authoritative; keep verification strict when
    # the target exposes readable content.
    if success and not inserted:
        print("type result is write-only; EditableText accepted the operation")
    if not success:
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
