# Arlinux buffer transport protocols

This package is the wire definition shared by Arlinux Xwayland, anlabwc,
Mesa's Arlinux WSI patch and the libhybris Wayland/X11 clients.

The [runtime protocol](../docs/RUNTIME-PROTOCOL.md#graphics-acceleration-and-presentation)
defines the host/guest contract. [Graphics acceleration](../docs/GRAPHICS.md)
explains the rendering paths and limitations. This package contains only the
wire definitions consumed by graphics implementations:

- [`include/arlinux/tawc-dri.h`](include/arlinux/tawc-dri.h): TAWC-DRI 0.4
  request/reply/XGE layouts, constants
  and compile-time wire-size checks. Xwayland's `tawcdriproto.h` only aliases
  these types to X server names.
- [`wayland-android.xml`](wayland-android.xml): android_wlegl v3. Each consumer
  generates its own client/server bindings from this XML; none vendors a second
  XML copy.

Set the target pkg-config search path to this directory. Meson consumers use
`dependency('arlinux-wsi-protocols')`; libhybris uses the same package through
Autoconf. Arlinux's build scripts supply the path. A standalone libhybris build
accepts `ARLINUX_WSI_PROTOCOL_DIR` and snapshots the package with its inputs.

TAWC-DRI 0.4 adds `PresentBuffer2` with an explicit OPAQUE flag. The original
`PresentBuffer` retains its premultiplied-alpha behavior regardless of X visual
depth. Each queued frame carries its own composition mode; a later legacy or
flags-zero frame clears the prior opaque region. Buffer bytes are unchanged.
Clients requiring OPAQUE must negotiate version 0.4 before using the new request.
