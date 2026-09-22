# Android compatibility policy

This preload library adapts a guest process to its Android app sandbox. It is
loaded by the patched glibc loader, not linked into distribution applications.
Applications continue using the normal Linux ABI.

| Owner | Responsibility |
| --- | --- |
| `runtime/glibc` | Loader, libc-internal behavior, syscall path translation, identity queries, IPC and kernel descriptor fallbacks |
| This directory | Synthetic filesystem/account metadata, environment policy, nested process launch and remaining Android policy |
| Android host | The first native process launch, desktop sockets, lifecycle and Android services |

The exported symbols are listed in `glibc-interpose.map`. Public libc calls and
the exported `syscall()` adapter must use the same backend and state. For
example, all SysV shared-memory and semaphore calls delegate to libc. Do not
add a second fallback table for a different entry point. Timer file descriptors
use glibc and the kernel directly; there is no eventfd/thread emulation.

Do not add per-function wrappers merely to translate a pathname. libc owns
that boundary, including directory, temporary-file and POSIX IPC operations.
Resolver configuration also belongs to libc and the host-provided
`resolv.conf`, not a process constructor or `res_init` override.

Interposing `syscall()` does not intercept inline assembly or libc's hidden
internal syscalls. Fix behavior needed by libc itself in the glibc build, rather
than assuming an exported preload symbol covers it. See the
[glibc boundary and current IPC limitations](../../runtime/glibc/README.md).

The real-device acceptance runner lives in the Android host repository because
it installs and starts the APK. It checks the installed runtime under the app's
actual seccomp policy, not merely a shell with a matching UID.
