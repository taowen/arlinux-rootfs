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
import math
import os
from pathlib import Path
import socket
import struct
import tempfile
import zlib


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


def screenshot(path: str | os.PathLike, *, max_size: int = 1280) -> Path:
    """Save the latest completed visible desktop frame as a private PNG.

    Returns its absolute path. The longest image side is at most max_size
    (default 1280), avoiding OpenCode Read's automatic 2000-pixel downscaling.
    Choose coordinates in this PNG, using dimensions(path). The PNG retains
    its desktop geometry so click() maps image pixels back to desktop pixels.
    It includes all compositor content, including hosted Android applications,
    but excludes Android system windows, the phone keyboard and phone overlays.
    A delivered input action need not have produced a new frame yet: wait for
    the application's resulting UI before treating a screenshot as acceptance.
    """
    response = _request({'operation': 'screenshot', 'maxSize': max_size})
    data = base64.b64decode(response['png'], validate=True)
    geometry = dict(image_width=response['width'], image_height=response['height'],
                    desktop_width=response['desktopWidth'], desktop_height=response['desktopHeight'])
    text = b'arlinux.desktop\0' + json.dumps(geometry, separators=(',', ':')).encode()
    chunk = b'tEXt' + text
    # PNG text metadata immediately after IHDR keeps each image self-contained;
    # no sidecar, runtime table or guessed viewer scale is needed for input.
    data = data[:33] + struct.pack('!I', len(text)) + chunk + struct.pack('!I', zlib.crc32(chunk)) + data[33:]
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

    The image geometry maps PNG pixels to desktop pixels. Resized images,
    changed desktop dimensions and points outside the PNG are rejected.
    Take another screenshot after navigation. Delivery is
    not proof of success: verify the resulting UI through AT-SPI or an image.
    """
    width, height = dimensions(image)
    with Path(image).open('rb') as source:
        source.seek(33)
        header = source.read(8)
        if len(header) != 8:
            raise ValueError('Take a new screenshot with desktop.screenshot')
        length, kind = struct.unpack('!I4s', header)
        if kind != b'tEXt' or not 1 <= length <= 4096:
            raise ValueError('Take a new screenshot with desktop.screenshot')
        text = source.read(length)
        crc = source.read(4)
    if len(crc) != 4 or zlib.crc32(kind + text) != struct.unpack('!I', crc)[0] or not text.startswith(b'arlinux.desktop\0'):
        raise ValueError('Screenshot geometry is missing or damaged')
    geometry = json.loads(text.split(b'\0', 1)[1])
    if (width, height) != (geometry['image_width'], geometry['image_height']):
        raise ValueError('Screenshot was resized; take a new screenshot')
    if not math.isfinite(x) or not math.isfinite(y) or not 0 <= x < width or not 0 <= y < height:
        raise ValueError('Point is outside the screenshot')
    desktop_width, desktop_height = geometry['desktop_width'], geometry['desktop_height']
    _request({'operation': 'click', 'x': x * desktop_width / width, 'y': y * desktop_height / height,
              'width': desktop_width, 'height': desktop_height,
              'button': button, 'count': count})
