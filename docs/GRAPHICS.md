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

## Chromium and Electron

Native Wayland Chromium can use `--ozone-platform=wayland --use-gl=angle
--use-angle=vulkan` to render through ANGLE and Turnip. These choose the display
and rendering backend; they do not disable sandboxing. Electron applications
must be verified separately because they ship their own Chromium version.

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
ANGLE Vulkan rendered a WebGL2 clear and returned the expected RGBA pixel
`[255, 0, 0, 255]`, with no GL error or context loss and zero reported GPU
process crashes. No sandbox-disabling switches were used. The existing runtime
and the consistent capability-reporting implementation both passed this check.
Chromium reported its GPU process as unsandboxed: its own fallback was active.
The multiple-thread sandbox initialization warning was not a rendering failure.

In that experiment Chromium still reported software window compositing and
WebGL with readback. Hardware WebGL rendering does not establish accelerated
window presentation. The missing DRM render node and presentation path require
separate investigation; disabling the GPU sandbox is not a demonstrated fix.
