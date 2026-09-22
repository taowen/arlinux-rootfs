# Android-compatible glibc

Each version directory pins an upstream glibc archive and its Android patch
recipe. `tools/build/linux-glibc.sh` builds the loader, libc, libm and ldconfig
for the bundle's host package. Distribution applications keep their standard
glibc ABI; they do not link against a separate Arlinux API.

glibc owns loader search paths, thread initialization, filesystem syscall
translation, identity queries, IPC and kernel fallbacks. The preload runtime in
`native/bionicx-runtime` supplies process/environment policy and synthetic
filesystem/account metadata. It is not a second C library.

## Filesystem boundary

`common/android-syscall.c` translates guest filesystem paths immediately before
the AArch64 kernel call. Public functions, hidden libc calls and `syscall()`
therefore use the same translation. The loader keeps its separate early-startup
path handling. Translation uses bounded stack storage and does not allocate,
read environment variables or call the dynamic loader.

`common/install-syscalls.py` installs the boundary in both supported upstream
versions. It validates each source edit and fails if the upstream contract
changes. Cancellation still uses glibc's original cancellation region; only
the pathname arguments are prepared beforehand.

Ordinary directory creation, renaming and temporary-file APIs need no preload
wrappers. Remaining wrappers implement additional guest policy, such as
`/proc/self/exe`, virtual ownership and Android filesystem restrictions. They
reuse libc's private path translator instead of maintaining a second mapping.

The Android host writes `resolv.conf`. The standard libc resolver reads it;
process constructors do not rewrite it and `res_init` is not interposed.

## Identity

The same syscall boundary owns UID/GID queries. Package transactions opt into
virtual root at process startup; normal applications retain the Android UID.
Public calls, hidden libc calls and `syscall()` agree, including after
`clearenv`. This changes the reported guest identity only: Android's kernel
credentials and sandbox are never elevated. Account lookup and package-manager
metadata remain process policy rather than a second identity-query backend.

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

The old imported ashmem backend is not compiled. The preload library does not
define `shmget`, `shmat`, `shmdt` or `shmctl`; its `syscall()` adapter forwards
these calls to libc. There is one segment table rather than competing libc and
preload tables.

This is not a complete implementation of kernel SysV IPC: key lookup and
attachment counts are currently local to the creating process. General
cross-process key discovery and lifetime semantics require further work.

## Semaphores

`common/sysv-semaphore.c` owns the public semaphore APIs and the corresponding
`syscall()` operations. App-private files hold shared state; file locks and
futexes implement atomic operation sets and blocking waits. The preload library
does not export a second semaphore implementation. `SEM_UNDO` state survives
exec and is reclaimed when another participant observes process death.

The implementation has bounded set and undo-table sizes. It is an Android
compatibility backend, not a claim of complete kernel IPC equivalence.

## Descriptor operations

Timer descriptors use the normal glibc wrappers and the kernel timerfd ABI.
There is no preload replacement backed by eventfd or timer threads.

`common/close_range.c` extends the generic libc fallback used by the Android
recipe with `CLOSE_RANGE_CLOEXEC`. Standard descriptors are treated like any
other requested descriptor. `CLOSE_RANGE_UNSHARE` returns `ENOSYS` without
changing the shared descriptor table; pretending to unshare would affect
other threads.
