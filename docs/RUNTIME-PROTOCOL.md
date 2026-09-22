# Android–Linux runtime protocol

This document defines the dynamic contract between a selected Linux instance
and the Arlinux Android host. The archive and installation format is defined in
[static bundle protocol](STATIC-PROTOCOL.md).

Distribution applications use standard Linux interfaces wherever one exists.
Arlinux-specific wire protocols are limited to boundaries where Android native
objects cannot be represented by a standard Linux interface.

## Session and lifetime

The host starts one selected instance and one compositor at a time. It owns the
Android application lifecycle, output size and rotation, input devices,
networking, audio device, and GPU device. The guest owns its desktop session and
ordinary Linux processes; it does not boot an init system or control Android
hardware policy.

The host creates a private, mode-`0700` runtime directory and exports at least:

| Variable | Contract |
| --- | --- |
| `BIONICX_ROOTFS` | Absolute path of the selected rootfs alias. |
| `BIONICX_FILES` | Android application files directory. |
| `BIONICX_TMPDIR` / `TMPDIR` | Guest temporary-file directory. |
| `XDG_RUNTIME_DIR` | Private directory containing session sockets. |
| `WAYLAND_DISPLAY` | `wayland-0`. |
| `DISPLAY` | The live Xwayland display selected by the host. Its number is not stable. |
| `PULSE_SERVER` | `unix:$XDG_RUNTIME_DIR/pulse-native`. |
| `HOME` | Persistent home directory of the selected instance. |

The host supplies the initial loader, library, timezone, DNS, and device-driver
environment. The bundled glibc owns Linux ABI adaptations; no compatibility
preload library is injected. Distribution profiles must not override reserved
`BIONICX_*` or `LD_LIBRARY_PATH` values, or inject `LD_PRELOAD`. Processes should
inherit the session environment and launch applications with normal command lines.

All filesystem sockets described below are ephemeral. The host removes stale
runtime endpoints when starting or switching an instance. A client must treat
disconnect as session termination and must not replay an uncertain operation
after reconnecting.

## Display and input

The Android host embeds either anlabwc or anhyprland and publishes the standard
Wayland socket `$XDG_RUNTIME_DIR/wayland-0`. Xwayland is started by the selected
compositor and publishes one live socket below `$XDG_RUNTIME_DIR/.X11-unix`.
Guest applications use ordinary Wayland, X11, XKB, pointer, keyboard, touch,
focus, clipboard, and window-management protocols.

Android touch is converted by the host into compositor pointer/button events.
Output resize and rotation are dynamic compositor events; applications must not
cache the initial Android surface size or require a desktop restart.

`wl-clipboard`, `wtype`, `xclip`, and `xdotool` remain ordinary guest tools.
There is currently no private Android clipboard wire protocol in the
distribution contract.

## Graphics acceleration and presentation

The host selects the bundled Vulkan implementation for the device: Mesa Turnip
on Qualcomm, or the Android vendor driver through libhybris on other supported
GPUs. OpenGL and OpenGL ES use Mesa Zink over the selected Vulkan driver. The
host configures the driver for the entire session; applications need no special
GPU command-line flags.

Accelerated window presentation passes Android Hardware Buffers (AHBs) from
the producer to the host compositor. Native Wayland clients use
[`android_wlegl` version 3](../graphics-protocols/wayland-android.xml). X11 EGL,
GLX, and Vulkan clients use [TAWC-DRI 0.4](../graphics-protocols/include/arlinux/tawc-dri.h)
through Xwayland, which forwards the handle to `android_wlegl`. Producers must
finish GPU work before presentation; the compositor must finish sampling before
buffer release. These versions provide no explicit acquire or release fences.

The distribution installs its normal Wayland, X11, XCB, EGL, GL, and Vulkan
client libraries and must not replace the runtime GPU overlay. Standard
`wl_shm`, Linux DMA-BUF, and DRI3 remain available when their backends support
them; AHB is the portable accelerated host boundary. See
[Graphics acceleration](GRAPHICS.md) for data flow, performance characteristics,
and current limitations, and the [graphics protocol package](../graphics-protocols/README.md)
for wire definitions.

## Hosted Android input method

Android hosts the supported IME UI so touch and voice input continue to use the
normal Android input-method lifecycle. Linux applications see a standard IBus
engine named `arlinux-hosted`; no application-specific integration is needed.

The distribution installs IBus, its toolkit input modules, Python GI bindings,
`/usr/lib/arlinux/hosted-ime.py`, and the D-Bus activation file for
`org.arlinux.HostedInput`. It exports:

```text
GTK_IM_MODULE=ibus
QT_IM_MODULE=ibus
XMODIFIERS=@im=ibus
```

Before desktop applications start, the session calls
`org.freedesktop.DBus.Peer.Ping` on destination `org.arlinux.HostedInput`, path
`/org/arlinux/HostedInput`. D-Bus activation starts IBus and publishes
`$XDG_RUNTIME_DIR/hosted-ime.sock`.

That socket is a same-UID Unix stream. Messages are UTF-8, newline-delimited
JSON, limited to 65,536 bytes. On connection and every focus transition, the
guest sends `{"focus":"<random-token>"}` or `{"focus":null}`. Every Android
edit must echo the current token and contain one operation:

```json
{"focus":"...", "operation":"commit", "text":"text"}
{"focus":"...", "operation":"preedit", "text":"composition"}
{"focus":"...", "operation":"finish"}
{"focus":"...", "operation":"delete", "before":1, "after":0}
{"focus":"...", "operation":"enter"}
{"focus":"...", "operation":"key", "keyval":65289, "keycode":15, "state":0}
```

The guest replies `{"accepted":true}` after applying an edit. A missing or stale
focus token is rejected with
`{"accepted":false,"reason":"focus-changed"}`. Focus tokens expire on every
focus transition, including returning to the same input context. Senders must
preserve order and must not retry a write whose delivery is uncertain.

## Audio

The host exposes a standard PulseAudio native-protocol socket at
`$XDG_RUNTIME_DIR/pulse-native` and routes its sink to Android AAudio. Guest
applications use `PULSE_SERVER`; ALSA applications can use the distribution's
standard PulseAudio plugin. Applications do not open the Android audio HAL.

## Accessibility

The guest runs the standard AT-SPI stack on its session D-Bus. The shared
`/usr/lib/arlinux/accessibility-session.sh` enables the accessibility bus before
starting the desktop and publishes `AT_SPI_BUS` on the X11 root window when
Xwayland is present. Applications expose their normal AT-SPI interfaces; no
Arlinux accessibility API is added.

Android-side automation executes guest Python against the same session and can
use the upstream `pyatspi`, D-Bus, Wayland, X11, and clipboard APIs. A
distribution must not require access to private Android host classes.

## Compatibility and ownership

Wire versions are negotiated by their native protocols (`android_wlegl` and
TAWC-DRI). Socket message extensions must be additive: receivers ignore unknown
JSON members, while a new required operation or changed meaning requires a new
endpoint/version. Standard Linux services follow their upstream compatibility
rules.

The host owns lifecycle, endpoint creation, Android resources, compositor and
device selection. The distribution owns packages, toolkit modules, desktop
startup and clients of these interfaces. Neither side should infer capabilities
from a process name or add per-application command-line workarounds.
