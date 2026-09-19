#ifndef BIONICX_LIBRARY_PATH_H
#define BIONICX_LIBRARY_PATH_H

#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

static int bionicx_library_path_has_dir(const char *library_path,
                                        const char *dir)
{
    const char *p;
    size_t n;

    if (library_path == NULL || dir == NULL || dir[0] == '\0')
        return 0;
    n = strlen(dir);
    p = library_path;
    while (*p != '\0') {
        const char *colon = strchr(p, ':');
        size_t len = colon != NULL ? (size_t)(colon - p) : strlen(p);
        if (len == n && memcmp(p, dir, n) == 0)
            return 1;
        if (colon == NULL)
            break;
        p = colon + 1;
    }
    return 0;
}

static int bionicx_library_path_append(char *library_path, int *n, size_t cap,
                                       const char *dir)
{
    size_t len;

    if (library_path == NULL || n == NULL || dir == NULL || dir[0] == '\0')
        return 0;
    if (bionicx_library_path_has_dir(library_path, dir))
        return 0;
    len = strlen(dir);
    if (*n < 0 || (size_t)*n + (*n > 0 ? 1u : 0u) + len >= cap)
        return -1;
    if (*n > 0)
        library_path[(*n)++] = ':';
    memcpy(library_path + *n, dir, len + 1);
    *n += (int)len;
    return 0;
}

/* Keep the Android libc overlay available. Debian dependencies and private
 * directories are resolved by the loader, not exported globally here. */
static int bionicx_library_path_init(const char *root, const char *extra,
                                     char *path, int *n, size_t cap)
{
    static const char *const dirs[] = {
        "/usr/lib/arlinux-platform"
    };
    *n = snprintf(path, cap, "%s", extra ? extra : "");
    if (*n < 0 || (size_t)*n >= cap) return -1;
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        char dir[PATH_MAX];
        if (snprintf(dir, sizeof(dir), "%s%s", root, dirs[i]) >= (int)sizeof(dir)
            || bionicx_library_path_append(path, n, cap, dir) != 0)
            return -1;
    }
    return 0;
}

/* Prepend a platform overlay directory. */
static int bionicx_library_path_prepend(char *library_path, int *n, size_t cap,
                                        const char *prefix)
{
    size_t len;

    if (library_path == NULL || n == NULL || prefix == NULL ||
            prefix[0] == '\0' || *n < 0)
        return 0;
    len = strlen(prefix);
    if (*n == 0) {
        if (len >= cap)
            return -1;
        memcpy(library_path, prefix, len + 1);
        *n = (int)len;
        return 0;
    }
    if ((size_t)*n + 1 + len >= cap)
        return -1;
    memmove(library_path + len + 1, library_path, (size_t)*n + 1);
    memcpy(library_path, prefix, len);
    library_path[len] = ':';
    *n += (int)len + 1;
    return 0;
}

/* Both GPU backends use the installed Mesa frontend. */
static void bionicx_library_path_put_mesa(const char *root, char *library_path,
                                          int *n, size_t cap)
{
    char mesa[PATH_MAX];

    if (root == NULL || root[0] != '/' || library_path == NULL ||
            n == NULL || *n < 0)
        return;
    if (snprintf(mesa, sizeof(mesa), "%s/usr/lib/mesa", root) >= PATH_MAX)
        return;
    if (bionicx_library_path_has_dir(library_path, mesa))
        return;
    (void)bionicx_library_path_prepend(library_path, n, cap, mesa);
}

#endif
