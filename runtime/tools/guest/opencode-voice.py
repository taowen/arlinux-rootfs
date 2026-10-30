#!/usr/bin/python3
"""Focus or submit OpenCode's prompt on any desktop using standard AT-SPI."""

from __future__ import annotations

import os
from pathlib import Path
import subprocess
import sys
import time

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from opencode import installed_binary
import desktop_bus
import shell_tasks


class AccessibleNotReady(RuntimeError):
    """An application or control has not appeared in the live AT-SPI tree yet."""


def select_accessibility_bus(*, deadline=None) -> None:
    # Every desktop publishes its session bus before launching applications.
    # Resolve AT-SPI through that bus instead of requiring an X11 root property.
    while True:
        try:
            joined = desktop_bus.environment(os.environ)
            for name in ('DBUS_SESSION_BUS_ADDRESS', 'AT_SPI_BUS_ADDRESS', 'NO_AT_BRIDGE'):
                os.environ[name] = joined[name]
            return
        except RuntimeError:
            # A restarted desktop replaces its published session address. Read
            # it again rather than caching a socket belonging to the old session.
            if deadline is None or time.monotonic() >= deadline:
                raise
            time.sleep(0.2)


def descendants(root, reverse: bool = False):
    stack = [root]
    while stack:
        node = stack.pop()
        yield node
        try:
            count = node.childCount
        except Exception:
            continue
        indices = range(count) if reverse else range(count - 1, -1, -1)
        for index in indices:
            try:
                stack.append(node[index])
            except Exception:
                pass


def find_opencode():
    import pyatspi

    binary = installed_binary()
    if binary is None:
        raise AccessibleNotReady('OpenCode is not installed')
    application = binary.name
    desktop = pyatspi.Registry.getDesktop(0)
    for app in desktop:
        try:
            if app.name == application:
                return app
        except Exception:
            pass
    raise AccessibleNotReady("OpenCode window not found")


def raise_opencode() -> bool:
    # Use the same owned launch and compositor identities as Tasks. Focusing an
    # AT-SPI entry or an X11 child alone cannot select a hidden Yibu task tab.
    owners = [task for task in shell_tasks.task_request('list')['tasks']
              if task.get('appId') == 'arlinux-opencode.desktop']
    pids = {pid for task in owners for pid in task['pids']}
    if not pids:
        return False
    windows = [window for window in shell_tasks.clients() if window.get('pid') in pids]
    if not windows:
        return False
    window = min(windows, key=lambda item: item.get('focusHistoryID', 9999))
    shell_tasks.operate(shell_tasks.identity(window), 'focus')
    return True


def find_named(root, role: str, name: str):
    import pyatspi

    for node in descendants(root, reverse=role == "entry" and name == "Prompt"):
        try:
            if (node.getRoleName() == role and node.name == name
                    and node.getState().contains(pyatspi.STATE_SHOWING)):
                return node
        except Exception:
            pass
    raise AccessibleNotReady(f"OpenCode {name!r} {role} not found")


def visible_dialog(root):
    import pyatspi

    # Electron appends overlays after the main page in its web document.
    # Do not walk a potentially huge conversation just to check for a modal.
    document = find_named(root, "document web", "OpenCode")
    for index in range(document.childCount - 1, 0, -1):
        for node in descendants(document[index]):
            try:
                if (node.getRoleName() == "dialog"
                        and node.getState().contains(pyatspi.STATE_SHOWING)):
                    return node
            except Exception:
                pass
    return None


def open_prompt(*, allow_navigation=True):
    """Return the current composer, leaving Home or a modal if necessary."""
    app = find_opencode()
    dialog = visible_dialog(app)
    if dialog is not None:
        try:
            find_named(dialog, "page tab", "General")
        except RuntimeError:
            raise RuntimeError("close the OpenCode dialog before voice input")
        subprocess.run(["xdotool", "key", "--clearmodifiers", "Escape"], check=True)
        time.sleep(0.2)
        app = find_opencode()
    try:
        return find_named(app, "entry", "Prompt")
    except AccessibleNotReady:
        pass
    if not allow_navigation:
        raise AccessibleNotReady("OpenCode session is loading its prompt")
    # Home and Settings have no composer. Reuse an open new-session tab when
    # possible, preserving its draft instead of making a duplicate session.
    try:
        target = find_named(app, "link", "New session")
    except RuntimeError:
        target = find_named(app, "button", "New session")
    if not any(activate(target, name) for name in ("jump", "press", "click", "open")):
        raise RuntimeError("could not open OpenCode session")
    return None


def text_of(entry) -> str:
    text = entry.queryText()
    return text.getText(0, text.characterCount)


def prompt_content(text: str) -> str:
    """Ignore accessibility placeholders and empty editor formatting."""
    return text.replace("\u200b", "").replace("\ufffc", "").strip()


def activate(node, action_name: str) -> bool:
    actions = node.queryAction()
    for index in range(actions.nActions):
        if actions.getName(index).lower() == action_name:
            return bool(actions.doAction(index))
    return False


def focus_prompt(*, deadline=None) -> None:
    try:
        find_opencode()
    except AccessibleNotReady:
        if sys.argv[1:2] == ["focus"]:
            # The host owns the application independently of this voice request.
            raise SystemExit(3)
    # A web document is published before its controls during cold startup.
    # Use one startup budget ending at the actual composer, not a second short
    # timeout starting at the empty document. Navigate at most once per request.
    if deadline is None:
        deadline = time.monotonic() + 24
    navigation_started = False
    raised = False
    last_pending = "OpenCode prompt is loading"
    while time.monotonic() < deadline:
        try:
            if not raised:
                if not raise_opencode():
                    raise AccessibleNotReady('OpenCode has no mapped task window yet')
                raised = True
            prompt = open_prompt(allow_navigation=not navigation_started)
            if prompt is None:
                navigation_started = True
                continue
            break
        except AccessibleNotReady as error:
            last_pending = str(error)
            time.sleep(0.2)
    else:
        raise RuntimeError(f"OpenCode prompt did not become ready within 24 seconds: {last_pending}")
    try:
        focused = prompt.queryComponent().grabFocus()
    except Exception:
        focused = False
    if not focused and not activate(prompt, "activate"):
        raise RuntimeError("could not focus OpenCode prompt")
    baseline = text_of(prompt)
    state = Path(os.environ.get("TMPDIR", "/tmp")) / "opencode-voice-baseline"
    state.write_text(baseline, encoding="utf-8")
    print("focused")


def send_prompt() -> None:
    if not raise_opencode():
        raise RuntimeError('OpenCode has no mapped task window')
    state = Path(os.environ.get("TMPDIR", "/tmp")) / "opencode-voice-baseline"
    if not state.exists():
        raise RuntimeError("voice prompt was not prepared")
    baseline = state.read_text(encoding="utf-8")
    deadline = time.monotonic() + 22
    changed = None
    stable_since = None
    while time.monotonic() < deadline:
        # Electron replaces its accessible entry as the prompt changes. Never
        # hold an AT-SPI object across recognition updates.
        try:
            prompt = find_named(find_opencode(), "entry", "Prompt")
            current = text_of(prompt)
        except Exception:
            time.sleep(0.15)
            continue
        if prompt_content(current) and prompt_content(current) != prompt_content(baseline):
            if current != changed:
                changed = current
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= 1.5:
                # Chromium advertises a Send.click AT-SPI action but returns
                # false without invoking it. Submit through the semantically
                # focused prompt's standard Enter binding instead.
                app = find_opencode()
                if visible_dialog(app) is not None:
                    raise RuntimeError("OpenCode opened a dialog during voice input")
                prompt = find_named(app, "entry", "Prompt")
                if not prompt.queryComponent().grabFocus():
                    raise RuntimeError("could not refocus OpenCode prompt")
                subprocess.run(
                    ["xdotool", "key", "--clearmodifiers", "Return"],
                    check=True,
                )
                submitted = time.monotonic() + 3
                while time.monotonic() < submitted:
                    try:
                        prompt = find_named(find_opencode(), "entry", "Prompt")
                        if not prompt_content(text_of(prompt)):
                            break
                    except Exception:
                        # Submission can replace the entry while navigating.
                        pass
                    time.sleep(0.1)
                else:
                    raise RuntimeError("OpenCode did not accept prompt submission")
                state.unlink(missing_ok=True)
                print("sent")
                return
        time.sleep(0.15)
    raise RuntimeError("no new voice text appeared in OpenCode prompt")


def main() -> int:
    if len(sys.argv) != 2 or sys.argv[1] not in {"focus", "focus-wait", "send"}:
        print("usage: opencode-voice.py focus|focus-wait|send", file=sys.stderr)
        return 2
    if sys.argv[1] == 'focus' and installed_binary() is None:
        return 4
    deadline = time.monotonic() + 24 if sys.argv[1] in {"focus", "focus-wait"} else None
    select_accessibility_bus(deadline=deadline)
    if sys.argv[1] in {"focus", "focus-wait"}:
        focus_prompt(deadline=deadline)
    else:
        send_prompt()
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as error:
        print(error, file=sys.stderr)
        raise SystemExit(1)
