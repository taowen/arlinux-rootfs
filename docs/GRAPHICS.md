# Graphics and sandbox compatibility

Arlinux supplies a shared graphics stack to distributions. Native Wayland
clients connect to the Android-hosted compositor (anlabwc or anhyprland).
X11 clients connect to Xwayland, which presents their windows through the same
compositor. Distributions should use the supplied display environment and GPU
overlay instead of starting another compositor or replacing its graphics libraries.

OpenGL and OpenGL ES use Mesa Zink, translating rendering to Vulkan. Native
Wayland applications use EGL; X11 applications can use GLX through Xwayland.
Two-dimensional clients may also use shared-memory buffers without OpenGL.

On supported Qualcomm devices, Mesa Turnip accesses the GPU through KGSL.
The host selects the appropriate Vulkan ICD in `arlinux_icd.json`; the
libhybris path provides access to Android Vulkan drivers where applicable.
The runtime sets the driver search paths and compatibility layer environment.
Applications should inherit those settings.

Libhybris enables its implemented compatibility passes by default, including
missing scaled/packed vertex formats, BC texture fallback and shader rewrites.
No per-application enablement list is required. Native format support and
existing eligibility checks remain authoritative; forced emulation and debug
logging stay opt-in. Individual passes can be disabled for diagnosis.
See [the switch policy and limitations](../third_party/libhybris/docs/compatibility-defaults.md).

## Chromium and Electron

Native Wayland Chromium uses its default ANGLE OpenGL backend with Mesa
EGL/Zink and Turnip. Only `--ozone-platform=wayland` is needed to select Wayland;
no `--use-gl`, `--use-angle`, or `BIONICX_CHILD_FLAGS` override is required for
3D rendering. Electron applications must be verified separately because they
ship their own Chromium version.
OpenCode Desktop (Electron 42.3.3) was also verified on the Redmi with the
default backend: Zink/Turnip renderer, successful WebGL2 pixel readback, and
zero reported GPU process crashes.

The Mesa EGL device enumerator includes the existing fd-less device in
Zink-only builds. Previously this device was hidden unless swrast was compiled,
so ANGLE found no device on KGSL systems without DRM render nodes. The device
platform already supports creating a Zink screen without a DRM fd. Vulkan
selects the actual GPU. This fixes the standard EGL discovery path for all
clients rather than recognizing Chromium processes.

`ANGLE_DEFAULT_PLATFORM=vulkan` does not override Chromium's explicit OpenGL
backend selection. It is not needed with the EGL device discovery fix. The
shared runtime already selects Zink and the Vulkan ICD for applications.

The runtime cannot support arbitrary additional guest seccomp filters: its
syscall translation and SIGSYS handling do not implement the same contract as
executing guest syscalls directly against a desktop Linux kernel. It reports
`ENOSYS` for the intercepted seccomp syscall and `EINVAL` for
`prctl(PR_SET_SECCOMP)`, consistently before and after virtual namespace setup.
Applications can detect that capability and use their own fallback. Programs
that require seccomp may refuse to run; the runtime does not pretend to install
a filter. This policy does not identify applications, rewrite sandbox flags,
or hide worker threads. It covers the runtime's intercepted interfaces, not
arbitrary inline syscall instructions.

`PR_GET_SECCOMP` retains the real kernel result. Android-launched and `adb
run-as` processes can inherit different filters. The Android application UID
and SELinux context do not provide the same intra-application isolation as
Chromium's renderer or GPU sandbox.

## Verification

Use `teapot-egl` and `teapot-glx` to check the native EGL and GLX paths and
inspect their reported renderer. For Chromium, inspect `chrome://gpu` or the
DevTools `SystemInfo.getInfo` result, then exercise WebGL and read back pixels.
Driver loading or a visible window alone does not establish hardware rendering.

On the Redmi Adreno 650 with Debian Chromium 153.0.8010.47, native Wayland plus
the default ANGLE OpenGL backend rendered a WebGL2 clear through Zink/Turnip
and returned the expected RGBA pixel
`[255, 0, 0, 255]`, with no GL error or context loss and zero reported GPU
process crashes. No backend overrides or sandbox-disabling switches were used.
The earlier explicit ANGLE Vulkan experiment also passed with both the old
runtime and the consistent capability-reporting implementation.
Chromium reported its GPU process as unsandboxed: its own fallback was active.
The multiple-thread sandbox initialization warning was not a rendering failure.

In that experiment Chromium still reported software window compositing and
WebGL with readback. Hardware WebGL rendering does not establish accelerated
window presentation. The missing DRM render node and presentation path require
separate investigation; disabling the GPU sandbox is not a demonstrated fix.

Wayland protocol tracing confirmed that Chromium's main surface attaches
`wl_shm` buffers (1145 by 1060 pixels in the experiment). It does not submit
that window through the compositor's advertised `android_wlegl` protocol.
AHardwareBuffer support elsewhere in the host does not make this client path
zero-copy. WebGL rendering, window composition, buffer transport, and Android
presentation must be verified independently. In particular, the reported
`enabled_readback` WebGL path must not be described as avoiding pixel readback.

## Buffer interoperability boundaries

The X11 accelerated path currently uses the custom `TAWC-DRI` extension. It is
an adapter to `android_wlegl`, not standard DRI3/Present. Both paths share Mesa's
Arlinux WSI allocation/import implementation and compositor AHB import. Do not
treat the name "DRI" as evidence of compatibility with arbitrary DRI3 clients.

The host must give each surface one presentation owner. While a TAWC native
presenter is alive, ordinary X damage must not replace its AHB with the
software backing pixmap. The host tracks this through the existing release
event selection and resumes ordinary X presentation when that selection ends.
Both Redmi and X300 passed a same-window GLX-to-software transition check.

The X300 OpenCode Home-page duplication was traced above compositor import:
a Vulkan capture reproduced the incorrect pixels in offscreen replay. Mesa's
threaded context advanced render-pass metadata after the first draw/clear of
the next pass, so Zink could use the preceding pass's resolve target. Advancing
the metadata before that command fixes this case without application matching,
backend flags or disabling render-pass optimization. Both Home and New Session
passed 180 resize cycles per page on X300/Mali and Redmi/Turnip with the rebuilt
library; GPU compositing remained enabled, with zero reported GPU crashes.
GLX pbuffer pixel checks also passed on both. This is application-specific
verification coverage, not a claim of complete Vulkan or compositor conformance.

On the Redmi Adreno 650, the Android vendor EGL driver advertises native AHB
image import and native fence sync, but **not** `EGL_EXT_image_dma_buf_import`.
Guest Turnip's DMA-BUF support does not supply this missing host capability.
An AHB native handle also contains vendor metadata: a bare DMA-BUF descriptor
cannot safely be wrapped as an AHB by inventing handle fields.

The same device's vendor Vulkan driver also lacks DMA-BUF import. An isolated
Android Turnip build, however, successfully exported and reimported a DMA-BUF,
copied a GPU-generated striped pattern into an AHB image, and exposed the
result to the vendor Android EGL driver. Pixel checks passed at 64x32, 63x37,
and 127x65; the last case used GPU-only AHB usage. CPU reads were used only for
validation, not the bridge. This demonstrates a feasible GPU-copy bridge, not
zero-copy presentation. It does not yet prove arbitrary client buffers,
modifiers, cross-process synchronization, or loading from an APK's restricted
classloader namespace. The experiment ran as a standalone `adb run-as` process
without changing the system GPU driver. The isolated Turnip backend is not yet
loaded by the compositor.

The vivo X300 (V2509A, MT6993, Mali-G1-Ultra MC12, Android 16) does not need
Turnip for the same bridge: its vendor Vulkan driver advertises DMA-BUF import,
DRM modifiers, and AHB import. Its EGL driver still lacks DMA-BUF import, so the
Vulkan-to-AHB bridge remains useful. The native Vulkan probe passed the same
three sizes, including GPU-only AHB usage, followed by vendor EGL pixel checks.
Because this driver advertises import but not export for the tested transfer
buffers, the source came from `/dev/dma_heap/system` rather than Vulkan export.
This worked without root under `adb run-as`; heap access in an APK context must
be verified independently. Import and export capabilities must not be conflated.

Guest applications on this device continue using Zink over libhybris and the
vendor Vulkan ICD. The installed September 12 ICD/compatibility-layer pair
failed Zink device creation with `VK_ERROR_FEATURE_NOT_PRESENT`. Updating that
pair to the current build restored both native Wayland EGL and X11 GLX teapot
rendering, shader compilation, and continuous swapping (about 37 FPS in the
short 560x400 diagnostic). The reported renderer was Mali through Zink, not
Turnip or a CPU renderer. This is a smoke test, not an application-wide benchmark
or proof of standard DRI3/linux-dmabuf presentation. The original libraries were
retained on the device for rollback.

Backend policy for the compositor bridge is capability-based:
prefer the vendor Vulkan driver when the required import operations work;
consider an isolated Turnip backend only on supported Adreno hardware. Keep
the existing AHB transport and software fallback when neither bridge is
available. Do not require Turnip, DMA-BUF export, or a DRM render node merely
to import an already-produced DMA-BUF.

The anlabwc Android renderer now contains an optional vendor-Vulkan GPU-copy
bridge. It advertises `linux-dmabuf` version 3 only when the vendor supports the
required import operations. Version 3 carries formats and modifiers without
inventing the DRM device identity required by version 4 feedback. The initial
implementation accepts explicit LINEAR, single-plane, 32-bit RGBA/BGRA buffers,
including padded rows and nonzero offsets. Unsupported modifiers are rejected.
It waits for implicit producer fences, copies into an AHB on the GPU, waits
for completion, then imports that AHB through the existing Android EGL path.
There is no CPU pixel readback, but this is a GPU copy, not zero-copy, and its
waits are synchronous. anhyprland has not yet adopted this bridge. The custom
AHB and shared-memory paths remain available independently.

On X300, this path was verified inside the installed Android APK: a separate
standard Wayland client submitted a 321x193 LINEAR ABGR8888 DMA-BUF with a
1344-byte stride and 64-byte offset. The compositor advertised version 3,
returned a frame callback, and displayed the expected red/green checkerboard.
The client exited successfully. A fresh Debian bundle with the generic GPU
overlay also passed native Wayland EGL and X11 GLX teapot hardware-renderer,
shader and swap checks using Zink/Mali. On the Redmi the new host retained the
old AHB path, passed both teapots with Zink/Turnip, and did not advertise the
unsupported system-Vulkan DMA-BUF path. This does not yet establish Chromium
hardware window composition or standard X11 DRI3 support.

X11 DRI3/
Present additionally needs a working allocation and pixmap-import contract;
enabling its protocol alone is insufficient. Chromium's GBM/render-node
requirements remain a separate compatibility issue. These standard paths are
not implemented by the EGL device-discovery fix.

Synchronization is part of that contract. The existing custom transports carry
release notifications but no acquire fence, so producer completion is waited
before presenting. The Android renderer finishes sampling before releasing
buffers. Removing these waits without transporting fences would allow buffer
reuse while the GPU still reads them. Native fence support by itself does not
make the current transport asynchronous.

The Android renderer's shared-memory fallback uploads a whole texture in one
call when rows are packed or GLES supports row length. Older GLES2 drivers use
row-by-row upload for unsupported strides. This reduces GL call overhead for
software-presented clients, but still transfers CPU pixels and is not zero-copy.
