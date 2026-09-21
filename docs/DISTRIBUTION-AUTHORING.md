# Distribution authoring

This guide describes the public interface between an Arlinux distribution and
`arlinux-rootfs`. It deliberately assumes no access to the Android host source.
The normative contracts are the [static bundle protocol](../STATIC-PROTOCOL.md)
and [Android–Linux runtime protocol](../RUNTIME-PROTOCOL.md).

## Design model

An Arlinux distribution is a normal AArch64 Linux userspace running on the
Android kernel. The Android application supplies a Wayland compositor, input,
audio, graphics device access, lifecycle management, and a process boundary.
The distribution supplies the root filesystem and starts ordinary Linux
programs.

The rootfs is not a container image and does not boot an init system. Package
managers still own their files and databases, but Android owns the kernel,
identity, networking device configuration, power management, and application
lifecycle. Do not require systemd as PID 1, a display manager, DRM/KMS, logind,
kernel modules, or privileged mounts.

Applications run with the Android app's real UID, not host root. The runtime
exposes the current guest account as `bionicx`, with the instance home directory;
standard passwd lookups by name or UID describe the same account. Do not
hard-code an Android `u0_aNNN` name or assume the UID is 1000.

Image loaders based on recent Glycin require an explicit platform choice:
Android app UIDs cannot create Bubblewrap's nested mount namespace. LXQt uses
the upstream `GLYCIN_DISABLE_SANDBOX=i-know-the-risks` setting in its launch
environment so GTK icons and images can load. The Android app sandbox still
applies, but image decoding has **no additional loader sandbox** and shares the
app's access to guest data. This is not equivalent to Bubblewrap isolation.

## Required layout

Create a Git repository under `distributions/<id>`. The directory name is the
stable distribution ID and must match `[a-z][a-z0-9-]{0,63}`.

```text
my-linux/
├── LICENSE
├── README.md
├── product.json
├── profile.json
├── rootfs.lock.json
├── guest/
│   └── first-boot.sh
├── native/
│   └── product-policy.h
└── tools/
    ├── seed.sh
    └── post-seed.sh        # optional
```

Run `./build.sh validate distributions/my-linux` before a full build.

## `product.json`

`product.json` declares data needed by the generic builder and host. It must
conform to [`schemas/product.schema.json`](../schemas/product.schema.json).

```json
{
  "$schema": "../../schemas/product.schema.json",
  "name": "My Linux",
  "compositor": "anlabwc",
  "glibcVersion": "2.43",
  "libraryDirectories": ["usr/lib"],
  "requiredFiles": ["usr/bin/sh", "etc/os-release"],
  "environment": {
    "LOCPATH": "${RUNTIME}/usr/lib/locale"
  }
}
```

- `glibcVersion` must have a matching `runtime/glibc/<version>/recipe.env` in
  `arlinux-rootfs`. It must match the distribution libc ABI.
- `compositor` is a default hint. Use `anlabwc` for a stacking desktop and
  `hyprland` for a Hyprland session.
- `libraryDirectories` are rootfs-relative dynamic-library directories.
- `requiredFiles` are startup sanity checks and therefore must already exist in
  the rootfs produced by `tools/seed.sh` and the optional post-seed hook. Do not
  list packages that `guest/first-boot.sh` installs later.
- `environment` is merged into the launch profile at bundle time.

Never put an Android application ID in a distribution repository. Multiple
distributions and instances share one host application.

## Seed the root filesystem

`tools/seed.sh OUTPUT` must create an AArch64 root filesystem at `OUTPUT`. The
builder invokes it as the regular build user. It may download a signed upstream
bootstrap, run `debootstrap --foreign`, or assemble another package-manager
owned seed. It must not use QEMU, chroot into AArch64, contact an Android device,
or depend on the private host repository.

Make network inputs reproducible. Record immutable URLs, upstream versions,
commits, and SHA-256 hashes in `rootfs.lock.json`. A rolling repository should
use a tested archive snapshot instead of an unbounded `latest` source whenever
the upstream supports snapshots.

The output should exclude kernel images, firmware, boot loaders, device nodes,
and service state that belongs to a booted machine.

### Optional post-seed hook

If present, executable `tools/post-seed.sh ROOTFS` runs immediately after
`seed.sh`. Use it for preparation that relies on shared framework tools. For
example, Arch recipes initialize package signing keys with:

```bash
#!/usr/bin/env bash
set -euo pipefail
rootfs=${1:?rootfs required}
framework=$(cd "$(dirname "$0")/../../.." && pwd)
"$framework/tools/build/seed-pacman-keyring.sh" "$rootfs" archlinuxarm archlinux
```

The framework never branches on a distribution ID.

The shared builder also packages both GPU overlays: `gpu-qualcomm.tar.zst`
(Mesa Turnip) and `gpu-generic.tar.zst` (Android vendor Vulkan via libhybris),
each with its corresponding `*-id` file. Both are required bundle payloads;
the host selects the device-appropriate one at installation. Distribution
authors do not need separate Qualcomm and Mali recipes. See
[the runtime protocol](../RUNTIME-PROTOCOL.md#graphics-acceleration-and-presentation)
for the host contract and [graphics support](GRAPHICS.md) for implementation
details and current limitations.

## First boot

Files under `guest/` are installed at `/usr/lib/arlinux/guest`. An executable
`guest/first-boot.sh` completes architecture-native package configuration on
the Android device. It receives these important variables:

- `BIONICX_ROOTFS`: absolute rootfs path;
- `BIONICX_FILES`: host application files directory;
- `HOME`: persistent home for this instance;
- `XDG_RUNTIME_DIR`: private session sockets and state (mode `0700`).
- `TMPDIR`: temporary files, also accessible as `/tmp`. This is a separate
  directory; never repoint it to `XDG_RUNTIME_DIR` or change the latter's mode.

If installing or upgrading libc replaces the loader beneath the current
process, exit with status `75`. The host restarts first boot at a fresh process
boundary. Any other non-zero status is a failure. The script must be
idempotent because it can be interrupted and run again.

Use the distribution package manager rather than copying shared libraries by
hand. Disable only service hooks that fundamentally require a booted Linux
system, and document each exception.

Install the distribution's normal Wayland and X11 client libraries. The shared
GPU overlays contain Mesa and the device driver, but deliberately do not bundle
display client libraries from the framework's build environment. This keeps the
desktop and its toolkit libraries on one package-manager-owned ABI generation.

## Launch profile

`profile.json` follows [`schemas/profile.schema.json`](../schemas/profile.schema.json).
Paths use host-expanded variables such as `${RUNTIME}`, `${HOME}`, `${FILES}`,
and `${DISPLAY}`. A Wayland desktop normally sets:

```json
{
  "schemaVersion": 3,
  "id": "desktop",
  "name": "Desktop",
  "display": {"dpi": 144, "socket": "filesystem"},
  "launch": {
    "executable": "${RUNTIME}/usr/bin/dbus-run-session",
    "workingDirectory": "${HOME}",
    "arguments": [
      "--",
      "${RUNTIME}/usr/lib/arlinux/accessibility-session.sh",
      "${RUNTIME}/usr/lib/arlinux/guest/session.sh"
    ],
    "environment": {
      "HOME": "${HOME}",
      "XDG_RUNTIME_DIR": "${FILES}/runtime",
      "WAYLAND_DISPLAY": "wayland-0",
      "XDG_SESSION_TYPE": "wayland",
      "GDK_BACKEND": "wayland,x11",
      "QT_QPA_PLATFORM": "wayland",
      "PATH": "${RUNTIME}/usr/bin:${RUNTIME}/bin:/system/bin"
    }
  }
}
```

The compositor is already running in the Android host. Start the desktop
session or application, not a second Linux compositor.

Use the desktop's upstream session entry point so its configuration search
paths, menu prefix, and theme defaults are initialized together. Install the
icon and theme packages those defaults reference. With LXQt, `startlxqt`
provides these defaults; its session configuration can declare
`XDG_CURRENT_DESKTOP=LXQt:wlroots` in `[Environment]` to select the panel's
standard Wayland window-management backend on the host compositor.

Applications use ordinary Linux paths and the platform-provided loader/libc.
The host installs and selects those runtime libraries. Do not prepend the
distribution's original libc directory to `LD_LIBRARY_PATH`: this bypasses
the shared filesystem and process integration, including libc-internal calls.
Applications should launch with their usual commands; graphics defaults come
from the runtime described in [graphics support](GRAPHICS.md).

## Native compatibility policy

`native/product-policy.h` is compiled into the bionicx runtime. Most
distributions should start with the no-op policy:

```c
#pragma once
#define ARLINUX_PRODUCT_ENVIRONMENT
static inline void arlinux_product_environment(void) {}
```

Only add policy when a package manager requires environment setup before its
own process starts. Keep it narrowly scoped by executable name. General Linux
compatibility fixes belong in `arlinux-rootfs`, not in a distribution policy.

## Host services and desktop integration

These services implement the dynamic contract summarized here. Refer to the
[runtime protocol](../RUNTIME-PROTOCOL.md) for normative endpoint, lifetime,
and message semantics.

- Wayland and Xwayland clients are supported by the host compositor.
- PulseAudio clients connect to the socket supplied under the host runtime
  directory; reference distributions install the standard ALSA Pulse plugin.
- AT-SPI runs on the session D-Bus bus through `accessibility-session.sh`.
- Hosted Android keyboard input can use the shared IBus engine at
  `/usr/lib/arlinux/hosted-ime.py`. Install IBus, its Python introspection
  bindings and the GTK input modules, then install the accompanying
  `org.arlinux.HostedInput.service` in `/usr/share/dbus-1/services`.
  Before launching the desktop, call `org.freedesktop.DBus.Peer.Ping` on the
  session-bus destination `org.arlinux.HostedInput`, path `/org/arlinux/HostedInput`.
  D-Bus activates the engine and waits for readiness; Qt clients must not start
  before their input service is available. Set `QT_IM_MODULE=ibus`,
  `GTK_IM_MODULE=ibus` and `XMODIFIERS=@im=ibus` in the desktop environment.
  The engine starts IBus in its standard daemon mode and registers itself;
  applications discover it through their normal input modules. No per-application
  socket address or source modification is needed. LXQt provides this setup.
- `wl-clipboard` and `wtype` are suitable standard tools for clipboard and
  key injection inside the guest.
- Android owns network, Bluetooth, brightness, suspend, reboot, and device
  lock. Desktop components for those operations should be disabled unless they
  are purely informational.

## Validation checklist

Before publishing a distribution:

1. `./build.sh validate distributions/<id>` succeeds.
2. `./build.sh build <id>` succeeds from a clean x86-64 Linux checkout.
3. `./build.sh verify out/<id>.zip` verifies every payload hash.
4. The repository contains no host source paths, device serials, credentials,
   private submodules, or Windows-specific build commands.
5. A fresh instance completes first boot, and rerunning first boot is safe.
6. Native Wayland, Xwayland, audio, clipboard, and accessibility are tested.
7. Rotation and output resize do not require restarting the desktop session.
8. License and third-party notices are preserved.

`arlinux-lxqt` is maintained as the external-reference test: it is developed as
an independent repository using only this documented interface.
