"""User-authorized Android main-screen observation, including other foreground apps.

This is separate from compositor capture and provides no phone input injection.
The Android host owns MediaProjection and its visible stop-sharing notification.
"""

import base64
from .desktop import _request, _save_png


def request_capture() -> bool:
    """Show Android's screen-sharing consent while ARLinux is in the foreground.

    Returns whether sharing is already active; otherwise the user must finish
    the system consent dialog. Poll capture_active() before requesting an image.
    For system settings, share the entire screen rather than a single app.
    """
    return _request({'operation': 'phone-capture-start'})['active']


def capture_active() -> bool:
    """Whether this process currently has an authorized screen-sharing session."""
    return _request({'operation': 'phone-capture-status'})['active']


def stop_capture() -> None:
    """End screen sharing; a later session requires fresh system consent."""
    _request({'operation': 'phone-capture-stop'})


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
