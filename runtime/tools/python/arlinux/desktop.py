"""Observe and click the ARLinux compositor when semantic actions are missing.

Use upstream AT-SPI first. ``screenshot(path)`` writes a full desktop PNG that
OpenCode can inspect with its image-capable Read tool. ``click(x, y, image=path)``
uses coordinates from that unmodified image, independent of the phone preview,
touch mode, X11/Wayland client backend or AR glasses. Neither operation uses ADB,
root or Android's system accessibility permission.
"""

from __future__ import annotations

import base64
import json
import os
from pathlib import Path
import socket
import struct
import tempfile


def _request(value):
    with socket.socket(socket.AF_UNIX) as client:
        client.settimeout(15)
        client.connect(str(Path(os.environ['XDG_RUNTIME_DIR']) / 'hosted.sock'))
        client.sendall(('@desktop\t' + json.dumps(value) + '\n').encode())
        with client.makefile('rb') as stream:
            response = stream.readline(48 * 1024 * 1024 + 1)
    if len(response) > 48 * 1024 * 1024 or not response.endswith(b'\n'):
        raise RuntimeError('Incomplete or oversized desktop response')
    if not response.startswith(b'OK '):
        raise RuntimeError(response.decode(errors='replace').strip())
    return json.loads(response[3:])


def dimensions(image: str | os.PathLike) -> tuple[int, int]:
    """Return the original PNG's width and height, before viewer scaling."""
    with Path(image).open('rb') as source:
        header = source.read(24)
    if len(header) != 24 or header[:8] != b'\x89PNG\r\n\x1a\n' or header[12:16] != b'IHDR':
        raise ValueError('Use an unmodified PNG from desktop.screenshot')
    return struct.unpack('!II', header[16:24])


def screenshot(path: str | os.PathLike) -> Path:
    """Save the latest completed visible desktop frame as a private PNG.

    Returns its absolute path. Inspect the image before choosing coordinates;
    if an image viewer scales it, convert points back to ``dimensions(path)``.
    It includes all compositor content, including hosted Android applications,
    but excludes Android system windows, the phone keyboard and phone overlays.
    A delivered input action need not have produced a new frame yet: wait for
    the application's resulting UI before treating a screenshot as acceptance.
    """
    response = _request({'operation': 'screenshot'})
    data = base64.b64decode(response['png'], validate=True)
    target = Path(path).expanduser().absolute()
    target.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.screenshot-', delete=False) as output:
            temporary = Path(output.name)
            output.write(data)
        if dimensions(temporary) != (response['width'], response['height']):
            raise RuntimeError('Desktop screenshot dimensions do not match its response')
        os.replace(temporary, target)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)
    return target


def click(x: float, y: float, *, image: str | os.PathLike,
          button: str = 'left', count: int = 1) -> None:
    """Click an observed point in an unmodified screenshot; count=2 double-clicks.

    The host rejects resized images, changed desktop dimensions and points
    outside the canvas. Take another screenshot after navigation. Delivery is
    not proof of success: verify the resulting UI through AT-SPI or an image.
    """
    width, height = dimensions(image)
    _request({'operation': 'click', 'x': x, 'y': y, 'width': width, 'height': height,
              'button': button, 'count': count})
