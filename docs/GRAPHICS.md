# Graphics acceleration

This page explains the implementation and current limits of the
[runtime graphics contract](RUNTIME-PROTOCOL.md#graphics-acceleration-and-presentation).
The [graphics protocol package](../graphics-protocols/README.md) owns the wire
definitions and version details.

Arlinux provides the graphics stack and its environment as part of every
distribution bundle. Applications should be started normally. Distribution
profiles must not add application-specific GPU flags, select an ANGLE backend,
or disable a sandbox to obtain acceleration.

## Rendering

Vulkan applications use the Vulkan ICD selected by the runtime:

- Qualcomm devices use Mesa Turnip over KGSL.
- Other supported Android GPUs use the device's vendor Vulkan driver through
  libhybris.

OpenGL and OpenGL ES use Mesa Zink, which translates GL commands to Vulkan and
therefore uses the same selected driver. Both Turnip and libhybris execute in
the application process, or in an application-owned GPU worker process.

The runtime supplies the driver search paths and GPU environment to the entire
session. Driver availability does not, by itself, guarantee accelerated window
presentation: an application must also support an available presentation path.
The distribution supplies its standard Wayland, X11, and XCB client libraries;
the GPU overlay does not replace them with copies from the build environment.

## Presentation

Wayland and X11 share one accelerated presentation contract:

1. The server allocates an Android Hardware Buffer (AHB) for the client image.
2. Mesa or libhybris imports that AHB and the GPU renders into it.
3. The Android renderer imports the same AHB and composites it on screen.
4. Buffer release lets the producer reuse the image.

Native Wayland clients use the `android_wlegl` path. Accelerated X11 EGL and
GLX surfaces run through Xwayland and use `TAWC-DRI`, an X11 adapter to the
same AHB allocation and presentation path. The contract is implemented by
both anlabwc and anhyprland.

Normal accelerated presentation does not map the image on the CPU, read its
pixels back, or copy a full frame through shared memory. A GPU-side layout copy
may be used when required by a driver, without making the image CPU-backed.

The current protocol carries buffer release events but not explicit acquire
fences. A producer therefore waits for GPU completion before presenting, and
the compositor finishes sampling before releasing the buffer. These waits
provide synchronization; they are not pixel copies.

## Software buffers and standard protocols

Applications that render with `wl_shm`, including ordinary 2D clients, use the
software-buffer path. Screenshots and capture tools may also read pixels by
design. Neither case changes the default path used by accelerated surfaces.

The build retains standard Linux DMA-BUF and DRI3 support for backends that can
satisfy those protocols. They are not the primary Android transport: Android
vendor handles can contain metadata that cannot be reconstructed from a bare
DMA-BUF descriptor, and vendor EGL implementations do not consistently support
DMA-BUF import. AHB is therefore the portable host boundary.

The Android renderer does not currently expose a GBM render device and
`linux-dmabuf` import. Chromium/Electron's native Wayland GPU compositor requires
these interfaces; it can therefore use hardware GL while still presenting its
window through software buffers. The AHB EGL/Vulkan paths above do not cover
that consumer yet. An accelerated GLX or EGL probe alone is not evidence of
accelerated browser window composition.

For the supported paths, no user or distribution configuration is required.
Inherit the Arlinux session environment and launch the application with its
normal command line.
