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
environment. The host-packaged tawcroot owns Linux syscall adaptations; no compatibility
preload library is injected. Distribution profiles must not override reserved
`BIONICX_*` or `LD_LIBRARY_PATH` values, or inject `LD_PRELOAD`. Processes should
inherit the session environment and launch applications with normal command lines.

### Sandbox compatibility is not isolation

The Android app sandbox remains the security boundary. Linux instances and
projects are not separate security boundaries; run only trusted guest software.
The runtime accepts guest seccomp filter installation without enforcing those
filters, because they can conflict with its syscall translation. Android's own
restrictions remain active. Seccomp notification listeners are not supported.

The shared builder installs `/usr/local/bin/bwrap`, a non-isolating command
launcher. Distribution `PATH` includes `/usr/local/bin` before `/usr/bin`.
It supports command arguments, environment, working directory, process lifecycle
and identity binds (source and destination already refer to the same object).
Namespace, read-only mount and directory-masking options add no isolation; the
launcher reports this on stderr. It never changes shared directory permissions
to imitate a private mount. Unknown setup options and non-identity binds fail.
This is not general Flatpak support or a replacement for real Bubblewrap security.

Optional applications remain user-installed. No Codex configuration is installed;
applications discovering `bwrap` through `PATH` use this compatibility entry point.

Session sockets are ephemeral. The host removes stale filesystem endpoints
when starting or switching an instance. A client must treat
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

## Native Steam client

`arlinux-steam` installs Valve's native ARM64 Steam client on first use and
starts its normal desktop interface. Subsequent invocations start the existing
client, including Steam's own updater. The client is downloaded on the phone,
not redistributed in a rootfs bundle or APK. First use needs network access
to the distribution package mirror and Valve's client CDN.

Installation reads Valve's stable ARM64 KeyValues manifest,
verifies the current native bootstrap's SHA-256 and stages it before writing
client files. Valve's updater downloads and verifies the remaining components
and owns its installation records. Downloads are cached under `$XDG_CACHE_HOME/arlinux/steam`;
client files live under `$XDG_DATA_HOME/Steam` (normally `~/.local/share/Steam`).
GTK2, PipeWire, libnm, lsof and normal X11/audio dependencies are installed
using Debian's apt. Arch uses pacman for available dependencies but requires
GTK2 to be installed separately; it is no longer in the main repositories.
Unrelated Steam links are not overwritten.
`arlinux-steam --update` refreshes the bootstrap and starts Valve's updater.
The default is Valve's stable ARM64 channel. `--channel publicbeta` opts into
the public beta; `--channel stable` returns to the stable client.

The launcher directly runs `steamrtarm64/steam` and handles updater exit code
42. It does not add sandbox/GPU arguments. ARM64 channel
discovery follows the approach demonstrated by
[DroidDeck](https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/droiddeck-steam-install).

On Debian-based desktops, native x86-64 Linux games can use the registered
**ARLinux Linux x86-64 (FEX)** tool in Steam's per-game Compatibility settings.
Install **FEX-Emu** (3127680) and **Steam Linux Runtime 3.0 (sniper)** (1628350)
through Steam first. The first game launch prepares a private runtime under
`$XDG_CACHE_HOME/arlinux/steam-fex`, downloading signed Debian Bookworm base
libraries through APT from the Tsinghua mirror. It does not replace the
desktop's ARM64 libraries. Steam SDK links select x86-64 for `sdk64` and
ARM64 for `sdkarm64`.

The tool runs Valve's FEX directly, without pressure-vessel: Android apps
cannot create its user namespaces. This is **not a game sandbox**. FEX's
GL/Vulkan thunks use the desktop's native ARM64 graphics stack; guest library
paths are kept separate. This does not provide Proton, Windows-game support
or touchscreen game controls, and compatibility remains game-specific.
The Steam compatibility-tool approach also builds on
[DroidDeck's Linux FEX integration](https://github.com/Droid-Deck/DroidDeck/blob/main/tools/linuxfs/overlay/usr/local/bin/droiddeck-fex).

## Hosted Android applications

The host installs `/usr/bin/arlinux-app`. `arlinux-app PACKAGE [HTTP_URL]`
opens an installed Android application as a desktop window. The optional URL
is delivered as an explicit `ACTION_VIEW` Intent. Mouse, keyboard and AT-SPI
accessibility use the same hosted Activity session.

`arlinux-app --apk PATH.apk` runs a standalone signed development APK without
installing it in Android's system PackageManager. The wrapper resolves relative
paths. The APK must be inside the active instance's rootfs or home directory;
its package name must not collide with a system-installed application. Repeated
launches close the previous test process, replace the code and preserve private
test data. This does not emulate a full system installation or accept split
APK sets. See the [phone-native Java/Geany example](../examples/android-dev/README.md).

The launcher uses `$XDG_RUNTIME_DIR/hosted.sock`, a same-UID Unix stream owned
by the host. Requests are UTF-8 lines, at most 8,450 bytes, with fields separated
by a tab: `PACKAGE\tURL\n` or `@apk\tABSOLUTE_PATH\n`. Fields cannot contain
tabs, carriage returns or newlines. The reply is `OK\n` or `ERR MESSAGE\n`.
An APK's immutable snapshot, package registration and Activity process are
owned by the Android runtime, not the distribution. Prefer the installed CLI
over connecting to this endpoint directly.

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

## Optional Arch package transactions

An ordinary desktop process retains the Android app UID; it cannot run pacman
as root. The host can perform a narrowly scoped transaction with its existing
virtual-root launcher, which changes neither the Android UID nor the host
device's privileges. Omarchy's `omarchy-pacman` client connects to the
abstract Unix stream socket `arlinux-pacman-<app-uid>` (where `<app-uid>` is
`id -u`). The host accepts only peers with the same Android app UID.

The client sends one ASCII line: `update` or `install PACKAGE...`. Package
names contain only letters, digits, `@._+-`; options and arbitrary commands
are not accepted. The host runs `pacman -Syu --noconfirm` or
`pacman -S --needed --noconfirm PACKAGE...`, streams its output, then ends
with `Package transaction exited N`. The client must wait for the socket to
close and must not retry a disconnected transaction because its outcome may be
unknown. Other distributions do not need this optional service.

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
