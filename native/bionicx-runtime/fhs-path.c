#include "runtime-internal.h"

#include <dlfcn.h>
#include <limits.h>
#include <sys/inotify.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/utsname.h>
#include <time.h>

#ifndef STATX_BASIC_STATS
#define STATX_BASIC_STATS 0x000007ffU
#endif

static void fill_statx_from_stat(struct statx *out, const struct stat *st,
                                 unsigned int mask) {
    unsigned int want = mask ? mask : STATX_BASIC_STATS;

    memset(out, 0, sizeof(*out));
    out->stx_mask = want & STATX_BASIC_STATS;
    out->stx_blksize = (uint32_t)st->st_blksize;
    out->stx_nlink = st->st_nlink;
    out->stx_uid = st->st_uid;
    out->stx_gid = st->st_gid;
    out->stx_mode = (uint16_t)st->st_mode;
    out->stx_ino = st->st_ino;
    out->stx_size = (uint64_t)st->st_size;
    out->stx_blocks = (uint64_t)st->st_blocks;
    out->stx_atime.tv_sec = st->st_atim.tv_sec;
    out->stx_atime.tv_nsec = (uint32_t)st->st_atim.tv_nsec;
    out->stx_mtime.tv_sec = st->st_mtim.tv_sec;
    out->stx_mtime.tv_nsec = (uint32_t)st->st_mtim.tv_nsec;
    out->stx_ctime.tv_sec = st->st_ctim.tv_sec;
    out->stx_ctime.tv_nsec = (uint32_t)st->st_ctim.tv_nsec;
    out->stx_btime.tv_sec = 0;
    out->stx_btime.tv_nsec = 0;
    out->stx_dev_major = major(st->st_dev);
    out->stx_dev_minor = minor(st->st_dev);
    out->stx_rdev_major = major(st->st_rdev);
    out->stx_rdev_minor = minor(st->st_rdev);
}

/* Linux exposes the mount ID of an O_PATH descriptor in fdinfo even when
 * Android's seccomp policy blocks statx. Do not substitute st_dev: bind
 * mounts can share a device while having distinct mount IDs. */
static void fill_statx_mount_id(struct statx *out, int dirfd, const char *path,
                              int flags, unsigned int mask) {
    if (!(mask & (0x1000U | 0x4000U))) return; /* MNT_ID / MNT_ID_UNIQUE */
    static int (*host_openat)(int, const char *, int, ...);
    static ssize_t (*host_read)(int, void *, size_t);
    static int (*host_close)(int);
    if (!host_openat) host_openat = dlsym(RTLD_NEXT, "openat");
    if (!host_read) host_read = dlsym(RTLD_NEXT, "read");
    if (!host_close) host_close = dlsym(RTLD_NEXT, "close");
    int owned = 0, fd = dirfd;
    if (path && path[0]) {
        fd = host_openat(dirfd, path, O_PATH | O_CLOEXEC |
                         ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0));
        if (fd < 0) return;
        owned = 1;
    }
    char info[64], data[1024];
    snprintf(info, sizeof(info), "/proc/self/fdinfo/%d", fd);
    int infofd = host_openat(AT_FDCWD, info, O_RDONLY | O_CLOEXEC);
    if (infofd >= 0) {
        ssize_t n = host_read(infofd, data, sizeof(data) - 1);
        if (n > 0) {
            data[n] = 0;
            char *field = strstr(data, "mnt_id:");
            unsigned long long id;
            if (field && sscanf(field, "mnt_id: %llu", &id) == 1) {
                out->stx_mnt_id = id;
                out->stx_mask |= 0x1000U; /* legacy mount ID, not unique */
            }
        }
        host_close(infofd);
    }
    if (owned) host_close(fd);
}

/* Zygote traps SYS_statx (probe → ENOSYS). GIO/nautilus need it; fall back
 * to fstatat and synthesize a basic struct statx. */
BIONICX_INTERNAL int bionicx_statx(int dirfd, const char *path, int flags,
                                   unsigned int mask, void *buf) {
    static int (*real_fstatat)(int, const char *, struct stat *, int);
    struct stat st;
    int atflags = 0;
    struct statx *out = buf;

    if (out == NULL) {
        errno = EFAULT;
        return -1;
    }
    if (real_fstatat == NULL)
        real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    if (real_fstatat == NULL) {
        errno = ENOSYS;
        return -1;
    }
    if ((flags & AT_SYMLINK_NOFOLLOW) != 0)
        atflags |= AT_SYMLINK_NOFOLLOW;
#ifdef AT_EMPTY_PATH
    if ((flags & AT_EMPTY_PATH) != 0)
        atflags |= AT_EMPTY_PATH;
#endif
    if (real_fstatat(dirfd, path != NULL ? path : "", &st, atflags) != 0)
        return -1;
    fill_statx_from_stat(out, &st, mask);
    fill_statx_mount_id(out, dirfd, path, flags, mask);
    return 0;
}

static const char *inotify_sysctl_value(const char *name) {
    if (strcmp(name, "max_user_watches") == 0) return "65536\n";
    if (strcmp(name, "max_user_instances") == 0) return "128\n";
    if (strcmp(name, "max_queued_events") == 0) return "16384\n";
    return NULL;
}

/* Android has no /proc/sys/fs/inotify sysctls. Chromium FilePathWatcher reads
 * max_user_watches and otherwise disables the explorer/git file watcher. */
static const char *redirect_inotify_sysctl(const char *path,
                                           char buffer[PATH_MAX]) {
    if (path == NULL || strncmp(path, "/proc/sys/fs/inotify/", 21) != 0)
        return NULL;
    const char *name = path + 21;
    const char *value = inotify_sysctl_value(name);
    if (value == NULL || strchr(name, '/') != NULL) return NULL;
    const char *tmp = bionicx_captured_tmpdir();
    if (tmp == NULL) tmp = bionicx_getenv("BIONICX_TMPDIR");
    if (tmp == NULL || tmp[0] != '/') return path;
    char directory[PATH_MAX];
    if (snprintf(directory, PATH_MAX, "%s/inotify-sysctl", tmp) >= PATH_MAX ||
            snprintf(buffer, PATH_MAX, "%s/%s", directory, name) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    static int (*real_mkdir)(const char *, mode_t);
    static int (*real_open)(const char *, int, ...);
    static int (*real_stat)(const char *, struct stat *);
    if (real_mkdir == NULL) real_mkdir = dlsym(RTLD_NEXT, "mkdir");
    if (real_open == NULL) real_open = dlsym(RTLD_NEXT, "open");
    if (real_stat == NULL) real_stat = dlsym(RTLD_NEXT, "stat");
    struct stat info;
    if (real_stat(buffer, &info) == 0) return buffer;
    if (real_mkdir(directory, 0700) != 0 && errno != EEXIST) return path;
    int fd = real_open(buffer, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return errno == EEXIST ? buffer : path;
    size_t length = strlen(value);
    ssize_t wrote = write(fd, value, length);
    close(fd);
    if (wrote != (ssize_t)length) return path;
    return buffer;
}

/* VS Code reads /etc/shells then fs.watch()s dirname. Android /bin is
 * /system/bin (mode o+x only), so watch("/bin") returns EACCES. Point
 * login shells at the real rootfs paths instead. */
static const char *redirect_etc_shells(const char *path, char buffer[PATH_MAX]) {
    static const char *const shells[] = {
        "/bin/sh", "/usr/bin/sh", "/bin/bash", "/usr/bin/bash",
        "/bin/rbash", "/usr/bin/rbash", "/usr/bin/dash"
    };
    if (path == NULL || strcmp(path, "/etc/shells") != 0) return NULL;
    const char *root = bionicx_captured_rootfs();
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    const char *tmp = bionicx_captured_tmpdir();
    if (tmp == NULL) tmp = bionicx_getenv("BIONICX_TMPDIR");
    if (root == NULL || root[0] != '/' || tmp == NULL || tmp[0] != '/')
        return NULL;
    if (snprintf(buffer, PATH_MAX, "%s/etc-shells", tmp) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    static int (*real_open)(const char *, int, ...);
    static int (*real_stat)(const char *, struct stat *);
    if (real_open == NULL) real_open = dlsym(RTLD_NEXT, "open");
    if (real_stat == NULL) real_stat = dlsym(RTLD_NEXT, "stat");
    struct stat info;
    if (real_stat(buffer, &info) == 0) return buffer;
    int fd = real_open(buffer, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return errno == EEXIST ? buffer : NULL;
    int ok = 1;
    for (size_t i = 0; i < sizeof(shells) / sizeof(shells[0]); ++i) {
        char line[PATH_MAX];
        int n = snprintf(line, sizeof(line), "%s%s\n", root, shells[i]);
        if (n < 0 || n >= (int)sizeof(line) ||
                write(fd, line, (size_t)n) != n) {
            ok = 0;
            break;
        }
    }
    close(fd);
    return ok ? buffer : NULL;
}

/* Android app UIDs cannot read /proc/stat. VS Code's cpuUsage.sh (and
 * `code --status`) then dies with division by zero. Rewrite a growing idle
 * counter so two samples in one script have a non-zero total delta. */
static const char *redirect_proc_stat(const char *path, char buffer[PATH_MAX]) {
    if (path == NULL || strcmp(path, "/proc/stat") != 0) return NULL;
    const char *tmp = bionicx_captured_tmpdir();
    if (tmp == NULL) tmp = bionicx_getenv("BIONICX_TMPDIR");
    if (tmp == NULL || tmp[0] != '/') return path;
    if (snprintf(buffer, PATH_MAX, "%s/proc-stat", tmp) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    static int (*real_open)(const char *, int, ...);
    if (real_open == NULL) real_open = dlsym(RTLD_NEXT, "open");
    int fd = real_open(buffer, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return path;
    static unsigned long ticks;
    unsigned long idle = 1000000UL + (unsigned long)time(NULL) + ++ticks * 10UL;
    char text[256];
    int length = snprintf(text, sizeof(text),
            "cpu  0 0 0 %lu 0 0 0 0 0 0\n"
            "cpu0 0 0 0 %lu 0 0 0 0 0 0\n"
            "intr 0\nctxt 0\nbtime 0\nprocesses 1\n"
            "procs_running 1\nprocs_blocked 0\nsoftirq 0\n",
            idle, idle);
    ssize_t wrote = length > 0 ? write(fd, text, (size_t)length) : -1;
    close(fd);
    if (wrote != (ssize_t)length) return path;
    return buffer;
}

/* Android SELinux denies app UIDs access to /proc/version.  Some desktop
 * launchers (LibreOffice's oosplash among them) use it only to establish that
 * procfs is available.  Preserve the Linux ABI with data already exposed by
 * uname(2), instead of making each application carry an Android workaround. */
static const char *redirect_proc_version(const char *path,
                                         char buffer[PATH_MAX]) {
    if (path == NULL || strcmp(path, "/proc/version") != 0) return NULL;
    const char *tmp = bionicx_captured_tmpdir();
    if (tmp == NULL) tmp = bionicx_getenv("BIONICX_TMPDIR");
    if (tmp == NULL || tmp[0] != '/') return path;
    if (snprintf(buffer, PATH_MAX, "%s/proc-version", tmp) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    static int (*real_open)(const char *, int, ...);
    if (real_open == NULL) real_open = dlsym(RTLD_NEXT, "open");
    int fd = real_open(buffer, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) return path;
    struct utsname info;
    char text[512];
    int length;
    if (uname(&info) == 0)
        length = snprintf(text, sizeof(text),
                "Linux version %s (arlinux@android) %s\n",
                info.release, info.version);
    else
        length = snprintf(text, sizeof(text),
                "Linux version 0.0.0-arlinux (arlinux@android)\n");
    ssize_t wrote = length > 0 && length < (int)sizeof(text)
            ? write(fd, text, (size_t)length) : -1;
    close(fd);
    if (wrote != (ssize_t)length) return path;
    return buffer;
}

/* Android has no /dev/shm. Feishu libmonitor uses Boost.Interprocess there
 * (sticky-bit check + shm files). glibc shm_open also targets /dev/shm via
 * an internal open that bypasses LD_PRELOAD. */
static int is_dev_shm_dir(const char *path) {
    return path != NULL && (strcmp(path, "/dev/shm") == 0 ||
            strcmp(path, "/dev/shm/") == 0);
}

static void apply_dev_shm_mode(const char *path, mode_t *mode) {
    if (!is_dev_shm_dir(path) || mode == NULL) return;
    *mode |= S_ISVTX | 0777;
}

static int is_proc_self_exe(const char *path) {
    char *end = NULL;
    long pid;

    if (path == NULL)
        return 0;
    if (strcmp(path, "/proc/self/exe") == 0)
        return 1;
    if (strcmp(path, "/proc/thread-self/exe") == 0)
        return 1;
    if (strncmp(path, "/proc/", 6) != 0)
        return 0;
    if (path[6] < '1' || path[6] > '9')
        return 0;
    pid = strtol(path + 6, &end, 10);
    return end != path + 6 && strcmp(end, "/exe") == 0 && (pid == (long)getpid() || pid == (long)bionicx_host_pid());
}

int bionicx_path_is_overlay_loader(const char *path) {
    const char *slash;
    if (path == NULL || path[0] == '\0')
        return 0;
    slash = strrchr(path, '/');
    return slash != NULL && strcmp(slash + 1, "ld-linux-aarch64.so.1") == 0;
}

/* Explicit-loader: kernel /proc/self/exe is overlay ld.so. When EXECFN is
 * set, always report/open the guest ELF (one policy for open + readlink). */
static ssize_t readlink_self_exe(char *value, size_t size) {
    static ssize_t (*real_readlink)(const char *, char *, size_t);
    char real[PATH_MAX];
    const char *exe = bionicx_guest_execfn();
    ssize_t n;
    size_t length;
    size_t copy;

    if (exe != NULL) {
        length = strlen(exe);
        copy = length < size ? length : size;
        memcpy(value, exe, copy);
        return (ssize_t)copy;
    }
    if (real_readlink == NULL)
        real_readlink = dlsym(RTLD_NEXT, "readlink");
    if (real_readlink == NULL)
        return -1;
    n = real_readlink("/proc/self/exe", real, sizeof(real) - 1);
    if (n < 0)
        return n;
    copy = (size_t)n < size ? (size_t)n : size;
    memcpy(value, real, copy);
    return (ssize_t)copy;
}

static const char *open_self_exe_path(void) {
    return bionicx_guest_execfn();
}

/* Redirect Linux FHS paths into app-private Android directories. */
const char *bionicx_redirect_path(const char *path, char buffer[PATH_MAX]) {
    if (path == NULL) return NULL;
    const char *ns = bionicx_ns_path(path, buffer);
    if (ns != path) return ns;
    const char *inotify = redirect_inotify_sysctl(path, buffer);
    if (inotify != NULL) return inotify;
    const char *proc_stat = redirect_proc_stat(path, buffer);
    if (proc_stat != NULL) return proc_stat;
    const char *proc_version = redirect_proc_version(path, buffer);
    if (proc_version != NULL) return proc_version;
    const char *shells = redirect_etc_shells(path, buffer);
    if (shells != NULL) return shells;
    static const char *(*root_path)(const char *, char *);
    if (!root_path) root_path = dlsym(RTLD_NEXT, "__arlinux_root_path");
    if (!root_path) { errno = ENOSYS; return NULL; }
    return root_path(path, buffer);
}

/* Kernel follows absolute symlinks without FHS rewrite, so a rootfs link
 * such as share/registry/main.xcd → /etc/libreoffice/registry/main.xcd
 * opens Android /etc and ENOENT. LibreOffice configmgr then throws
 * uno::RuntimeException. Walk absolute FHS targets ourselves. */
static const char *bionicx_resolve_fhs_symlink(const char *actual,
                                               char out[PATH_MAX])
{
    static ssize_t (*real_readlink)(const char *, char *, size_t);
    static int (*real_lstat)(const char *, struct stat *);
    char current[PATH_MAX];
    int hop;

    if (actual == NULL || actual[0] != '/')
        return actual;
    if (real_readlink == NULL)
        real_readlink = dlsym(RTLD_NEXT, "readlink");
    if (real_lstat == NULL)
        real_lstat = dlsym(RTLD_NEXT, "lstat");
    if (real_readlink == NULL || real_lstat == NULL)
        return actual;
    if (snprintf(current, sizeof(current), "%s", actual) >=
            (int)sizeof(current))
        return actual;
    for (hop = 0; hop < 8; ++hop) {
        struct stat info;
        char target[PATH_MAX];
        char rewritten[PATH_MAX];
        const char *next_path;
        ssize_t n;

        if (real_lstat(current, &info) != 0 || !S_ISLNK(info.st_mode))
            break;
        n = real_readlink(current, target, sizeof(target) - 1);
        if (n <= 0)
            break;
        target[n] = '\0';
        if (target[0] != '/')
            break;
        next_path = bionicx_redirect_path(target, rewritten);
        if (next_path == NULL || next_path == target)
            break;
        if (snprintf(current, sizeof(current), "%s", next_path) >=
                (int)sizeof(current))
            return actual;
    }
    if (snprintf(out, PATH_MAX, "%s", current) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return out;
}

/* Android SELinux does not allow an app process to open another process'
 * /proc/<pid>/root magic link, even when both processes share the same app
 * UID.  Desktop portals use that link to resolve paths supplied by their
 * clients.  Every process visible inside one arlinux instance shares the
 * same guest rootfs, so following the magic link is equivalent to opening
 * $BIONICX_ROOTFS.  Keep readlink/lstat untouched: they must still expose a
 * procfs symlink whose visible target is "/". */
static const char *redirect_proc_process_root(const char *path,
                                              char buffer[PATH_MAX])
{
    const char *part;
    const char *suffix;
    const char *root;

    if (path == NULL || strncmp(path, "/proc/", 6) != 0)
        return NULL;
    part = path + 6;
    if (strncmp(part, "self/root", 9) == 0)
        suffix = part + 9;
    else if (strncmp(part, "thread-self/root", 16) == 0)
        suffix = part + 16;
    else {
        if (*part < '1' || *part > '9')
            return NULL;
        while (*part >= '0' && *part <= '9')
            ++part;
        if (strncmp(part, "/root", 5) != 0)
            return NULL;
        suffix = part + 5;
    }
    if (*suffix != '\0' && *suffix != '/')
        return NULL;
    root = bionicx_captured_rootfs();
    if (root == NULL)
        root = bionicx_getenv("BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/')
        return path;
    if (snprintf(buffer, PATH_MAX, "%s%s", root, suffix) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    return buffer;
}

static const char *bionicx_redirect_open_path(const char *path,
                                              char out[PATH_MAX],
                                              int follow)
{
    char redirected[PATH_MAX];
    const char *proc_root;

    if (follow) {
        proc_root = redirect_proc_process_root(path, out);
        if (proc_root != NULL)
            return proc_root;
    }
    const char *actual = bionicx_redirect_path(path, redirected);

    if (actual == NULL)
        return NULL;
    if (!follow) {
        if (actual == out)
            return actual;
        if (snprintf(out, PATH_MAX, "%s", actual) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        return out;
    }
    return bionicx_resolve_fhs_symlink(actual, out);
}

static const char *redirect_symlink_target(const char *target,
                                           char buffer[PATH_MAX]) {
    const char *rewrite = bionicx_getenv("BIONICX_REWRITE_ABSOLUTE_SYMLINKS");
    if (rewrite == NULL || strcmp(rewrite, "1") != 0 || target == NULL ||
            target[0] != '/')
        return target;
    return bionicx_redirect_path(target, buffer);
}

int symlink(const char *target, const char *link_path) {
    static int (*next)(const char *, const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "symlink");
    char target_buffer[PATH_MAX], link_buffer[PATH_MAX];
    const char *actual_target = redirect_symlink_target(target, target_buffer);
    const char *actual_link = bionicx_redirect_path(link_path, link_buffer);
    if (actual_target == NULL || actual_link == NULL) return -1;
    return next(actual_target, actual_link);
}

int symlinkat(const char *target, int directory, const char *link_path) {
    static int (*next)(const char *, int, const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "symlinkat");
    char target_buffer[PATH_MAX], link_buffer[PATH_MAX];
    const char *actual_target = redirect_symlink_target(target, target_buffer);
    const char *actual_link = bionicx_redirect_path(link_path, link_buffer);
    if (actual_target == NULL || actual_link == NULL) return -1;
    return next(actual_target, directory, actual_link);
}

mode_t bionicx_optional_mode(int flags, va_list arguments) {
    return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)
            ? (mode_t)va_arg(arguments, int) : 0;
}

int inotify_add_watch(int fd, const char *path, uint32_t mask) {
    static int (*next)(int, const char *, uint32_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "inotify_add_watch");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    return next(fd, actual, mask);
}

/* Guest namespace and path translation are still transitional interposition.
 * Kernel compatibility belongs to libc, including its syscall() entry point. */
static long libc_syscall6(long number, long a1, long a2, long a3, long a4,
                         long a5, long a6) {
    static long (*next)(long, ...);
    if (next == NULL) next = dlsym(RTLD_NEXT, "syscall");
    if (next == NULL) { errno = ENOSYS; return -1; }
    return next(number, a1, a2, a3, a4, a5, a6);
}

long syscall(long number, ...) {
    va_list arguments;
    va_start(arguments, number);
    long a1 = va_arg(arguments, long);
    long a2 = va_arg(arguments, long);
    long a3 = va_arg(arguments, long);
    long a4 = va_arg(arguments, long);
    long a5 = va_arg(arguments, long);
    long a6 = va_arg(arguments, long);
    va_end(arguments);
    long ns_result;
    if (bionicx_ns_syscall(number, a1, a2, a3, a4, a5, a6, &ns_result))
        return ns_result;
    if (number == SYS_openat) {
        int fd = bionicx_ns_open((int)a1, (const char *)a2, (int)a3);
        if (fd != -2) return fd;
    }
    long *path_slot = NULL;
    char path_buffer[PATH_MAX];
#ifdef SYS_openat
    if (number == SYS_openat) path_slot = &a2;
#endif
#ifdef SYS_openat2
    if (number == SYS_openat2) path_slot = &a2;
#endif
#ifdef SYS_statx
    if (number == SYS_statx) path_slot = &a2;
#endif
#ifdef SYS_inotify_add_watch
    if (number == SYS_inotify_add_watch) path_slot = &a2;
#endif
#ifdef SYS_newfstatat
    if (number == SYS_newfstatat) path_slot = &a2;
#endif
#ifdef SYS_faccessat
    if (number == SYS_faccessat) path_slot = &a2;
#endif
#ifdef SYS_faccessat2
    if (number == SYS_faccessat2) path_slot = &a2;
#endif
#ifdef SYS_readlinkat
    if (number == SYS_readlinkat) path_slot = &a2;
#endif
#ifdef SYS_unlink
    if (number == SYS_unlink) path_slot = &a1;
#endif
#ifdef SYS_unlinkat
    if (number == SYS_unlinkat) path_slot = &a2;
#endif
#ifdef SYS_mkdir
    if (number == SYS_mkdir) path_slot = &a1;
#endif
#ifdef SYS_mkdirat
    if (number == SYS_mkdirat) path_slot = &a2;
#endif
#ifdef SYS_chdir
    if (number == SYS_chdir) path_slot = &a1;
#endif
#ifdef SYS_utimensat
    if (number == SYS_utimensat) path_slot = &a2;
#endif
#ifdef SYS_utime
    if (number == SYS_utime) path_slot = &a1;
#endif
#ifdef SYS_utimes
    if (number == SYS_utimes) path_slot = &a1;
#endif
#ifdef SYS_setxattr
    if ((number == SYS_setxattr
#ifdef SYS_lsetxattr
            || number == SYS_lsetxattr
#endif
#ifdef SYS_fsetxattr
            || number == SYS_fsetxattr
#endif
            ) && bionicx_is_file_capability_xattr((const char *)a2)) {
        long cap_result = libc_syscall6(number, a1, a2, a3, a4, a5, a6);
        if (cap_result == -1) {
            int err = errno;
            if (err == EPERM || err == EACCES || err == ENOSYS || err == EOPNOTSUPP)
                return 0;
            errno = err;
            return -1;
        }
        return cap_result;
    }
#endif
#ifdef SYS_statx
    if (number == SYS_statx) {
        const char *path = (const char *)a2;
        char buffer[PATH_MAX];
        const char *actual = path;
        int flags = (int)a3;

        if (path != NULL && path[0] != '\0'
#ifdef AT_EMPTY_PATH
            && (flags & AT_EMPTY_PATH) == 0
#endif
            ) {
            actual = bionicx_redirect_open_path(path, buffer,
                    (flags & AT_SYMLINK_NOFOLLOW) == 0);
            if (actual == NULL)
                return -1;
        }
        return bionicx_statx((int)a1, actual, flags, (unsigned int)a4,
                             (void *)a5);
    }
#endif
#ifdef SYS_readlinkat
    if (number == SYS_readlinkat && is_proc_self_exe((const char *)a2)) {
        if (a3 == 0) {
            errno = EFAULT;
            return -1;
        }
        return readlink_self_exe((char *)a3, (size_t)a4);
    }
#endif
    if (path_slot != NULL) {
        const char *path = (const char *)*path_slot;
        if (path != NULL && path[0] != '\0') {
            if ((number == SYS_openat
#ifdef SYS_openat2
                        || number == SYS_openat2
#endif
                        ) && is_proc_self_exe(path)) {
                const char *exe = open_self_exe_path();
                if (exe != NULL)
                    path = exe;
            }
            int follow = 1;
#ifdef SYS_openat
            if (number == SYS_openat && (a3 & O_NOFOLLOW) != 0)
                follow = 0;
#endif
#ifdef SYS_newfstatat
            if (number == SYS_newfstatat && (a4 & AT_SYMLINK_NOFOLLOW) != 0)
                follow = 0;
#endif
#ifdef SYS_faccessat
            if (number == SYS_faccessat && (a4 & AT_SYMLINK_NOFOLLOW) != 0)
                follow = 0;
#endif
#ifdef SYS_faccessat2
            if (number == SYS_faccessat2 && (a4 & AT_SYMLINK_NOFOLLOW) != 0)
                follow = 0;
#endif
#ifdef SYS_readlinkat
            if (number == SYS_readlinkat)
                follow = 0;
#endif
#ifdef SYS_unlink
            if (number == SYS_unlink)
                follow = 0;
#endif
#ifdef SYS_unlinkat
            if (number == SYS_unlinkat)
                follow = 0;
#endif
#ifdef SYS_utimensat
            if (number == SYS_utimensat && (a4 & AT_SYMLINK_NOFOLLOW) != 0)
                follow = 0;
#endif
#ifdef SYS_utime
            if (number == SYS_utime)
                follow = 0;
#endif
#ifdef SYS_utimes
            if (number == SYS_utimes)
                follow = 0;
#endif
            const char *actual = bionicx_redirect_open_path(path, path_buffer,
                                                            follow);
            if (actual == NULL) return -1;
            if (actual != path) *path_slot = (long)actual;
            else if (path != (const char *)*path_slot)
                *path_slot = (long)path;
        }
    }
#if defined(SYS_rename) || defined(SYS_renameat) || defined(SYS_renameat2)
    if (0
#ifdef SYS_rename
            || number == SYS_rename
#endif
#ifdef SYS_renameat
            || number == SYS_renameat
#endif
#ifdef SYS_renameat2
            || number == SYS_renameat2
#endif
            ) {
        char old_buffer[PATH_MAX], new_buffer[PATH_MAX];
        long *old_slot = &a1;
        long *new_slot = &a2;
#ifdef SYS_renameat
        if (number == SYS_renameat)
            old_slot = &a2, new_slot = &a4;
#endif
#ifdef SYS_renameat2
        if (number == SYS_renameat2)
            old_slot = &a2, new_slot = &a4;
#endif
        const char *old_path = (const char *)*old_slot;
        const char *new_path = (const char *)*new_slot;
        if (old_path != NULL && old_path[0] != '\0') {
            const char *actual = bionicx_redirect_path(old_path, old_buffer);
            if (actual == NULL) return -1;
            if (actual != old_path) *old_slot = (long)actual;
        }
        if (new_path != NULL && new_path[0] != '\0') {
            const char *actual = bionicx_redirect_path(new_path, new_buffer);
            if (actual == NULL) return -1;
            if (actual != new_path) *new_slot = (long)actual;
        }
        return libc_syscall6(number, a1, a2, a3, a4, a5, a6);
    }
#endif
    return libc_syscall6(number, a1, a2, a3, a4, a5, a6);
}

static int guest_virtual_root_active(void) {
    const char *value = bionicx_captured_value("BIONICX_VIRTUAL_ROOT");
    if (value == NULL) value = bionicx_getenv("BIONICX_VIRTUAL_ROOT");
    return value != NULL && value[0] != '\0' && strcmp(value, "0") != 0;
}

static int guest_system_path(const char *actual) {
    const char *root = bionicx_captured_value("BIONICX_ROOTFS");
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    if (actual == NULL || root == NULL || root[0] != '/') return 0;
    size_t length = strlen(root);
    return strncmp(actual, root, length) == 0 &&
            (actual[length] == '/' || actual[length] == '\0');
}

/* Android grants the app UID write access to every extracted rootfs inode.
 * Ordinary guest programs must instead obey the root-owned permissions that
 * stat() exposes.  Package transactions opt into BIONICX_VIRTUAL_ROOT and are
 * allowed to mutate the system tree. */
static int check_guest_open_write(int directory, const char *actual,
                                  int flags) {
    int access_mode = flags & O_ACCMODE;
    int writes_existing = access_mode == O_WRONLY || access_mode == O_RDWR ||
            (flags & O_TRUNC) != 0;
    int may_create = (flags & O_CREAT) != 0 ||
            (flags & O_TMPFILE) == O_TMPFILE;
    if ((!writes_existing && !may_create) || !guest_system_path(actual) ||
            guest_virtual_root_active())
        return 0;

    static int (*real_fstatat)(int, const char *, struct stat *, int);
    if (real_fstatat == NULL) real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    struct stat info;
    if ((flags & O_TMPFILE) != O_TMPFILE &&
            real_fstatat(directory, actual, &info, 0) == 0) {
        if (!writes_existing || (info.st_mode & S_IWOTH) != 0) return 0;
        errno = EACCES;
        return -1;
    }
    if (!may_create || (errno != ENOENT && errno != ENOTDIR)) return 0;

    char parent[PATH_MAX];
    if (snprintf(parent, sizeof(parent), "%s", actual) >= (int)sizeof(parent)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if ((flags & O_TMPFILE) != O_TMPFILE) {
        char *slash = strrchr(parent, '/');
        if (slash == NULL) return 0;
        if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
    }
    if (real_fstatat(directory, parent, &info, 0) != 0) return 0;
    if ((info.st_mode & (S_IWOTH | S_IXOTH)) == (S_IWOTH | S_IXOTH)) return 0;
    errno = EACCES;
    return -1;
}

int open(const char *path, int flags, ...) {
    int ns_fd = bionicx_ns_open(AT_FDCWD, path, flags);
    if (ns_fd != -2) return ns_fd;
    static int (*next)(const char *, int, ...);
    if (next == NULL) next = dlsym(RTLD_NEXT, "open");
    va_list arguments;
    va_start(arguments, flags);
    mode_t mode = bionicx_optional_mode(flags, arguments);
    va_end(arguments);
    if (is_proc_self_exe(path)) {
        const char *exe = open_self_exe_path();
        if (exe != NULL)
            path = exe;
    }
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & O_NOFOLLOW) == 0);
    if (actual == NULL) return -1;
    if (check_guest_open_write(AT_FDCWD, actual, flags) != 0) return -1;
    return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)
            ? next(actual, flags, mode) : next(actual, flags);
}

/* creat() calls libc's internal open, bypassing the interposed open(). */
int creat(const char *path, mode_t mode) {
    return open(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int creat64(const char *path, mode_t mode) {
    return open64(path, O_WRONLY | O_CREAT | O_TRUNC, mode);
}

int open64(const char *path, int flags, ...) {
    int ns_fd = bionicx_ns_open(AT_FDCWD, path, flags);
    if (ns_fd != -2) return ns_fd;
    static int (*next)(const char *, int, ...);
    if (next == NULL) next = dlsym(RTLD_NEXT, "open64");
    va_list arguments;
    va_start(arguments, flags);
    mode_t mode = bionicx_optional_mode(flags, arguments);
    va_end(arguments);
    if (is_proc_self_exe(path)) {
        const char *exe = open_self_exe_path();
        if (exe != NULL)
            path = exe;
    }
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & O_NOFOLLOW) == 0);
    if (actual == NULL) return -1;
    if (check_guest_open_write(AT_FDCWD, actual, flags) != 0) return -1;
    return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)
            ? next(actual, flags, mode) : next(actual, flags);
}

int openat(int directory, const char *path, int flags, ...) {
    int ns_fd = bionicx_ns_open(directory, path, flags);
    if (ns_fd != -2) return ns_fd;
    static int (*next)(int, const char *, int, ...);
    if (next == NULL) next = dlsym(RTLD_NEXT, "openat");
    va_list arguments;
    va_start(arguments, flags);
    mode_t mode = bionicx_optional_mode(flags, arguments);
    va_end(arguments);
    if (is_proc_self_exe(path)) {
        const char *exe = open_self_exe_path();
        if (exe != NULL)
            path = exe;
    }
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & O_NOFOLLOW) == 0);
    if (actual == NULL) return -1;
    if (check_guest_open_write(directory, actual, flags) != 0) return -1;
    return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)
            ? next(directory, actual, flags, mode)
            : next(directory, actual, flags);
}

/* Fortified glibc open() compiles to __open_2 / __open64_2 and never
 * reaches the interposed open() symbol. shadow-utils groupadd uses this. */
int __open_2(const char *path, int flags) {
    return open(path, flags);
}

int __open64_2(const char *path, int flags) {
    return open64(path, flags);
}

int __openat_2(int directory, const char *path, int flags) {
    return openat(directory, path, flags);
}

int __openat64_2(int directory, const char *path, int flags) {
    return openat64(directory, path, flags);
}

static int password_lock_fd = -1;

int lckpwdf(void) {
    struct flock lock;
    if (password_lock_fd >= 0) return 0;
    password_lock_fd = open("/etc/.pwd.lock", O_WRONLY | O_CREAT, 0600);
    if (password_lock_fd < 0) return -1;
    memset(&lock, 0, sizeof(lock));
    lock.l_type = F_WRLCK;
    lock.l_whence = SEEK_SET;
    if (fcntl(password_lock_fd, F_SETLKW, &lock) < 0) {
        close(password_lock_fd);
        password_lock_fd = -1;
        return -1;
    }
    return 0;
}

int ulckpwdf(void) {
    if (password_lock_fd < 0) {
        errno = EINVAL;
        return -1;
    }
    int result = close(password_lock_fd);
    password_lock_fd = -1;
    return result;
}

int openat64(int directory, const char *path, int flags, ...) {
    int ns_fd = bionicx_ns_open(directory, path, flags);
    if (ns_fd != -2) return ns_fd;
    static int (*next)(int, const char *, int, ...);
    if (next == NULL) next = dlsym(RTLD_NEXT, "openat64");
    va_list arguments;
    va_start(arguments, flags);
    mode_t mode = bionicx_optional_mode(flags, arguments);
    va_end(arguments);
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & O_NOFOLLOW) == 0);
    if (actual == NULL) return -1;
    if (check_guest_open_write(directory, actual, flags) != 0) return -1;
    return (flags & O_CREAT) || ((flags & O_TMPFILE) == O_TMPFILE)
            ? next(directory, actual, flags, mode)
            : next(directory, actual, flags);
}

static void log_xkb_path(const char *fn, const char *path, const char *actual) {
    if (path == NULL || strstr(path, "xkb") == NULL) return;
    if (bionicx_getenv("BIONICX_LOG_EXEC") == NULL) return;
    fprintf(stderr, "bionicx-%s: %s -> %s\n", fn, path,
            actual != NULL ? actual : "(null)");
    fflush(stderr);
}

FILE *fopen(const char *path, const char *mode) {
    static FILE *(*next)(const char *, const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fopen");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    log_xkb_path("fopen", path, actual);
    return actual != NULL ? next(actual, mode) : NULL;
}

FILE *fopen64(const char *path, const char *mode) {
    static FILE *(*next)(const char *, const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fopen64");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    log_xkb_path("fopen64", path, actual);
    return actual != NULL ? next(actual, mode) : NULL;
}

DIR *opendir(const char *path) {
    static DIR *(*next)(const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "opendir");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    return actual != NULL ? next(actual) : NULL;
}

/* libc's scandir uses its internal opendir, bypassing the public interposer.
 * ALSA and PulseAudio use it to discover system configuration fragments. */
int scandir(const char *path, struct dirent ***entries,
            int (*filter)(const struct dirent *),
            int (*compare)(const struct dirent **, const struct dirent **)) {
    static int (*next)(const char *, struct dirent ***,
                       int (*)(const struct dirent *),
                       int (*)(const struct dirent **, const struct dirent **));
    if (next == NULL) next = dlsym(RTLD_NEXT, "scandir");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    return actual != NULL ? next(actual, entries, filter, compare) : -1;
}

int scandir64(const char *path, struct dirent64 ***entries,
              int (*filter)(const struct dirent64 *),
              int (*compare)(const struct dirent64 **, const struct dirent64 **)) {
    static int (*next)(const char *, struct dirent64 ***,
                       int (*)(const struct dirent64 *),
                       int (*)(const struct dirent64 **, const struct dirent64 **));
    if (next == NULL) next = dlsym(RTLD_NEXT, "scandir64");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    return actual != NULL ? next(actual, entries, filter, compare) : -1;
}

char *realpath(const char *path, char *resolved_path) {
    static char *(*next)(const char *, char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "realpath");
    /* libc resolves symlinks internally, without calling our readlink.
     * Keep canonical executable discovery consistent with readlink/open
     * when the kernel executable is the explicit ELF loader. */
    if (is_proc_self_exe(path)) {
        const char *exe = open_self_exe_path();
        if (exe != NULL) path = exe;
    }
    char redirected[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, redirected, 1);
    if (actual == NULL) return NULL;

    char physical[PATH_MAX];
    char *result = next(actual, resolved_path == NULL ? NULL : physical);
    if (result == NULL) return NULL;

    const char *root = bionicx_captured_rootfs();
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    size_t root_length = root != NULL ? strlen(root) : 0;
    char canonical_root[PATH_MAX];
    const char *root_prefix = root;
    if (root != NULL && next(root, canonical_root) != NULL) {
        root_prefix = canonical_root;
        root_length = strlen(canonical_root);
    }
    const char *visible = result;
    if (strcmp(actual, path) != 0 && root_prefix != NULL && root_length > 0 &&
            strncmp(result, root_prefix, root_length) == 0 &&
            (result[root_length] == '/' || result[root_length] == '\0'))
        visible = result + root_length;

    if (visible == result && strcmp(actual, path) != 0) {
        const char *tmpdir = bionicx_captured_tmpdir();
        char canonical_tmp[PATH_MAX];
        if (tmpdir == NULL) tmpdir = bionicx_getenv("BIONICX_TMPDIR");
        if (tmpdir != NULL && next(tmpdir, canonical_tmp) != NULL) {
            size_t tmp_length = strlen(canonical_tmp);
            const char *suffix = NULL;
            const char *guest_prefix = "/tmp";
            size_t guest_length = 4;
            if (strncmp(result, canonical_tmp, tmp_length) == 0 &&
                    (result[tmp_length] == '/' || result[tmp_length] == '\0')) {
                suffix = result + tmp_length;
                if (strncmp(suffix, "/run", 4) == 0 &&
                        (suffix[4] == '/' || suffix[4] == '\0')) {
                    guest_prefix = "/run";
                    suffix += 4;
                }
                memmove(result + guest_length, suffix, strlen(suffix) + 1);
                memcpy(result, guest_prefix, guest_length);
                visible = result;
            }
        }
    }

    if (resolved_path == NULL) {
        if (visible != result) memmove(result, visible, strlen(visible) + 1);
        return result;
    }
    strcpy(resolved_path, visible);
    return resolved_path;
}

char *canonicalize_file_name(const char *path) {
    return realpath(path, NULL);
}

ssize_t readlink(const char *path, char *value, size_t size) {
    static ssize_t (*next)(const char *, char *, size_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "readlink");
    if (is_proc_self_exe(path))
        return readlink_self_exe(value, size);
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    return next(actual, value, size);
}

ssize_t readlinkat(int directory, const char *path, char *value, size_t size) {
    static ssize_t (*next)(int, const char *, char *, size_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "readlinkat");
    if (is_proc_self_exe(path))
        return readlink_self_exe(value, size);
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    return next(directory, actual, value, size);
}

/* Fortified callers must see the same guest executable and namespace as
 * readlink, while retaining glibc's fail-fast buffer-size check. */
extern void __chk_fail(void) __attribute__((noreturn));

char *__realpath_chk(const char *path, char *resolved_path, size_t size) {
    char *canonical = realpath(path, NULL);
    if (canonical == NULL) return NULL;
    size_t length = strlen(canonical) + 1;
    if (length > size) {
        free(canonical);
        __chk_fail();
    }
    memcpy(resolved_path, canonical, length);
    free(canonical);
    return resolved_path;
}

ssize_t __readlink_chk(const char *path, char *value, size_t size,
                       size_t value_size) {
    if (size > value_size) __chk_fail();
    return readlink(path, value, size);
}

ssize_t __readlinkat_chk(int directory, const char *path, char *value,
                         size_t size, size_t value_size) {
    if (size > value_size) __chk_fail();
    return readlinkat(directory, path, value, size);
}

int access(const char *path, int mode) {
    if (bionicx_ns_access(path, mode)) return 0;
    static int (*next)(const char *, int);
    static int (*real_stat)(const char *, struct stat *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "access");
    if (real_stat == NULL) real_stat = dlsym(RTLD_NEXT, "stat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    if (actual == NULL || next(actual, mode) != 0) return -1;
    const char *root = bionicx_captured_value("BIONICX_ROOTFS");
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    const char *virtual_root = bionicx_captured_value("BIONICX_VIRTUAL_ROOT");
    if (virtual_root == NULL) virtual_root = bionicx_getenv("BIONICX_VIRTUAL_ROOT");
    size_t root_length = root != NULL ? strlen(root) : 0;
    if (mode != F_OK && root_length != 0 &&
            strncmp(actual, root, root_length) == 0 &&
            (actual[root_length] == '/' || actual[root_length] == '\0') &&
            (virtual_root == NULL || virtual_root[0] == '\0' ||
             strcmp(virtual_root, "0") == 0)) {
        struct stat info;
        if (real_stat(actual, &info) != 0) return -1;
        mode_t allowed = info.st_mode & 07;
        if (((mode & R_OK) && !(allowed & 04)) ||
                ((mode & W_OK) && !(allowed & 02)) ||
                ((mode & X_OK) && !(allowed & 01))) {
            errno = EACCES;
            return -1;
        }
    }
    return 0;
}

/* Bash uses euidaccess(3), exposed by glibc under both names, for `test -r`
 * and `test -x`.  It must observe the same virtual-root permissions and path
 * translation as access(2); otherwise standard /etc/profile skips every
 * readable file under /etc/profile.d.  Android apps do not change their real
 * and effective UID, so the access/euidaccess distinction is immaterial here. */
int euidaccess(const char *path, int mode) {
    return access(path, mode);
}

int eaccess(const char *path, int mode) {
    return access(path, mode);
}

int faccessat(int directory, const char *path, int mode, int flags) {
    static int (*next)(int, const char *, int, int);
    static int (*real_fstatat)(int, const char *, struct stat *, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "faccessat");
    if (real_fstatat == NULL) real_fstatat = dlsym(RTLD_NEXT, "fstatat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & AT_SYMLINK_NOFOLLOW) == 0);
    if (actual == NULL || next(directory, actual, mode, flags) != 0) return -1;
    const char *root = bionicx_captured_value("BIONICX_ROOTFS");
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    const char *virtual_root = bionicx_captured_value("BIONICX_VIRTUAL_ROOT");
    if (virtual_root == NULL) virtual_root = bionicx_getenv("BIONICX_VIRTUAL_ROOT");
    size_t root_length = root != NULL ? strlen(root) : 0;
    if (mode != F_OK && root_length != 0 && actual[0] == '/' &&
            strncmp(actual, root, root_length) == 0 &&
            (actual[root_length] == '/' || actual[root_length] == '\0') &&
            (virtual_root == NULL || virtual_root[0] == '\0' ||
             strcmp(virtual_root, "0") == 0)) {
        struct stat info;
        /* faccessat accepts AT_EACCESS, but fstatat does not.  Passing the
         * access-only flag through made Bash's `test -r` fail with EINVAL
         * after the real permission check had already succeeded. */
        int stat_flags = flags & AT_SYMLINK_NOFOLLOW;
#ifdef AT_EMPTY_PATH
        stat_flags |= flags & AT_EMPTY_PATH;
#endif
#ifdef AT_NO_AUTOMOUNT
        stat_flags |= flags & AT_NO_AUTOMOUNT;
#endif
        if (real_fstatat(directory, actual, &info, stat_flags) != 0) return -1;
        mode_t allowed = info.st_mode & 07;
        if (((mode & R_OK) && !(allowed & 04)) ||
                ((mode & W_OK) && !(allowed & 02)) ||
                ((mode & X_OK) && !(allowed & 01))) {
            errno = EACCES;
            return -1;
        }
    }
    return 0;
}

/* Android stores an extracted rootfs under the application's one real UID.
 * Present system paths as root-owned to ordinary guest processes; package
 * transactions opt into BIONICX_VIRTUAL_ROOT and therefore still see their
 * effective uid and can update the tree.  Home, /tmp and /run live outside
 * BIONICX_ROOTFS and retain the Android app UID. */
static void apply_guest_ownership(const char *actual, uid_t *uid, gid_t *gid) {
    if (!guest_system_path(actual)) return;
    *uid = 0;
    *gid = 0;
}

/* f2fs cannot hard-link. groupadd locks /etc/group by linking
 * group.PID -> group.lock and requiring st_nlink == 2. */
#define BIONICX_FAKE_LINKS 32

static struct {
    char first[PATH_MAX];
    char second[PATH_MAX];
    ino_t inode;
    int used;
} fake_links[BIONICX_FAKE_LINKS];

static void remember_fake_link(const char *source, const char *destination) {
    static int (*real_stat)(const char *, struct stat *);
    struct stat info;
    int index;
    if (source == NULL || destination == NULL) return;
    if (real_stat == NULL) real_stat = dlsym(RTLD_NEXT, "stat");
    if (real_stat(source, &info) != 0) return;
    for (index = 0; index < BIONICX_FAKE_LINKS; ++index) {
        if (!fake_links[index].used) break;
    }
    if (index == BIONICX_FAKE_LINKS) return;
    snprintf(fake_links[index].first, PATH_MAX, "%s", source);
    snprintf(fake_links[index].second, PATH_MAX, "%s", destination);
    fake_links[index].inode = info.st_ino;
    fake_links[index].used = 1;
}

static void apply_fake_nlink(const char *path, nlink_t *nlink, ino_t *inode) {
    int index;
    if (path == NULL || path[0] == '\0') return;
    for (index = 0; index < BIONICX_FAKE_LINKS; ++index) {
        if (!fake_links[index].used) continue;
        if (strcmp(path, fake_links[index].first) != 0 &&
                strcmp(path, fake_links[index].second) != 0)
            continue;
        if (nlink != NULL) *nlink = 2;
        if (inode != NULL) *inode = fake_links[index].inode;
        return;
    }
}

static void forget_fake_link(const char *path) {
    int index;
    if (path == NULL) return;
    for (index = 0; index < BIONICX_FAKE_LINKS; ++index) {
        if (!fake_links[index].used) continue;
        if (strcmp(path, fake_links[index].first) != 0 &&
                strcmp(path, fake_links[index].second) != 0)
            continue;
        fake_links[index].used = 0;
        fake_links[index].first[0] = '\0';
        fake_links[index].second[0] = '\0';
    }
}

int stat(const char *path, struct stat *value) {
    static int (*next)(const char *, struct stat *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "stat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    if (actual == NULL || next(actual, value) != 0) return -1;
    apply_fake_nlink(actual, &value->st_nlink, &value->st_ino);
    apply_dev_shm_mode(path, &value->st_mode);
    apply_guest_ownership(actual, &value->st_uid, &value->st_gid);
    return 0;
}

int stat64(const char *path, struct stat64 *value) {
    static int (*next)(const char *, struct stat64 *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "stat64");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer, 1);
    if (actual == NULL || next(actual, value) != 0) return -1;
    apply_fake_nlink(actual, &value->st_nlink, &value->st_ino);
    apply_dev_shm_mode(path, &value->st_mode);
    apply_guest_ownership(actual, &value->st_uid, &value->st_gid);
    return 0;
}

int lstat(const char *path, struct stat *value) {
    static int (*next)(const char *, struct stat *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "lstat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL || next(actual, value) != 0) return -1;
    apply_fake_nlink(actual, &value->st_nlink, &value->st_ino);
    apply_dev_shm_mode(path, &value->st_mode);
    apply_guest_ownership(actual, &value->st_uid, &value->st_gid);
    return 0;
}

int lstat64(const char *path, struct stat64 *value) {
    static int (*next)(const char *, struct stat64 *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "lstat64");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL || next(actual, value) != 0) return -1;
    apply_fake_nlink(actual, &value->st_nlink, &value->st_ino);
    apply_dev_shm_mode(path, &value->st_mode);
    apply_guest_ownership(actual, &value->st_uid, &value->st_gid);
    return 0;
}

int fstatat(int directory, const char *path, struct stat *value, int flags) {
    static int (*next)(int, const char *, struct stat *, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fstatat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & AT_SYMLINK_NOFOLLOW) == 0);
    if (actual == NULL || next(directory, actual, value, flags) != 0)
        return -1;
    apply_fake_nlink(actual, &value->st_nlink, &value->st_ino);
    apply_dev_shm_mode(path, &value->st_mode);
    apply_guest_ownership(actual, &value->st_uid, &value->st_gid);
    return 0;
}

int fstatat64(int directory, const char *path, struct stat64 *value,
              int flags) {
    static int (*next)(int, const char *, struct stat64 *, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fstatat64");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & AT_SYMLINK_NOFOLLOW) == 0);
    if (actual == NULL || next(directory, actual, value, flags) != 0)
        return -1;
    apply_fake_nlink(actual, &value->st_nlink, &value->st_ino);
    apply_dev_shm_mode(path, &value->st_mode);
    apply_guest_ownership(actual, &value->st_uid, &value->st_gid);
    return 0;
}

int statx(int directory, const char *path, int flags, unsigned int mask,
          struct statx *value) {
    static int (*next)(int, const char *, int, unsigned int, struct statx *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "statx");
    /* Node/libuv uses statx, not stat. Without a rewrite, existsSync("/bin/bash")
     * looks at Android and VS Code falls back to /bin/sh. */
    if ((flags & AT_EMPTY_PATH) != 0 && (path == NULL || path[0] == '\0')) {
        if (next(directory, path, flags, mask, value) == 0) {
            if (!(value->stx_mask & 0x1000U))
                fill_statx_mount_id(value, directory, path, flags, mask);
            return 0;
        }
        if (errno != ENOSYS && errno != EPERM)
            return -1;
        return bionicx_statx(directory, path, flags, mask, value);
    }
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_open_path(path, buffer,
            (flags & AT_SYMLINK_NOFOLLOW) == 0);
    if (actual == NULL) return -1;
    if (next(directory, actual, flags, mask, value) != 0) {
        if (errno != ENOSYS && errno != EPERM)
            return -1;
        if (bionicx_statx(directory, actual, flags, mask, value) != 0)
            return -1;
    }
    if (!(value->stx_mask & 0x1000U))
        fill_statx_mount_id(value, directory, actual, flags, mask);
    if (guest_system_path(actual)) {
        value->stx_uid = 0;
        value->stx_gid = 0;
    }
    nlink_t nlink = (nlink_t)value->stx_nlink;
    apply_fake_nlink(actual, &nlink, NULL);
    value->stx_nlink = nlink;
    if (is_dev_shm_dir(path))
        value->stx_mode |= S_ISVTX | 0777;
    return 0;
}

/* Android's / is often 100% full. glibc apps that statvfs("/") or an
 * unredirected /usr think there is no space and skip writes / resource
 * copies. Reuse the app-private rootfs numbers when the kernel reports
 * zero available blocks. */
static void replenish_statvfs(struct statvfs *info) {
    static int (*next)(const char *, struct statvfs *);
    if (info == NULL || info->f_bavail != 0) return;
    if (next == NULL) next = dlsym(RTLD_NEXT, "statvfs");
    const char *root = bionicx_captured_rootfs();
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/') return;
    struct statvfs fresh;
    if (next(root, &fresh) == 0 && fresh.f_bavail != 0) *info = fresh;
}

static void replenish_statfs(struct statfs *info) {
    static int (*next)(const char *, struct statfs *);
    if (info == NULL || info->f_bavail != 0) return;
    if (next == NULL) next = dlsym(RTLD_NEXT, "statfs");
    const char *root = bionicx_captured_rootfs();
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/') return;
    struct statfs fresh;
    if (next(root, &fresh) == 0 && fresh.f_bavail != 0) *info = fresh;
}

int statvfs(const char *path, struct statvfs *info) {
    static int (*next)(const char *, struct statvfs *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "statvfs");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    if (next(actual, info) != 0) return -1;
    replenish_statvfs(info);
    return 0;
}

int statvfs64(const char *path, struct statvfs64 *info) {
    return statvfs(path, (struct statvfs *)info);
}

int statfs(const char *path, struct statfs *info) {
    static int (*next)(const char *, struct statfs *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "statfs");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    if (next(actual, info) != 0) return -1;
    replenish_statfs(info);
    return 0;
}

int statfs64(const char *path, struct statfs64 *info) {
    return statfs(path, (struct statfs *)info);
}

int unlink(const char *path) {
    static int (*next)(const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "unlink");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    forget_fake_link(actual);
    return next(actual);
}

int unlinkat(int directory, const char *path, int flags) {
    static int (*next)(int, const char *, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "unlinkat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    forget_fake_link(actual);
    return next(directory, actual, flags);
}


static int link_copy_forced(void) {
    const char *force = bionicx_getenv("BIONICX_FORCE_LINK_COPY");
    return force != NULL && strcmp(force, "1") == 0;
}

static int link_copy_errno(int error) {
    return error == EACCES || error == EPERM || error == EOPNOTSUPP ||
            error == ENOSYS || error == EXDEV;
}

/* Android app-data f2fs often denies link(2). dpkg backs up status with
 * link(status, status-old); a same-directory copy is enough for that. */
static int copy_regular_file(const char *old_path, const char *new_path,
                             int follow) {
    static int (*real_open)(const char *, int, ...);
    static int (*real_unlink)(const char *);
    if (real_open == NULL) real_open = dlsym(RTLD_NEXT, "open");
    if (real_unlink == NULL) real_unlink = dlsym(RTLD_NEXT, "unlink");
    int flags = O_RDONLY | O_CLOEXEC;
    if (!follow) flags |= O_NOFOLLOW;
    int input = real_open(old_path, flags);
    if (input < 0) return -1;
    struct stat info;
    if (fstat(input, &info) != 0) {
        close(input);
        return -1;
    }
    if (!S_ISREG(info.st_mode)) {
        close(input);
        errno = EPERM;
        return -1;
    }
    int output = real_open(new_path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                           info.st_mode & 0777);
    if (output < 0) {
        close(input);
        return -1;
    }
    char block[8192];
    for (;;) {
        ssize_t got = read(input, block, sizeof(block));
        if (got == 0) break;
        if (got < 0) {
            int saved = errno;
            close(input);
            close(output);
            real_unlink(new_path);
            errno = saved;
            return -1;
        }
        ssize_t sent = 0;
        while (sent < got) {
            ssize_t wrote = write(output, block + sent, (size_t)(got - sent));
            if (wrote < 0) {
                int saved = errno;
                close(input);
                close(output);
                real_unlink(new_path);
                errno = saved;
                return -1;
            }
            sent += wrote;
        }
    }
    close(input);
    if (close(output) != 0) {
        int saved = errno;
        real_unlink(new_path);
        errno = saved;
        return -1;
    }
    return 0;
}

int link(const char *old_path, const char *new_path) {
    static int (*next)(const char *, const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "link");
    char old_buffer[PATH_MAX], new_buffer[PATH_MAX];
    const char *actual_old = bionicx_redirect_path(old_path, old_buffer);
    const char *actual_new = bionicx_redirect_path(new_path, new_buffer);
    if (actual_old == NULL || actual_new == NULL) return -1;
    if (link_copy_forced()) {
        if (copy_regular_file(actual_old, actual_new, 0) != 0) return -1;
        remember_fake_link(actual_old, actual_new);
        return 0;
    }
    int result = next(actual_old, actual_new);
    if (result == 0 || !link_copy_errno(errno)) return result;
    if (copy_regular_file(actual_old, actual_new, 0) != 0) return -1;
    remember_fake_link(actual_old, actual_new);
    return 0;
}

static int resolve_at_path(int directory, const char *path, char out[PATH_MAX]) {
    if (path != NULL && path[0] == '/') {
        if (snprintf(out, PATH_MAX, "%s", path) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
        return 0;
    }
    if (directory == AT_FDCWD) {
        if (getcwd(out, PATH_MAX) == NULL) return -1;
        if (path != NULL && path[0] != '\0') {
            size_t used = strlen(out);
            if (snprintf(out + used, PATH_MAX - used, "/%s", path) >=
                    (int)(PATH_MAX - used)) {
                errno = ENAMETOOLONG;
                return -1;
            }
        }
        return 0;
    }
    static ssize_t (*real_readlink)(const char *, char *, size_t);
    if (real_readlink == NULL) real_readlink = dlsym(RTLD_NEXT, "readlink");
    char fd_path[64];
    snprintf(fd_path, sizeof(fd_path), "/proc/self/fd/%d", directory);
    char directory_path[PATH_MAX];
    ssize_t length = real_readlink(fd_path, directory_path,
                                   sizeof(directory_path) - 1);
    if (length < 0) return -1;
    directory_path[length] = '\0';
    if (path != NULL && path[0] != '\0') {
        if (snprintf(out, PATH_MAX, "%s/%s", directory_path, path) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return -1;
        }
    } else if (snprintf(out, PATH_MAX, "%s", directory_path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int copy_linkat(int old_directory, const char *old_path,
                       int new_directory, const char *new_path, int flags) {
    char old_resolved[PATH_MAX], new_resolved[PATH_MAX];
    char old_redirect[PATH_MAX], new_redirect[PATH_MAX];
    const char *source;
    if ((flags & AT_EMPTY_PATH) != 0 &&
            (old_path == NULL || old_path[0] == '\0')) {
        if (snprintf(old_resolved, sizeof(old_resolved), "/proc/self/fd/%d",
                     old_directory) >= (int)sizeof(old_resolved)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        source = old_resolved;
    } else {
        if (resolve_at_path(old_directory, old_path, old_resolved) != 0)
            return -1;
        source = bionicx_redirect_path(old_resolved, old_redirect);
        if (source == NULL) return -1;
    }
    if (resolve_at_path(new_directory, new_path, new_resolved) != 0) return -1;
    const char *destination = bionicx_redirect_path(new_resolved, new_redirect);
    if (destination == NULL) return -1;
    int follow = (flags & (AT_SYMLINK_FOLLOW | AT_EMPTY_PATH)) != 0;
    if (copy_regular_file(source, destination, follow) != 0) return -1;
    remember_fake_link(source, destination);
    return 0;
}

int linkat(int old_directory, const char *old_path, int new_directory,
           const char *new_path, int flags) {
    static int (*next)(int, const char *, int, const char *, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "linkat");
    char old_buffer[PATH_MAX], new_buffer[PATH_MAX];
    const char *actual_old = bionicx_redirect_path(old_path, old_buffer);
    const char *actual_new = bionicx_redirect_path(new_path, new_buffer);
    if (actual_old == NULL || actual_new == NULL) return -1;
    if (link_copy_forced())
        return copy_linkat(old_directory, actual_old, new_directory,
                           actual_new, flags);
    int result = next(old_directory, actual_old, new_directory, actual_new,
                      flags);
    if (result == 0 || !link_copy_errno(errno)) return result;
    return copy_linkat(old_directory, actual_old, new_directory, actual_new,
                       flags);
}


static int redirect_socket_address(const struct sockaddr *address,
                                   socklen_t length,
                                   struct sockaddr_un *translated,
                                   const struct sockaddr **actual,
                                   socklen_t *actual_length,
                                   int *directory_fd) {
    *actual = address;
    *actual_length = length;
    *directory_fd = -1;
    if (address == NULL || address->sa_family != AF_UNIX) return 0;
    const struct sockaddr_un *unix_address = (const struct sockaddr_un *)address;
    char socket_path[sizeof(translated->sun_path)];
    if (unix_address->sun_path[0] == '\0') return 0;
    char buffer[PATH_MAX];
    const char *redirected = bionicx_redirect_path(unix_address->sun_path, buffer);
    if (redirected == NULL) return -1;
    if (redirected == unix_address->sun_path) return 0;
    size_t path_length = strlen(redirected);
    if (path_length >= sizeof(translated->sun_path)) {
        const char *slash = strrchr(redirected, '/');
        char directory[PATH_MAX];
        size_t directory_length;
        int written;

        if (slash == NULL || slash[1] == '\0') {
            errno = ENAMETOOLONG;
            return -1;
        }
        directory_length = (size_t)(slash - redirected);
        if (directory_length == 0 || directory_length >= sizeof(directory)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        memcpy(directory, redirected, directory_length);
        directory[directory_length] = '\0';
        *directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        if (*directory_fd < 0)
            return -1;
        written = snprintf(socket_path, sizeof(socket_path), "/proc/self/fd/%d/%s",
                           *directory_fd, slash + 1);
        if (written < 0 || (size_t)written >= sizeof(socket_path)) {
            int saved = errno;
            close(*directory_fd);
            *directory_fd = -1;
            errno = written < 0 ? saved : ENAMETOOLONG;
            return -1;
        }
        redirected = socket_path;
        path_length = (size_t)written;
    }
    memset(translated, 0, sizeof(*translated));
    translated->sun_family = AF_UNIX;
    memcpy(translated->sun_path, redirected, path_length + 1);
    *actual = (const struct sockaddr *)translated;
    *actual_length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                 path_length + 1);
    return 0;
}

static void log_unix_socket(const char *fn, int fd,
                            const struct sockaddr *address, socklen_t length,
                            int result) {
    const struct sockaddr_un *unix_address;
    if (bionicx_getenv("BIONICX_LOG_EXEC") == NULL) return;
    if (address == NULL || address->sa_family != AF_UNIX) return;
    unix_address = (const struct sockaddr_un *)address;
    if (length > (socklen_t)offsetof(struct sockaddr_un, sun_path) &&
            unix_address->sun_path[0] == '\0') {
        fprintf(stderr, "bionicx-%s: fd=%d abstract @%s rc=%d errno=%d\n",
                fn, fd, unix_address->sun_path + 1, result,
                result < 0 ? errno : 0);
    } else {
        fprintf(stderr, "bionicx-%s: fd=%d path=%s rc=%d errno=%d\n",
                fn, fd, unix_address->sun_path, result,
                result < 0 ? errno : 0);
    }
    fflush(stderr);
}

int bind(int socket, const struct sockaddr *address, socklen_t length) {
    static int (*next)(int, const struct sockaddr *, socklen_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "bind");
    struct sockaddr_un translated;
    const struct sockaddr *actual;
    socklen_t actual_length;
    int directory_fd;
    int saved;
    int result;
    if (redirect_socket_address(address, length, &translated, &actual,
                                &actual_length, &directory_fd) < 0) return -1;
    result = next(socket, actual, actual_length);
    saved = errno;
    if (directory_fd >= 0) close(directory_fd);
    log_unix_socket("bind", socket, actual, actual_length, result);
    errno = saved;
    return result;
}

int connect(int socket, const struct sockaddr *address, socklen_t length) {
    static int (*next)(int, const struct sockaddr *, socklen_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "connect");
    struct sockaddr_un translated;
    const struct sockaddr *actual;
    socklen_t actual_length;
    int directory_fd;
    int saved;
    int result;
    if (redirect_socket_address(address, length, &translated, &actual,
                                &actual_length, &directory_fd) < 0) return -1;
    result = next(socket, actual, actual_length);
    saved = errno;
    if (directory_fd >= 0) close(directory_fd);
    log_unix_socket("connect", socket, actual, actual_length, result);
    errno = saved;
    return result;
}

int listen(int socket, int backlog) {
    static int (*next)(int, int);
    struct sockaddr_storage name;
    socklen_t name_length = sizeof(name);
    int saved;
    int result;
    if (next == NULL) next = dlsym(RTLD_NEXT, "listen");
    result = next(socket, backlog);
    saved = errno;
    if (bionicx_getenv("BIONICX_LOG_EXEC") != NULL &&
            getsockname(socket, (struct sockaddr *)&name, &name_length) == 0)
        log_unix_socket("listen", socket, (struct sockaddr *)&name,
                        name_length, result);
    errno = saved;
    return result;
}

int close(int fd) {
    static int (*next)(int);
    struct sockaddr_storage name;
    socklen_t name_length = sizeof(name);
    if (next == NULL) next = dlsym(RTLD_NEXT, "close");
    if (bionicx_getenv("BIONICX_LOG_EXEC") != NULL && fd >= 3 &&
            getsockname(fd, (struct sockaddr *)&name, &name_length) == 0 &&
            name.ss_family == AF_UNIX) {
        log_unix_socket("close", fd, (struct sockaddr *)&name, name_length, 0);
        fprintf(stderr, "bionicx-close: fd=%d caller=%p\n",
                fd, __builtin_return_address(0));
        fflush(stderr);
    }
    return next(fd);
}
int chroot(const char *path) {
    const char *root = bionicx_captured_rootfs();
    if (root == NULL) root = bionicx_getenv("BIONICX_ROOTFS");
    size_t root_length = root ? strlen(root) : 0;
    if (root_length && strncmp(path, root, root_length) == 0 &&
            strspn(path + root_length, "/") == strlen(path + root_length)) {
        if (setenv("BIONICX_VIRTUAL_ROOT", "1", 1) != 0)
            return -1;
        /* dpkg chroot()s then chdir("/"). Make that the guest rootfs. */
        return chdir("/");
    }
    return bionicx_ns_chroot(path);
}

int __xstat(int version, const char *path, struct stat *st) {
    (void)version; return stat(path, st);
}
int __xstat64(int version, const char *path, struct stat64 *st) {
    (void)version; return stat64(path, st);
}
int __fxstatat(int version, int fd, const char *path, struct stat *st, int flags) {
    (void)version; return fstatat(fd, path, st, flags);
}
int __fxstatat64(int version, int fd, const char *path, struct stat64 *st, int flags) {
    (void)version; return fstatat64(fd, path, st, flags);
}

/* libc's setmntent opens internally, bypassing the public fopen interposer. */
FILE *setmntent(const char *path, const char *mode) {
    static FILE *(*next)(const char *, const char *);
    char buffer[PATH_MAX];
    if (!next) next = dlsym(RTLD_NEXT, "setmntent");
    return next(bionicx_redirect_path(path, buffer), mode);
}
