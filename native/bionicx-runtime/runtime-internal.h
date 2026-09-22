#ifndef BIONICX_ROOTFS_INTERNAL_H
#define BIONICX_ROOTFS_INTERNAL_H

#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <grp.h>
#include <pwd.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

#define BIONICX_INTERNAL __attribute__((visibility("hidden")))

static inline const char *bionicx_getenv(const char *name) {
    size_t name_length = 0;
    while (name[name_length] != '\0') ++name_length;
    for (char **item = environ; item != NULL && *item != NULL; ++item) {
        size_t index = 0;
        while (index < name_length && (*item)[index] == name[index]) ++index;
        if (index == name_length && (*item)[index] == '=') {
            return *item + index + 1;
        }
    }
    return NULL;
}

BIONICX_INTERNAL const char *bionicx_captured_rootfs(void);
BIONICX_INTERNAL const char *bionicx_captured_tmpdir(void);
BIONICX_INTERNAL const char *bionicx_guest_execfn(void);

BIONICX_INTERNAL const char *bionicx_captured_value(const char *name);
BIONICX_INTERNAL int bionicx_fork_exec(int (*fn)(void *), void *arg, int *pidfd);
/* envp first. GPU-switch names omitted from a full guest envp stay unset;
 * dpkg/zygote envp (no BIONICX_ROOTFS) still refill from getenv/capture. */
BIONICX_INTERNAL const char *bionicx_env_lookup(
        char *const environment[], const char *name);
BIONICX_INTERNAL void bionicx_restore_runtime_environment(void);
BIONICX_INTERNAL void bionicx_ensure_rootfs_path(void);
BIONICX_INTERNAL int bionicx_package_transaction(void);
BIONICX_INTERNAL int bionicx_path_is_proc_exe(const char *path);
BIONICX_INTERNAL int bionicx_save_execfn(char saved[PATH_MAX]);
BIONICX_INTERNAL void bionicx_assign_execfn(const char *path);
BIONICX_INTERNAL void bionicx_restore_execfn(int had, const char saved[PATH_MAX]);
BIONICX_INTERNAL char **bionicx_with_runtime_environment(
        char *const environment[], size_t *owned_from);
BIONICX_INTERNAL void bionicx_free_runtime_environment(
        char **merged, size_t owned_from);
BIONICX_INTERNAL int bionicx_path_is_overlay_loader(const char *path);
BIONICX_INTERNAL const char *bionicx_redirect_path(
        const char *path, char buffer[PATH_MAX]);
BIONICX_INTERNAL mode_t bionicx_optional_mode(
        int flags, va_list arguments);
BIONICX_INTERNAL int bionicx_ignore_ownership_failure(int result);
BIONICX_INTERNAL int bionicx_is_file_capability_xattr(const char *name);
BIONICX_INTERNAL int bionicx_ignore_file_capability_failure(int result);
BIONICX_INTERNAL int bionicx_statx(int dirfd, const char *path, int flags,
                                   unsigned int mask, void *buf);

BIONICX_INTERNAL pid_t bionicx_host_pid(void);
BIONICX_INTERNAL pid_t bionicx_ns_host_pid(pid_t);
BIONICX_INTERNAL pid_t bionicx_ns_guest_pid(pid_t);
BIONICX_INTERNAL const char *bionicx_ns_path(const char *, char [PATH_MAX]);
BIONICX_INTERNAL int bionicx_ns_chroot(const char *);
BIONICX_INTERNAL int bionicx_ns_access(const char *, int);
BIONICX_INTERNAL int bionicx_ns_open(int, const char *, int);
BIONICX_INTERNAL char *bionicx_ns_environment(void);
BIONICX_INTERNAL void bionicx_ns_export(void);
BIONICX_INTERNAL int bionicx_ns_syscall(long, long, long, long, long, long, long, long *);
#endif
