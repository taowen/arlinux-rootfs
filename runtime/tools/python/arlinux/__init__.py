"""Arlinux-specific Python integration.

Linux GUI accessibility intentionally uses the upstream ``dogtail`` and
``pyatspi`` packages directly.  This package contains only behavior that those
mature APIs do not provide on Arlinux.
"""

from __future__ import annotations

from . import _tts, desktop


__all__ = ("speak", "desktop")


def speak(
    text: str,
    *,
    voice: str = "zh-CN-XiaoxiaoNeural",
    rate: str = "+0%",
    volume: str = "+0%",
    pitch: str = "+0Hz",
) -> bool:
    """Speak a short progress update in the background and return immediately.

    Edge TTS performs online synthesis and mpg123 plays through PulseAudio
    without opening a GUI. A new call automatically interrupts an older call,
    including one from another process; callers must not coordinate playback.
    The return value only confirms that the background worker was started.
    """
    return _tts.speak(
        text,
        voice=voice,
        rate=rate,
        volume=volume,
        pitch=pitch,
    )
