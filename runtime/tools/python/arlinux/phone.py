"""User-authorized Android main-screen observation, including other foreground apps.

This is separate from compositor capture and provides no phone input injection.
The Android host owns MediaProjection and its visible stop-sharing notification.
"""

import base64
import subprocess
from .desktop import _request, _save_png


def request_capture() -> bool:
    """Show Android's screen-sharing consent while ARLinux is in the foreground.

    Returns whether sharing is already active; otherwise the user must finish
    the system consent dialog. Poll capture_active() before requesting an image.
    For system settings, share the entire screen rather than a single app.
    """
    return _request({'operation': 'phone-capture-start'})['active']


def capture_active() -> bool:
    """Whether the Android host has an authorized screen-sharing session."""
    return _request({'operation': 'phone-capture-status'})['active']


def capture_status() -> str:
    """Return idle, pending, starting, active, denied, stopped or failed.

    Pending means the system consent dialog is open; denied means the user
    cancelled. State is session-local and resets to idle if the host restarts.
    """
    return _request({'operation': 'phone-capture-status'})['state']


def stop_capture() -> None:
    """End screen sharing; a later session requires fresh system consent."""
    _request({'operation': 'phone-capture-stop'})


def open_opencode(serial: str) -> str:
    """Return this host to the foreground and focus OpenCode using authorized ADB.

    Pass the exact ADB serial whose identity and shell access you verified.
    This uses ordinary `adb shell am start`; it neither pairs nor grants access.
    Stop screen sharing before returning. The host supplies its package so the
    same command handles Debug and Release. Returns Android's launch output;
    verify the resulting foreground window instead of assuming input delivery.
    """
    return _open_host(serial, opencode=True)


def open_desktop(serial: str) -> str:
    """Return ARLinux to the foreground through an explicitly verified ADB serial.

    Stop sharing first, then use tasks.focus(saved_window_id) to restore the
    requesting assistant. This does not pair, authorize or start a new agent.
    """
    return _open_host(serial, opencode=False)


def _open_host(serial: str, *, opencode: bool) -> str:
    if not isinstance(serial, str) or not serial or any(c.isspace() for c in serial):
        raise ValueError('An explicit ADB device serial is required')
    package = _request({'operation': 'phone-capture-status'})['package']
    command = ['adb', '-s', serial, 'shell', 'am', 'start',
               '-n', package + '/io.taowen.arlinux.ArlinuxActivity']
    if opencode:
        command += ['--ez', 'io.taowen.arlinux.extra.OPEN_OPENCODE', 'true']
    result = subprocess.run(command, check=True, capture_output=True, text=True, timeout=15)
    output = result.stdout + result.stderr
    if 'Error:' in output or 'Exception' in output:
        raise RuntimeError(output.strip())
    return output.strip()


def screenshot(path, *, max_size: int = 1280):
    """Save the shared phone screen as a private PNG, even with ARLinux behind Settings.

    Requires request_capture() and user consent. Protected content may be black.
    The host captures at most 1920 pixels on the longest side. This image has no
    compositor geometry and cannot be used with desktop.click(). Keep pairing
    codes local; do not quote them in conversation or diagnostic logs.
    """
    response = _request({'operation': 'phone-screenshot', 'maxSize': max_size})
    data = base64.b64decode(response['png'], validate=True)
    return _save_png(data, response['width'], response['height'], path)
