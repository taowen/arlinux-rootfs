/* Android application-UID namespace compatibility, not kernel isolation.
 * All processes retain the Android UID and SELinux sandbox. See
 * docs/namespace-compat.md for the deliberately supported syscall surface. */
#include "runtime-internal.h"
#include <dlfcn.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <pthread.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>

#define NS_FLAGS (CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)
#define NS_ENV "BIONICX_NS_STATE"
struct map_descriptor { dev_t device; ino_t inode; int kind; int mode; };
struct ns_state {
    unsigned version;
    unsigned active;
    pid_t init;
    unsigned user;
    struct __user_cap_data_struct caps[2];
    char maps[3][256];
    struct map_descriptor descriptors[32];
};
static struct ns_state state = {.version = 1};
static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static char *fsroot;
static long (*host_call)(long, ...);
static int (*host_clone)(int (*)(void *), void *, int, void *, ...);

static long raw(long n, long a, long b, long c, long d, long e, long f) {
    if (!host_call) host_call = dlsym(RTLD_NEXT, "syscall");
    return host_call(n, a, b, c, d, e, f);
}
pid_t bionicx_host_pid(void) { return raw(SYS_getpid, 0, 0, 0, 0, 0, 0); }
static int copy_fs(void) {
    char *copy = mmap(NULL, PATH_MAX, PROT_READ | PROT_WRITE,
                      MAP_ANONYMOUS | MAP_SHARED, -1, 0);
    if (copy == MAP_FAILED) return -1;
    if (fsroot) {
        memcpy(copy, fsroot, PATH_MAX);
        munmap(fsroot, PATH_MAX);
    }
    fsroot = copy;
    return 0;
}
static void fork_prepare(void) { pthread_mutex_lock(&map_lock); }
static void fork_parent(void) { pthread_mutex_unlock(&map_lock); }
static void fork_child(void) {
    pthread_mutex_unlock(&map_lock);
    if (copy_fs() != 0) _exit(125);
}
static void new_user(void) {
    state.user = 1;
    memset(state.maps, 0, sizeof state.maps);
    memset(state.descriptors, 0, sizeof state.descriptors);
    strcpy(state.maps[2], "allow\n");
    uint64_t all = (UINT64_C(1) << (CAP_LAST_CAP + 1)) - 1;
    memset(state.caps, 0, sizeof state.caps);
    state.caps[0].effective = state.caps[0].permitted = (uint32_t)all;
    state.caps[1].effective = state.caps[1].permitted = (uint32_t)(all >> 32);
}
static void child_state(unsigned flags) {
    if (!(flags & CLONE_FS) && copy_fs() != 0) _exit(125);
    if (flags & CLONE_NEWPID) state.init = bionicx_host_pid();
    if (flags & CLONE_NEWUSER) new_user();
}
/* Encoding is bounded, versioned, and independent of inherited descriptors:
 * Chromium closes descriptors and constructs a fresh envp before exec. */
char *bionicx_ns_environment(void) {
    if (!state.active && (!fsroot || !fsroot[0])) return NULL;
    size_t rootlen = fsroot ? strlen(fsroot) : 0;
    size_t bytes = sizeof state + rootlen + 1;
    char *s = malloc(bytes * 2 + 1);
    if (!s) return NULL;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < bytes; ++i) {
        unsigned char c = i < sizeof state ? ((unsigned char *)&state)[i]
                          : (unsigned char)(fsroot ? fsroot[i - sizeof state] : 0);
        s[2*i] = hex[c >> 4]; s[2*i+1] = hex[c & 15];
    }
    s[bytes*2] = 0;
    return s;
}
void bionicx_ns_export(void) {
    char *value = bionicx_ns_environment();
    if (value) { setenv(NS_ENV, value, 1); free(value); }
    else unsetenv(NS_ENV);
}
__attribute__((constructor)) static void namespace_init(void) {
    host_call = dlsym(RTLD_NEXT, "syscall");
    host_clone = dlsym(RTLD_NEXT, "clone");
    if (copy_fs() != 0) _exit(125);
    const char *s = bionicx_getenv(NS_ENV);
    if (s) {
        size_t n = strlen(s);
        unsigned char decoded[sizeof state + PATH_MAX];
        int valid = !(n & 1) && n / 2 > sizeof state && n / 2 <= sizeof decoded;
        for (size_t i = 0; valid && i < n; i += 2) {
            unsigned v = 0;
            for (size_t j = 0; j < 2; ++j) {
                char c = s[i+j];
                if (c >= '0' && c <= '9') v = v*16 + c-'0';
                else if (c >= 'a' && c <= 'f') v = v*16 + c-'a'+10;
                else { valid = 0; break; }
            }
            decoded[i/2] = v;
        }
        if (valid && decoded[n/2-1] == 0) {
            struct ns_state restored;
            memcpy(&restored, decoded, sizeof restored);
            if (restored.version == 1 && restored.init >= 0) {
                state = restored;
                memcpy(fsroot, decoded + sizeof state, n/2 - sizeof state);
            }
        }
    }
    pthread_atfork(fork_prepare, fork_parent, fork_child);
}
pid_t bionicx_ns_host_pid(pid_t pid) {
    return pid == 1 && state.init ? state.init : pid;
}
pid_t bionicx_ns_guest_pid(pid_t pid) {
    return pid > 0 && pid == state.init ? 1 : pid;
}
pid_t getpid(void) {
    pid_t p = bionicx_host_pid();
    return bionicx_ns_guest_pid(p);
}
pid_t getppid(void) {
    if (bionicx_host_pid() == state.init) return 0;
    pid_t p = raw(SYS_getppid, 0, 0, 0, 0, 0, 0);
    return bionicx_ns_guest_pid(p);
}
int bionicx_ns_access(const char *path, int mode) {
    if (path && (!fsroot || !fsroot[0]) && mode == F_OK &&
        (!strcmp(path, "/proc/self/ns/user") ||
         !strcmp(path, "/proc/self/ns/pid") ||
         !strcmp(path, "/proc/self/ns/net"))) {
        state.active = 1;
        return 1;
    }
    return 0;
}
const char *bionicx_ns_path(const char *path, char buffer[PATH_MAX]) {
    if (!path || path[0] != '/') return path;
    if (fsroot && fsroot[0]) {
        /* The runtime passes resolved physical paths between its FHS and
         * explicit-loader helpers. Keep that existing Arlinux convention. */
        size_t rootlen = strlen(fsroot);
        if (!strncmp(path, fsroot, rootlen) &&
            (path[rootlen] == '/' || path[rootlen] == 0)) return path;
        int n = snprintf(buffer, PATH_MAX, "%s%s", fsroot, path);
        if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
        return buffer;
    }
    if (state.init && !strncmp(path, "/proc/1", 7) &&
        (path[7] == '/' || path[7] == 0)) {
        int n = snprintf(buffer, PATH_MAX, "/proc/%d%s", state.init, path + 7);
        if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
        return buffer;
    }
    return path;
}
int bionicx_ns_chroot(const char *path) {
    if (!path) { errno = EFAULT; return -1; }
    char resolved[PATH_MAX], translated[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, translated);
    if (!actual) return -1;
    if (!strncmp(actual, "/proc/self/", 11)) {
        int n = snprintf(resolved, sizeof resolved, "/proc/%d/%s",
                         bionicx_host_pid(), actual + 11);
        if (n < 0 || n >= PATH_MAX) { errno = ENAMETOOLONG; return -1; }
    } else {
        static char *(*host_realpath)(const char *, char *);
        if (!host_realpath) host_realpath = dlsym(RTLD_NEXT, "realpath");
        if (!host_realpath(actual, resolved)) return -1;
    }
    struct stat st;
    static int (*host_stat)(const char *, struct stat *);
    if (!host_stat) host_stat = dlsym(RTLD_NEXT, "stat");
    if (host_stat(resolved, &st) != 0) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }
    if (!fsroot && copy_fs() != 0) return -1;
    strcpy(fsroot, resolved);
    state.active = 1;
    return 0;
}
int unshare(int flags) {
    if (flags & ~(CLONE_NEWUSER | CLONE_NEWNET | CLONE_FS)) {
        errno = EINVAL; return -1;
    }
    if ((flags & CLONE_FS) && copy_fs() != 0) return -1;
    if (flags & CLONE_NEWUSER) new_user();
    if (flags & NS_FLAGS) state.active = 1;
    return 0;
}
struct callback { int (*fn)(void *); void *arg; unsigned flags; };
static int cloned(void *opaque) {
    pthread_mutex_unlock(&map_lock);
    struct callback c = *(struct callback *)opaque;
    free(opaque);
    child_state(c.flags);
    return c.fn(c.arg);
}
int clone(int (*fn)(void *), void *stack, int flags, void *arg, ...) {
    va_list ap;
    va_start(ap, arg);
    void *ptid = va_arg(ap, void *), *tls = va_arg(ap, void *),
         *ctid = va_arg(ap, void *);
    va_end(ap);
    if (!host_clone) host_clone = dlsym(RTLD_NEXT, "clone");
    if (!fn || !stack) { errno = EINVAL; return -1; }
    if (flags == (CLONE_VM | CLONE_VFORK | CLONE_PIDFD | SIGCHLD))
        return bionicx_fork_exec(fn, arg, ptid);
    if ((flags & NS_FLAGS) && (flags & (CLONE_VM | CLONE_THREAD))) {
        errno = ENOTSUP; return -1;
    }
    if ((flags & CLONE_NEWUSER) && (flags & CLONE_FS)) {
        errno = EINVAL; return -1;
    }
    if (flags & NS_FLAGS) state.active = 1;
    if (flags & CLONE_VM)
        return host_clone(fn, stack, flags, arg, ptid, tls, ctid);
    struct callback *c = malloc(sizeof *c);
    if (!c) return -1;
    *c = (struct callback){fn, arg, (unsigned)flags};
    pthread_mutex_lock(&map_lock);
    int result = host_clone(cloned, stack, flags & ~NS_FLAGS, c, ptid, tls, ctid);
    int saved = errno;
    pthread_mutex_unlock(&map_lock);
    free(c); errno = saved;
    return result;
}
int prctl(int option, ...) {
    va_list ap;
    va_start(ap, option);
    unsigned long a = va_arg(ap, unsigned long), b = va_arg(ap, unsigned long),
                  c = va_arg(ap, unsigned long), d = va_arg(ap, unsigned long);
    va_end(ap);
    /* Guest syscall translation and SIGSYS handling cannot support additional
     * guest filters. Capability probes must agree before and after namespace
     * setup. PR_GET_SECCOMP still reports the inherited Android filter. */
    if (option == PR_SET_SECCOMP) { errno = EINVAL; return -1; }
    return raw(SYS_prctl, option, a, b, c, d, 0);
}
/* Return 1 when handled; preserve libc's -1/errno convention. */
int bionicx_ns_syscall(long n, long a, long b, long c, long d, long e, long f,
                       long *result) {
    if (n == SYS_getpid) *result = getpid();
    else if (n == SYS_getppid) *result = getppid();
    else if (n == SYS_kill) *result = kill(a, b);
    else if (n == SYS_unshare) *result = unshare(a);
    else if (n == SYS_chroot) {
        if (!a) { errno = EFAULT; *result = -1; }
        else *result = chroot((const char *)a);
    }
    else if (n == SYS_wait4) {
        *result = raw(n, bionicx_ns_host_pid(a), b, c, d, e, f);
        if (*result > 0) *result = bionicx_ns_guest_pid(*result);
    }
    else if (n == SYS_waitid) {
        *result = raw(n, a, a == P_PID ? bionicx_ns_host_pid(b) : b, c, d, e, f);
        if (*result == 0 && c) {
            siginfo_t *info = (void *)c;
            info->si_pid = bionicx_ns_guest_pid(info->si_pid);
        }
    }
#ifdef SYS_pidfd_open
    else if (n == SYS_pidfd_open && a == 1 && state.init)
        *result = raw(n, state.init, b, c, d, e, f);
#endif
    else if (n == SYS_read && state.user) *result = read(a, (void *)b, c);
    else if (n == SYS_write && state.user) *result = write(a, (const void *)b, c);
    else if (n == SYS_prctl) *result = prctl(a, b, c, d, e);
    else if (n == SYS_seccomp) { errno = ENOSYS; *result = -1; }
    else if (n == SYS_clone && ((a & NS_FLAGS) || !(a & CLONE_VM))) {
        if (a & NS_FLAGS) state.active = 1;
        if (b || (a & (CLONE_VM | CLONE_THREAD))) { errno = ENOTSUP; *result = -1; }
        else {
            pthread_mutex_lock(&map_lock);
            *result = raw(n, a & ~NS_FLAGS, b, c, d, e, f);
            int saved = errno;
            pthread_mutex_unlock(&map_lock);
            errno = saved;
            if (!*result) child_state(a);
        }
    } else if ((n == SYS_capget || n == SYS_capset) && state.user) {
        struct __user_cap_header_struct *h = (void *)a;
        struct __user_cap_data_struct *data = (void *)b;
        *result = -1;
        if (!h) errno = EFAULT;
        else if (h->version != _LINUX_CAPABILITY_VERSION_3 &&
                 h->version != _LINUX_CAPABILITY_VERSION_2 &&
                 h->version != _LINUX_CAPABILITY_VERSION_1) {
            h->version = _LINUX_CAPABILITY_VERSION_3; errno = EINVAL;
        } else if (h->pid != 0 && h->pid != getpid() && h->pid != bionicx_host_pid())
            errno = ESRCH;
        else if (!data) errno = EFAULT;
        else {
            size_t size = h->version == _LINUX_CAPABILITY_VERSION_1
                        ? sizeof *data : sizeof state.caps;
            if (n == SYS_capget) { memcpy(data, state.caps, size); *result = 0; }
            else {
                int valid = 1;
                for (size_t i = 0; i < size / sizeof *data; ++i)
                    if ((data[i].effective & ~data[i].permitted) ||
                        (data[i].permitted & ~state.caps[i].permitted) ||
                        (data[i].inheritable & ~(state.caps[i].inheritable |
                                                 state.caps[i].permitted))) valid = 0;
                if (!valid) errno = EPERM;
                else {
                    memcpy(state.caps, data, size);
                    if (size == sizeof *data) memset(&state.caps[1], 0, sizeof *data);
                    *result = 0;
                }
            }
        }
    } else return 0;
    return 1;
}

/* Map descriptors are disposable snapshots. Track their inode, not only the
 * fd number, so dup works and descriptor reuse cannot redirect an unrelated
 * write. The current mapping lives in process state and survives exec. */

static void prune_map_descriptors(void) {
    static DIR *(*host_opendir)(const char *);
    if (!host_opendir) host_opendir = dlsym(RTLD_NEXT, "opendir");
    DIR *dir = host_opendir("/proc/self/fd");
    if (!dir) return;
    unsigned char alive[32] = {0};
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        char *end;
        long fd = strtol(entry->d_name, &end, 10);
        if (*end || end == entry->d_name || fd < 0 || fd > INT_MAX) continue;
        struct stat st;
        if (fstat((int)fd, &st) != 0) continue;
        for (size_t i = 0; i < 32; ++i)
            if (state.descriptors[i].inode == st.st_ino &&
                state.descriptors[i].device == st.st_dev) alive[i] = 1;
    }
    closedir(dir);
    for (size_t i = 0; i < 32; ++i)
        if (!alive[i]) memset(&state.descriptors[i], 0, sizeof state.descriptors[i]);
}
int bionicx_ns_open(int directory, const char *path, int flags) {
    if (!state.user || !path || (fsroot && fsroot[0])) return -2;
    char absolute[PATH_MAX];
    if (path[0] != '/' && directory != AT_FDCWD) {
        char link[64];
        snprintf(link, sizeof link, "/proc/self/fd/%d", directory);
        ssize_t n = raw(SYS_readlinkat, AT_FDCWD, (long)link, (long)absolute,
                        sizeof absolute - 1, 0, 0);
        if (n < 0 || n >= PATH_MAX - 1) return -2;
        absolute[n] = 0;
        size_t len = strlen(path);
        if ((size_t)n + len + 2 > sizeof absolute) return -2;
        absolute[n++] = '/'; memcpy(absolute+n, path, len+1);
        path = absolute;
    }
    char prefix[64];
    snprintf(prefix, sizeof prefix, "/proc/%d/", bionicx_host_pid());
    const char *leaf;
    if (!strncmp(path, "/proc/self/", 11)) leaf = path + 11;
    else if (!strncmp(path, prefix, strlen(prefix))) leaf = path + strlen(prefix);
    else if (getpid() == 1 && !strncmp(path, "/proc/1/", 8)) leaf = path + 8;
    else return -2;
    int kind = !strcmp(leaf, "uid_map") ? 0 : !strcmp(leaf, "gid_map") ? 1
             : !strcmp(leaf, "setgroups") ? 2 : -1;
    if (kind < 0) return -2;
    if (flags & (O_CREAT | O_EXCL | O_DIRECTORY | O_PATH)) {
        errno = EINVAL; return -1;
    }
    int backing = raw(SYS_memfd_create, (long)"arlinux-ns-map", MFD_CLOEXEC, 0, 0, 0, 0);
    if (backing < 0) return -1;
    size_t len = strlen(state.maps[kind]);
    if (len && raw(SYS_write, backing, (long)state.maps[kind], len, 0, 0, 0) < 0) {
        int saved = errno; raw(SYS_close, backing, 0, 0, 0, 0, 0); errno = saved; return -1;
    }
    /* Android SELinux denies reopening a memfd through /proc/self/fd.
     * Keep the original description; libc I/O checks its requested mode. */
    int fd = backing;
    if (!(flags & O_CLOEXEC)) fcntl(fd, F_SETFD, 0);
    if (lseek(fd, 0, SEEK_SET) < 0) { close(fd); return -1; }
    struct stat st;
    if (fstat(fd, &st) != 0) { close(fd); return -1; }
    pthread_mutex_lock(&map_lock);
    /* Fixed table bounds startup resource use; old snapshots remain ordinary
     * files after a new namespace is entered. */
    size_t slot;
    for (slot = 0; slot < 32 && state.descriptors[slot].inode; ++slot) {}
    if (slot == 32) {
        prune_map_descriptors();
        for (slot = 0; slot < 32 && state.descriptors[slot].inode; ++slot) {}
    }
    if (slot == 32) {
        pthread_mutex_unlock(&map_lock); close(fd); errno = EMFILE; return -1;
    }
    state.descriptors[slot] = (struct map_descriptor){st.st_dev, st.st_ino, kind, flags & O_ACCMODE};
    pthread_mutex_unlock(&map_lock);
    return fd;
}
ssize_t write(int fd, const void *buffer, size_t size) {
    static ssize_t (*host_write)(int, const void *, size_t);
    if (!host_write) host_write = dlsym(RTLD_NEXT, "write");
    if (!state.user) return host_write(fd, buffer, size);
    struct stat st;
    int saved = errno;
    if (fstat(fd, &st) != 0) { errno = saved; return host_write(fd, buffer, size); }
    int kind = -1, mode = O_RDWR;
    pthread_mutex_lock(&map_lock);
    for (size_t i = 0; i < 32; ++i)
        if (state.descriptors[i].inode == st.st_ino && state.descriptors[i].device == st.st_dev) {
            kind = state.descriptors[i].kind; mode = state.descriptors[i].mode; break;
        }
    if (kind < 0) {
        pthread_mutex_unlock(&map_lock); errno = saved; return host_write(fd, buffer, size);
    }
    int error = 0;
    char text[256];
    if (mode == O_RDONLY) error = EBADF;
    else if (!buffer) error = EFAULT;
    else if (!size || size >= sizeof text) error = EINVAL;
    else {
        memcpy(text, buffer, size); text[size] = 0;
        if (strlen(text) != size) error = EINVAL;
        else if (kind == 2) {
            if (strcmp(text, "deny") && strcmp(text, "deny\n") &&
                strcmp(text, "allow") && strcmp(text, "allow\n")) error = EINVAL;
            else if (!strncmp(state.maps[2], "deny", 4) && !strncmp(text, "allow", 5)) error = EPERM;
        } else if (state.maps[kind][0]) error = EPERM;
        else {
            unsigned long inside, outside, count;
            int consumed = 0;
            if (sscanf(text, "%lu %lu %lu %n", &inside, &outside, &count, &consumed) != 3 ||
                text[consumed] || !count || inside > UINT32_MAX || outside > UINT32_MAX ||
                count > UINT32_MAX || inside + count > UINT32_MAX || outside + count > UINT32_MAX)
                error = EINVAL;
            /* Mapping changes are logical metadata; Android credentials never change. */
        }
    }
    ssize_t result = -1;
    if (!error) {
        result = host_write(fd, buffer, size);
        if (result == (ssize_t)size) memcpy(state.maps[kind], text, size+1);
    }
    pthread_mutex_unlock(&map_lock);
    if (error) errno = error;
    return result;
}

int kill(pid_t pid, int sig) {
    pid = bionicx_ns_host_pid(pid);
    return raw(SYS_kill, pid, sig, 0, 0, 0, 0);
}
pid_t waitpid(pid_t pid, int *status, int options) {
    static pid_t (*host_waitpid)(pid_t, int *, int);
    if (!host_waitpid) host_waitpid = dlsym(RTLD_NEXT, "waitpid");
    pid = bionicx_ns_host_pid(pid);
    pid_t result = host_waitpid(pid, status, options);
    return bionicx_ns_guest_pid(result);
}

ssize_t read(int fd, void *buffer, size_t size) {
    static ssize_t (*host_read)(int, void *, size_t);
    if (!host_read) host_read = dlsym(RTLD_NEXT, "read");
    if (state.user) {
        struct stat st;
        int saved = errno;
        if (fstat(fd, &st) == 0) {
            int write_only = 0;
            pthread_mutex_lock(&map_lock);
            for (size_t i = 0; i < 32; ++i)
                if (state.descriptors[i].inode == st.st_ino &&
                    state.descriptors[i].device == st.st_dev &&
                    state.descriptors[i].mode == O_WRONLY) write_only = 1;
            pthread_mutex_unlock(&map_lock);
            if (write_only) { errno = EBADF; return -1; }
        }
        errno = saved;
    }
    return host_read(fd, buffer, size);
}
int capget(struct __user_cap_header_struct *header,
           struct __user_cap_data_struct *data) {
    long result;
    if (bionicx_ns_syscall(SYS_capget, (long)header, (long)data, 0, 0, 0, 0, &result))
        return result;
    return raw(SYS_capget, (long)header, (long)data, 0, 0, 0, 0);
}
int capset(struct __user_cap_header_struct *header,
           const struct __user_cap_data_struct *data) {
    long result;
    if (bionicx_ns_syscall(SYS_capset, (long)header, (long)data, 0, 0, 0, 0, &result))
        return result;
    /* Same Android-UID compatibility as the runtime's existing capset trap. */
    return 0;
}
