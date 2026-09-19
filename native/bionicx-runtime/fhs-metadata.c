#include "runtime-internal.h"

#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/xattr.h>
#include <utime.h>

#define FILE_CAPABILITY_XATTR "security.capability"

int bionicx_is_file_capability_xattr(const char *name) {
    return name != NULL && strcmp(name, FILE_CAPABILITY_XATTR) == 0;
}

int bionicx_ignore_file_capability_failure(int result) {
    if (result < 0 && (errno == EPERM || errno == EACCES || errno == ENOSYS
            || errno == EOPNOTSUPP)) {
        errno = 0;
        return 0;
    }
    return result;
}

int chmod(const char *path, mode_t mode) {
    static int (*next)(const char *, mode_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "chmod");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    return actual != NULL ? next(actual, mode) : -1;
}

int fchmodat(int directory, const char *path, mode_t mode, int flags) {
    static int (*next)(int, const char *, mode_t, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fchmodat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    int result = next(directory, actual, mode, flags);
#ifdef AT_EMPTY_PATH
    if (result < 0 && errno == EINVAL && (flags & AT_EMPTY_PATH) != 0 &&
            actual[0] == '\0') return fchmod(directory, mode);
#endif
#ifdef AT_SYMLINK_NOFOLLOW
    if (result < 0 && errno == EINVAL &&
            (flags & AT_SYMLINK_NOFOLLOW) != 0)
        return next(directory, actual, mode, flags & ~AT_SYMLINK_NOFOLLOW);
#endif
    return result;
}

int bionicx_ignore_ownership_failure(int result) {
    if (result < 0 && (errno == EPERM || errno == EACCES || errno == EINVAL)) {
        errno = 0;
        return 0;
    }
    return result;
}

int chown(const char *path, uid_t owner, gid_t group) {
    static int (*next)(const char *, uid_t, gid_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "chown");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    return bionicx_ignore_ownership_failure(next(actual, owner, group));
}

int lchown(const char *path, uid_t owner, gid_t group) {
    static int (*next)(const char *, uid_t, gid_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "lchown");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    return bionicx_ignore_ownership_failure(next(actual, owner, group));
}

int fchownat(int directory, const char *path, uid_t owner, gid_t group,
             int flags) {
    static int (*next)(int, const char *, uid_t, gid_t, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fchownat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    return bionicx_ignore_ownership_failure(
            next(directory, actual, owner, group, flags));
}

int fchown(int descriptor, uid_t owner, gid_t group) {
    static int (*next)(int, uid_t, gid_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fchown");
    return bionicx_ignore_ownership_failure(next(descriptor, owner, group));
}

int setxattr(const char *path, const char *name, const void *value, size_t size,
             int flags) {
    static int (*next)(const char *, const char *, const void *, size_t, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "setxattr");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    int result = next(actual, name, value, size, flags);
    return bionicx_is_file_capability_xattr(name)
            ? bionicx_ignore_file_capability_failure(result) : result;
}

int lsetxattr(const char *path, const char *name, const void *value, size_t size,
              int flags) {
    static int (*next)(const char *, const char *, const void *, size_t, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "lsetxattr");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    int result = next(actual, name, value, size, flags);
    return bionicx_is_file_capability_xattr(name)
            ? bionicx_ignore_file_capability_failure(result) : result;
}

int fsetxattr(int fd, const char *name, const void *value, size_t size, int flags) {
    static int (*next)(int, const char *, const void *, size_t, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "fsetxattr");
    int result = next(fd, name, value, size, flags);
    return bionicx_is_file_capability_xattr(name)
            ? bionicx_ignore_file_capability_failure(result) : result;
}

int utime(const char *path, const struct utimbuf *times) {
    static int (*next)(const char *, const struct utimbuf *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "utime");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    return actual != NULL ? next(actual, times) : -1;
}

int utimes(const char *path, const struct timeval times[2]) {
    static int (*next)(const char *, const struct timeval *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "utimes");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    return actual != NULL ? next(actual, times) : -1;
}

int lutimes(const char *path, const struct timeval times[2]) {
    static int (*next)(const char *, const struct timeval *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "lutimes");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    return actual != NULL ? next(actual, times) : -1;
}

int utimensat(int directory, const char *path, const struct timespec times[2],
              int flags) {
    static int (*next)(int, const char *, const struct timespec *, int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "utimensat");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    return actual != NULL ? next(directory, actual, times, flags) : -1;
}
