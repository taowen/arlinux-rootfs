# Android-compatible glibc

Each version directory pins an upstream glibc archive and its Android patch
recipe. `tools/build/linux-glibc.sh` builds the loader, libc, libm and ldconfig
for the bundle's host package. Distribution applications keep their standard
glibc ABI; they do not link against a separate Arlinux API.

## Source ownership

This directory is a source recipe, not a standalone glibc Git fork. The build
verifies a GNU glibc release archive, applies patches from a pinned
`termux-pacman/glibc-packages` commit, then installs the Arlinux adaptations.
`2.41/recipe.env` and `2.43/recipe.env` pin these inputs. The completed cache
contains the assembled `source/` tree and an `output/BUILD-INFO` manifest.

Arlinux currently integrates changes in three forms: small unified patches,
shared C implementations, and checked upstream-source edits in
`common/install-syscalls.py` and `tools/build/linux-glibc.sh`. Moving a patch
into that Python script would not eliminate its maintenance cost. A future
Git fork can represent these changes as commits, but must also absorb the
pinned Termux changes; copying only the files in `common/` is insufficient.

The three shared patch files still have distinct runtime responsibilities:

| Patch | Responsibility |
| --- | --- |
| `zz-arlinux-loader-search-path.patch` | Relocate FHS library names and RPATH/RUNPATH without losing the calling object's search context. |
| `zz-arlinux-ldconfig-prefix.patch` | Make package-manager cache generation use physical guest paths, including `DPKG_ROOT` and Android path aliases. |
| `zz-bionicx-robust-fallback.patch` | Stop assuming kernel robust-list registration and reject process-shared/priority-inheritance modes that cannot be implemented correctly. |

The version directories link to those shared patches; they are not duplicate
copies. Version-specific patches fix Android group membership and, for 2.43,
the fortified syslog export. Removing any of these requires replacing its
behavior or verifying that the selected upstream inputs already provide it.

glibc owns loader search paths, thread initialization, filesystem syscall
translation, process execution, identity queries, IPC and kernel fallbacks.
There is no compatibility preload library or libc symbol-interposition layer.
The Android launcher initializes the session environment and starts the first
process; subsequent processes use glibc's normal execution APIs.

## Execution

`common/android-exec.c` handles the Android executable boundary for public,
hidden libc and `syscall()` entry points. Upstream `execvp`, `posix_spawn`,
`popen` and `system` retain their own PATH, descriptor and child-wait semantics.
GNU dynamic executables enter the bundled loader; Android and static
executables keep their native execution path. Shebang interpreters use the
same guest path translation.

Bundle assembly preserves distribution shebangs and absolute symlink targets.
The Android launcher's first-exec adapter selects the guest interpreter without
editing the script; glibc owns subsequent execution and pathname resolution.

Execution preparation uses stack-owned scratch, not malloc or persistent
mappings. The upstream spawn stack reserves the scratch area and the actual
argument/environment vectors, so shared-VM children do not leak allocations
into their parent. Explicit child environment values remain authoritative; only
the rootfs and execution identity needed for the ABI are carried across a
sanitized environment. GPU, TLS and session defaults are set by the host
launcher, not recreated by libc constructors.

## Filesystem boundary

`common/android-syscall.c` translates guest filesystem paths immediately before
the AArch64 kernel call. Public functions, hidden libc calls and `syscall()`
therefore use the same translation. The loader and libc share
`common/android-path.h` for FHS and symlink lookup; the loader uses it directly
before libc policy is initialized. Translation uses bounded stack storage and
does not allocate, read environment variables or call the dynamic loader.

`common/install-syscalls.py` installs the boundary in both supported upstream
versions. It validates each source edit and fails if the upstream contract
changes. Cancellation still uses glibc's original cancellation region; only
the pathname arguments are prepared beforehand.

Directory creation, renaming, temporary-file APIs and executable procfs aliases
use that boundary directly. Pathname Unix sockets use the same translation;
abstract socket names are unchanged. Long physical socket paths use a temporary
directory descriptor, released on return or thread cancellation.

If Android denies `/proc/version`, the boundary supplies an app-private kernel
identity line derived from `uname`. It is a compatibility view, not a byte-exact
copy of the inaccessible procfs file or a bypass of SELinux. On systems where
the real file is readable, it remains authoritative.

Path lookup resolves guest absolute symlink targets, including intermediate
directories. Link contents stay unchanged; no-follow operations still act on
the link itself. Applications can therefore use distribution-default certificate
and configuration paths even when their environment has been cleared.

Package transactions in virtual-root mode can publish a regular-file backup
through an atomic, exclusive copy when Android forbids hard links. This is
not shared-inode hard-link semantics and is not used for named semaphores.

The Android host writes `resolv.conf`. The standard libc resolver reads it;
process constructors do not rewrite it and `res_init` is not interposed.

## Identity

The same syscall boundary owns UID/GID queries. Package transactions opt into
virtual root at process startup; normal applications retain the Android UID.
Public calls, hidden libc calls and `syscall()` agree, including after
`clearenv`. This changes the reported guest identity only: Android's kernel
credentials and sandbox are never elevated. Account lookup and package-manager
metadata remain process policy rather than a second identity-query backend.

## Namespace startup

`common/android-namespace.c` models the limited USER/PID/NET startup operations
used by desktop applications. Clone stack switching remains upstream assembly;
ordinary vfork does not pass through a C syscall wrapper. Process state, mapping
metadata and capability drops survive exec. Filesystem views follow fork,
`CLONE_FS` and `unshare(CLONE_FS)` ownership.

These operations do not create kernel namespaces, grant Android privileges or
provide additional confinement. The Android UID and SELinux sandbox remain the
security boundary. Unsupported operations and extra guest seccomp filters fail
explicitly. Inline application syscalls do not acquire modeled libc behavior.

## POSIX shared memory

`shm_open` and named semaphores share `/dev/shm`, backed by the instance's
private temporary directory. Android can prohibit hard links in app data.
Named semaphore creation publishes its initialized file with
`renameat2(RENAME_NOREPLACE)`: publication is atomic, existing names cannot be
overwritten, and every opener maps the same inode. Symlink and copy fallbacks
are not used because they violate semaphore semantics. Unlinking the name
does not invalidate existing mappings.

## Shared memory

`common/sysv-shm.c` is the single System V shared-memory implementation for both
glibc recipes. Android app UIDs cannot use the kernel's SysV SHM syscalls, so
segments use memory-backed file descriptors. Each `shmat` owns an independent
mapping; removal is deferred until the remaining local mappings detach.
Descriptors can be exported to Xwayland over the existing abstract socket
protocol. This is ordinary shared memory, not the GPU buffer transport.

The old imported ashmem backend is not compiled. Public calls and `syscall()`
share one segment table.

This is not a complete implementation of kernel SysV IPC: key lookup and
attachment counts are currently local to the creating process. General
cross-process key discovery and lifetime semantics require further work.

## Semaphores

`common/sysv-semaphore.c` owns the public semaphore APIs and the corresponding
`syscall()` operations. App-private files hold shared state; file locks and
futexes implement atomic operation sets and blocking waits. `SEM_UNDO` state survives
exec and is reclaimed when another participant observes process death.

The implementation has bounded set and undo-table sizes. It is an Android
compatibility backend, not a claim of complete kernel IPC equivalence.

## Descriptor operations

Timer descriptors use the normal glibc wrappers and the kernel timerfd ABI.
There is no replacement backed by eventfd or timer threads.

`common/close_range.c` extends the generic libc fallback used by the Android
recipe with `CLOSE_RANGE_CLOEXEC`. Standard descriptors are treated like any
other requested descriptor. `CLOSE_RANGE_UNSHARE` returns `ENOSYS` without
changing the shared descriptor table; pretending to unshare would affect
other threads.
