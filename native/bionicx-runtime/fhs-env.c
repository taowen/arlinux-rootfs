#include "runtime-internal.h"

#include <dlfcn.h>
#include <product-policy.h>

static char *libc_getenv(const char *name);
static int libc_unsetenv(const char *name);

static int path_has_dir(const char *path, const char *dir) {
    size_t n;
    const char *p;

    if (path == NULL || dir == NULL) return 0;
    n = strlen(dir);
    p = path;
    while (*p != '\0') {
        if (strncmp(p, dir, n) == 0 && (p[n] == ':' || p[n] == '\0'))
            return 1;
        p = strchr(p, ':');
        if (p == NULL) break;
        p++;
    }
    return 0;
}

void bionicx_ensure_rootfs_path(void) {
    const char *root = bionicx_getenv("BIONICX_ROOTFS");
    const char *current = bionicx_getenv("PATH");
    char sbin[PATH_MAX];
    char value[PATH_MAX * 2];
    int count;

    if (root == NULL || root[0] != '/')
        return;
    if (snprintf(sbin, sizeof(sbin), "%s/usr/sbin", root) >= (int)sizeof(sbin))
        return;
    /* xterm PATH starts with $root/usr/bin, so a prefix check would skip
     * sbin. dpkg then errors: ldconfig / start-stop-daemon not in PATH. */
    if (path_has_dir(current, sbin))
        return;
    count = snprintf(value, sizeof(value),
                     "%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin:%s",
                     root, root, root, root,
                     current != NULL ? current : "/system/bin");
    if (count > 0 && count < (int)sizeof(value)) setenv("PATH", value, 1);
}

int bionicx_package_transaction(void) {
    const char *value = bionicx_getenv("BIONICX_VIRTUAL_ROOT");
    if (!value) value = bionicx_captured_value("BIONICX_VIRTUAL_ROOT");
    if (value != NULL && value[0] != '\0' && strcmp(value, "0") != 0)
        return 1;
    return 0;
}

static void apply_vulkan_layer(const char *root)
{
    const char *name = "VK_LAYER_HYBRIS_compat";
    const char *layers = bionicx_getenv("VK_INSTANCE_LAYERS");
    const char *paths = bionicx_getenv("VK_LAYER_PATH");
    char directory[PATH_MAX], manifest[PATH_MAX], value[PATH_MAX * 2];
    char enabled[PATH_MAX * 2];
    if (path_has_dir(layers, name)) return;
    if (snprintf(directory, sizeof(directory), "%s/usr/lib/arlinux/vulkan", root) >= (int)sizeof(directory) ||
        snprintf(manifest, sizeof(manifest), "%s/VkLayer_hybris_compat.json", directory) >= (int)sizeof(manifest) ||
        access(manifest, R_OK) != 0) return;
    if (snprintf(value, sizeof(value), "%s%s%s", directory,
                 paths && *paths ? ":" : "", paths ? paths : "") >= (int)sizeof(value) ||
        snprintf(enabled, sizeof(enabled), "%s%s%s", name,
                 layers && *layers ? ":" : "", layers ? layers : "") >= (int)sizeof(enabled)) return;
    setenv("VK_LAYER_PATH", value, 1);
    setenv("VK_INSTANCE_LAYERS", enabled, 1);
}

/* One GL implementation and one selected Vulkan ICD. The APK installs the
 * device default as arlinux_icd.json; explicit driver files remain authoritative. */
static void apply_gpu_env(void)
{
    const char *root = bionicx_getenv("BIONICX_ROOTFS");
    const char *gpu = bionicx_getenv("BIONICX_GPU");
    const char *icd = "arlinux_icd.json";
    const char *selected = bionicx_getenv("VK_DRIVER_FILES");
    const char *derived = bionicx_getenv("BIONICX_DEFAULT_ICD");
    char path[PATH_MAX];
    if (!root || root[0] != '/') return;
    apply_vulkan_layer(root);
    if (gpu && (!strcmp(gpu, "turnip") || !strcmp(gpu, "mesa")))
        icd = "freedreno_icd.json";
    else if (gpu && !strcmp(gpu, "hybris"))
        icd = "hybris_icd.json";
    if (snprintf(path, sizeof(path), "%s/usr/lib/mesa/dri", root) < (int)sizeof(path))
        setenv("LIBGL_DRIVERS_PATH", path, 0);
    setenv("GALLIUM_DRIVER", "zink", 0);
    setenv("MESA_LOADER_DRIVER_OVERRIDE", "zink", 0);
    setenv("LIBGL_KOPPER_DRI2", "true", 0);
    setenv("LIBGL_KOPPER_DISABLE", "false", 0);
    if ((!selected || (derived && !strcmp(selected, derived))) &&
        !bionicx_getenv("VK_ICD_FILENAMES") &&
        snprintf(path, sizeof(path), "%s/usr/share/vulkan/icd.d/%s", root, icd) < (int)sizeof(path)) {
        setenv("VK_DRIVER_FILES", path, 1);
        setenv("BIONICX_DEFAULT_ICD", path, 1);
    }
    setenv("ANDROID_ROOT", "/system", 0);
    setenv("ANDROID_DATA", "/data", 0);
    /* Let libhybris use Android's platform/HAL namespaces. A flat automatic
     * search path overrides their dependency boundaries. Explicit caller
     * HYBRIS_LD_LIBRARY_PATH overrides remain untouched. */
}

int bionicx_path_is_proc_exe(const char *path) {
    char *end = NULL;
    long pid;
    if (path == NULL)
        return 0;
    if (strcmp(path, "/proc/self/exe") == 0 ||
            strcmp(path, "/proc/thread-self/exe") == 0)
        return 1;
    if (strncmp(path, "/proc/", 6) != 0)
        return 0;
    if (path[6] < '1' || path[6] > '9')
        return 0;
    pid = strtol(path + 6, &end, 10);
    (void)pid;
    return end != path + 6 && strcmp(end, "/exe") == 0;
}

static int is_usable_guest_execfn(const char *path) {
    return path != NULL && path[0] == '/' && !bionicx_path_is_proc_exe(path) &&
            !bionicx_path_is_overlay_loader(path);
}

const char *bionicx_guest_execfn(void) {
    const char *exe = bionicx_captured_value("BIONICX_EXECFN");
    if (!is_usable_guest_execfn(exe))
        exe = getenv("BIONICX_EXECFN");
    if (is_usable_guest_execfn(exe))
        return exe;
    return NULL;
}

/* Contract vs session: refill contract if dpkg/zygote dropped it. Session
 * variables stay omitted when the child envp already names BIONICX_ROOTFS. */
static const char *const runtime_environment_names[] = {
    "LD_PRELOAD",
    "BIONICX_ROOTFS",
    "BIONICX_FILES",
    "BIONICX_TMPDIR",
    "BIONICX_DNS_SERVERS",
    "BIONICX_VIRTUAL_ROOT",
    ARLINUX_PRODUCT_ENVIRONMENT
    "BIONICX_REWRITE_ABSOLUTE_SYMLINKS",
    "BIONICX_EXECFN",
    "BIONICX_LOG_EXEC",
    "SSL_CERT_FILE",
    "SSL_CERT_DIR",
    "NODE_EXTRA_CA_CERTS",
    "SHELL",
    "HOME",
    "PATH",
    "LANG",
    "TERM",
    "DISPLAY",
    "DBUS_SESSION_BUS_ADDRESS",
    "PULSE_SERVER",
    "XDG_RUNTIME_DIR",
    "CHROME_EXTRA_FLAGS",
    /* GPU switch: envp is source of truth when it still has BIONICX_ROOTFS.
     * Captured values refill only sanitized envp (zygote). Derived hybris
     * vars (HYBRIS_LD_LIBRARY_PATH, ANDROID_ROOT, ANDROID_DATA) are filled
     * by apply_gpu_env in the child, not restored here. */
    "BIONICX_GPU",
    "VK_ICD_FILENAMES",
    "VK_DRIVER_FILES",
    "VK_LAYER_PATH",
    "VK_INSTANCE_LAYERS",
    "BIONICX_DEFAULT_ICD",
    "LIBGL_DRIVERS_PATH",
    "MESA_LOADER_DRIVER_OVERRIDE",
    "GALLIUM_DRIVER",
    "LIBGL_KOPPER_DRI2",
    "LIBGL_KOPPER_DISABLE",
    "HYBRIS_ANDROID_SDK_VERSION",
};

static int is_session_environment_name(const char *name)
{
    return strcmp(name, "DISPLAY") == 0 ||
            strcmp(name, "WAYLAND_DISPLAY") == 0 ||
            strcmp(name, "DBUS_SESSION_BUS_ADDRESS") == 0 ||
            strcmp(name, "PULSE_SERVER") == 0 ||
            strcmp(name, "XDG_RUNTIME_DIR") == 0 ||
            strcmp(name, "CHROME_EXTRA_FLAGS") == 0;
}

static int is_gpu_switch_name(const char *name)
{
    static const char *const names[] = {
        "BIONICX_GPU",
        "VK_ICD_FILENAMES",
        "VK_DRIVER_FILES",
        "VK_LAYER_PATH",
        "VK_INSTANCE_LAYERS",
        "BIONICX_DEFAULT_ICD",
        "LIBGL_DRIVERS_PATH",
        "MESA_LOADER_DRIVER_OVERRIDE",
        "GALLIUM_DRIVER",
        "LIBGL_KOPPER_DRI2",
        "LIBGL_KOPPER_DISABLE",
        "HYBRIS_ANDROID_SDK_VERSION",
    };
    size_t i;

    if (name == NULL)
        return 0;
    for (i = 0; i < sizeof(names) / sizeof(names[0]); ++i) {
        if (strcmp(name, names[i]) == 0)
            return 1;
    }
    return 0;
}

static char captured_runtime_environment
        [sizeof(runtime_environment_names) /
         sizeof(runtime_environment_names[0])][PATH_MAX];

/* Static child binaries cannot use our FHS open interposition. Give TLS
 * libraries the real Debian trust-store paths before spawning those children. */
static void apply_certificate_env(void)
{
    const char *root = bionicx_getenv("BIONICX_ROOTFS");
    char path[PATH_MAX];
    if (!root || root[0] != '/') return;
    if (snprintf(path, sizeof(path), "%s/etc/ssl/certs/ca-certificates.crt", root) < (int)sizeof(path)
            && access(path, R_OK) == 0)
        setenv("SSL_CERT_FILE", path, 0);
    if (snprintf(path, sizeof(path), "%s/etc/ssl/certs", root) < (int)sizeof(path)
            && access(path, R_OK | X_OK) == 0)
        setenv("SSL_CERT_DIR", path, 0);
}

__attribute__((constructor(101)))
static void capture_runtime_environment(void) {
    bionicx_ensure_rootfs_path();
    arlinux_product_environment();
    apply_gpu_env();
    apply_certificate_env();
    for (size_t i = 0; i < sizeof(runtime_environment_names) /
            sizeof(runtime_environment_names[0]); ++i) {
        const char *value = bionicx_getenv(runtime_environment_names[i]);
        if (value == NULL) {
            captured_runtime_environment[i][0] = '\0';
            continue;
        }
        snprintf(captured_runtime_environment[i],
                 sizeof(captured_runtime_environment[i]), "%s", value);
    }
    /* EXECFN is loader bookkeeping, not application configuration.  Keeping
     * it in environ lets shell probes (`env -0`) copy a transient helper's
     * identity back into their parent.  Electron then respawns that helper
     * instead of /proc/self/exe for utility processes.  Children still get
     * the value explicitly from bionicx_with_runtime_environment(). */
    libc_unsetenv("BIONICX_EXECFN");
}

const char *bionicx_captured_value(const char *name) {
    for (size_t i = 0; i < sizeof(runtime_environment_names) /
            sizeof(runtime_environment_names[0]); ++i) {
        if (strcmp(runtime_environment_names[i], name) != 0) continue;
        return captured_runtime_environment[i][0] != '\0'
                ? captured_runtime_environment[i] : NULL;
    }
    return NULL;
}

static void update_captured_execfn(const char *path) {
    for (size_t i = 0; i < sizeof(runtime_environment_names) /
            sizeof(runtime_environment_names[0]); ++i) {
        if (strcmp(runtime_environment_names[i], "BIONICX_EXECFN") != 0)
            continue;
        if (path != NULL && path[0] == '/')
            snprintf(captured_runtime_environment[i],
                     sizeof(captured_runtime_environment[i]), "%s", path);
        else
            captured_runtime_environment[i][0] = '\0';
        return;
    }
}

int bionicx_save_execfn(char saved[PATH_MAX]) {
    const char *old = libc_getenv("BIONICX_EXECFN");
    if (old == NULL || old[0] == '\0')
        old = bionicx_captured_value("BIONICX_EXECFN");
    if (old != NULL && old[0] != '\0') {
        snprintf(saved, PATH_MAX, "%s", old);
        return 1;
    }
    saved[0] = '\0';
    return 0;
}

void bionicx_assign_execfn(const char *path) {
    if (!is_usable_guest_execfn(path))
        return;
    update_captured_execfn(path);
}

void bionicx_restore_execfn(int had, const char saved[PATH_MAX]) {
    if (had) {
        update_captured_execfn(saved);
    } else {
        update_captured_execfn(NULL);
    }
}

static char *libc_getenv(const char *name) {
    static char *(*next)(const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "getenv");
    return next != NULL ? next(name) : NULL;
}

static int libc_unsetenv(const char *name) {
    static int (*next)(const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "unsetenv");
    return next != NULL ? next(name) : -1;
}

void bionicx_restore_runtime_environment(void) {
    bionicx_ns_export();
    for (size_t i = 0; i < sizeof(runtime_environment_names) /
            sizeof(runtime_environment_names[0]); ++i) {
        const char *live;
        if (captured_runtime_environment[i][0] == '\0') continue;
        if (is_session_environment_name(runtime_environment_names[i]) ||
                is_gpu_switch_name(runtime_environment_names[i]))
            continue;
        live = libc_getenv(runtime_environment_names[i]);
        if (live != NULL && live[0] != '\0')
            continue;
        setenv(runtime_environment_names[i],
               captured_runtime_environment[i], 1);
    }
    if (bionicx_package_transaction()) {
        if (libc_getenv("BIONICX_VIRTUAL_ROOT") == NULL
                || libc_getenv("BIONICX_VIRTUAL_ROOT")[0] == '\0')
            setenv("BIONICX_VIRTUAL_ROOT", "1", 1);
        if (libc_getenv("TERM") == NULL || libc_getenv("TERM")[0] == '\0')
            setenv("TERM", "dumb", 1);
    }
}

int clearenv(void) {
    static int (*next)(void);
    if (next == NULL) next = dlsym(RTLD_NEXT, "clearenv");
    int result = next != NULL ? next() : 0;
    /* Chromium's zygote clearenv()s before specializing Node utilities.
     * Node copies environ into process.env, so libc getenv hooks are not
     * enough for VS Code's pty host to see SHELL/HOME/PATH. */
    bionicx_restore_runtime_environment();
    return result;
}

int unsetenv(const char *name) {
    static int (*next)(const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "unsetenv");
    /* Zygote must not drop SHELL/HOME/PATH. GPU switch names are unsettable
     * so nested `export BIONICX_GPU=…` / explicit ICD selection work. */
    if (!is_gpu_switch_name(name) && bionicx_captured_value(name) != NULL)
        return 0;
    return next != NULL ? next(name) : -1;
}

const char *bionicx_captured_rootfs(void) {
    return bionicx_captured_value("BIONICX_ROOTFS");
}

const char *bionicx_captured_tmpdir(void) {
    return bionicx_captured_value("BIONICX_TMPDIR");
}

char *getenv(const char *name) {
    static char *(*next)(const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "getenv");
    char *live = next(name);
    if (live != NULL && live[0] != '\0') return live;
    return (char *)bionicx_captured_value(name);
}

char *secure_getenv(const char *name) {
    static char *(*next)(const char *);
    if (next == NULL) next = dlsym(RTLD_NEXT, "secure_getenv");
    char *live = next != NULL ? next(name) : NULL;
    if (live != NULL && live[0] != '\0') return live;
    return (char *)bionicx_captured_value(name);
}

__attribute__((constructor(102)))
static void log_runtime_loaded(void) {
    if (bionicx_captured_value("BIONICX_LOG_EXEC") == NULL) return;
    fputs("bionicx-runtime: exec log enabled\n", stderr);
    fflush(stderr);
}

__attribute__((constructor(103)))
static void trace_bash_startup(void) {
    if (bionicx_captured_value("BIONICX_LOG_EXEC") == NULL) return;
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = '\0';
    if (strstr(exe, "bash") == NULL) return;
    const char *tmp = bionicx_captured_value("BIONICX_TMPDIR");
    if (tmp == NULL || tmp[0] != '/') tmp = bionicx_getenv("BIONICX_TMPDIR");
    const char *home = bionicx_getenv("HOME");
    char path[PATH_MAX];
    if (tmp != NULL && tmp[0] == '/' &&
            snprintf(path, sizeof(path), "%s/bash-ctor.log", tmp) < PATH_MAX) {
        /* Prefer the app cache so run-as can cat it. */
    } else if (home != NULL && home[0] == '/' &&
            snprintf(path, sizeof(path), "%s/bash-ctor.log", home) < PATH_MAX) {
    } else {
        return;
    }
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char cmdline[256];
    int cmdfd = open("/proc/self/cmdline", O_RDONLY | O_CLOEXEC);
    ssize_t clen = cmdfd >= 0 ? read(cmdfd, cmdline, sizeof(cmdline) - 1) : 0;
    if (cmdfd >= 0) close(cmdfd);
    if (clen < 0) clen = 0;
    for (ssize_t i = 0; i < clen; ++i)
        if (cmdline[i] == '\0') cmdline[i] = ' ';
    cmdline[clen] = '\0';
    char rcpath[PATH_MAX];
    int rc_ok = 0, rc_err = 0;
    if (home != NULL &&
            snprintf(rcpath, sizeof(rcpath), "%s/.bashrc", home) < PATH_MAX) {
        int rcfd = open(rcpath, O_RDONLY | O_CLOEXEC);
        if (rcfd >= 0) {
            rc_ok = 1;
            close(rcfd);
        } else {
            rc_err = errno;
        }
    }
    char line[512];
    int w = snprintf(line, sizeof(line),
                     "exe=%s cmdline=%s isatty0=%d HOME=%s bashrc=%d errno=%d\n",
                     exe, cmdline, isatty(STDIN_FILENO),
                     home != NULL ? home : "", rc_ok, rc_err);
    if (w > 0) (void)write(fd, line, (size_t)w);
    close(fd);
}

static int environment_has_name(const char *entry, const char *name) {
    size_t length = strlen(name);
    return strncmp(entry, name, length) == 0 && entry[length] == '=';
}

static int is_runtime_environment_entry(const char *entry) {
    for (size_t i = 0; i < sizeof(runtime_environment_names) /
            sizeof(runtime_environment_names[0]); ++i) {
        if (environment_has_name(entry, runtime_environment_names[i]))
            return 1;
    }
    return 0;
}

static const char *env_array_value(char *const environment[], const char *name)
{
    size_t i;
    size_t n;

    if (environment == NULL || name == NULL)
        return NULL;
    n = strlen(name);
    for (i = 0; environment[i] != NULL; ++i) {
        if (strncmp(environment[i], name, n) == 0 &&
                environment[i][n] == '=') {
            const char *v = environment[i] + n + 1;

            return v[0] != '\0' ? v : NULL;
        }
    }
    return NULL;
}

const char *bionicx_env_lookup(char *const environment[], const char *name)
{
    const char *v;
    const char *raw;

    v = env_array_value(environment, name);
    if (v != NULL)
        return v;
    /* envp that still names BIONICX_ROOTFS is complete: omitted session
     * and GPU switch names stay unset instead of inheriting xterm. */
    if (env_array_value(environment, "BIONICX_ROOTFS") != NULL &&
            (is_gpu_switch_name(name) || is_session_environment_name(name)))
        return NULL;
    raw = bionicx_getenv(name);
    if (raw != NULL && raw[0] != '\0')
        return raw;
    return bionicx_captured_value(name);
}

/* dpkg execve()s maintainer helpers with a sanitized environment. The
 * mandatory runtime contract must still travel with every Debian process. */
static int is_wps_office_bin(const char *path)
{
    const char *base;

    if (path == NULL || path[0] == '\0')
        return 0;
    base = strrchr(path, '/');
    base = base != NULL ? base + 1 : path;
    return strcmp(base, "wps") == 0 || strcmp(base, "wpp") == 0 ||
           strcmp(base, "et") == 0 || strcmp(base, "wpspdf") == 0;
}

static int find_kso_a11y(char out[PATH_MAX])
{
    const char *files = libc_getenv("BIONICX_FILES");

    if (files != NULL && files[0] != '\0' &&
            snprintf(out, PATH_MAX, "%s/bin/libkso-a11y.so", files) < PATH_MAX &&
            access(out, R_OK) == 0)
        return 1;
    return 0;
}

/* WPS re-execs and drops the shell environment. Only those four office
 * binaries get libkso-a11y.so prepended onto the child's LD_PRELOAD. */
static const char *ld_preload_for_exec(const char *execfn)
{
    static char buf[PATH_MAX * 2];
    char so[PATH_MAX];
    const char *live = getenv("LD_PRELOAD");
    const char *cap = bionicx_captured_value("LD_PRELOAD");
    const char *rest;
    size_t n, r;

    if (live != NULL && live[0] == '\0')
        live = NULL;
    if (!is_wps_office_bin(execfn) || !find_kso_a11y(so))
        return live != NULL ? live : cap;
    rest = live != NULL ? live : (cap != NULL ? cap : "");
    n = strlen(so);
    r = strlen(rest);
    if (n >= sizeof(buf))
        return live != NULL ? live : cap;
    memcpy(buf, so, n + 1);
    if (r > 0 && n + 1 + r < sizeof(buf)) {
        buf[n] = ':';
        memcpy(buf + n + 1, rest, r + 1);
    }
    return buf;
}

static int append_env(char **merged, size_t *out, const char *name,
                      const char *value)
{
    size_t needed;
    if (name == NULL || value == NULL)
        return 0;
    needed = strlen(name) + 1 + strlen(value) + 1;
    merged[*out] = malloc(needed);
    if (merged[*out] == NULL)
        return -1;
    memcpy(merged[*out], name, strlen(name));
    merged[*out][strlen(name)] = '=';
    memcpy(merged[*out] + strlen(name) + 1, value, strlen(value) + 1);
    ++*out;
    return 0;
}

char **bionicx_with_runtime_environment(char *const environment[],
                                       size_t *owned_from) {
    *owned_from = 0;
    if (environment == NULL) return NULL;

    size_t original = 0;
    while (environment[original] != NULL) ++original;

    const char *execfn = bionicx_guest_execfn();
    int wps = is_wps_office_bin(execfn);
    size_t extra = 0;
    const char *values[sizeof(runtime_environment_names) /
                       sizeof(runtime_environment_names[0])];
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (strcmp(runtime_environment_names[i], "BIONICX_EXECFN") == 0)
            /* prepare_guest_exec assigned the child's identity. A shell's
             * explicit envp can still contain the parent's old value. */
            values[i] = execfn;
        else if (strcmp(runtime_environment_names[i], "LD_PRELOAD") == 0)
            values[i] = ld_preload_for_exec(execfn);
        else
            values[i] = bionicx_env_lookup(environment,
                                           runtime_environment_names[i]);
        if (values[i] != NULL) ++extra;
    }

    char **merged = calloc(original + extra + 5, sizeof(*merged));
    if (merged == NULL) return NULL;
    size_t out = 0;
    for (size_t i = 0; i < original; ++i) {
        if (!environment_has_name(environment[i], "BIONICX_NS_STATE") &&
                !is_runtime_environment_entry(environment[i]) &&
                !(wps && environment_has_name(environment[i], "LD_LIBRARY_PATH")))
            merged[out++] = environment[i];
    }
    *owned_from = out;
    for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); ++i) {
        if (values[i] == NULL) continue;
        size_t needed = strlen(runtime_environment_names[i]) + 1 +
                        strlen(values[i]) + 1;
        merged[out] = malloc(needed);
        if (merged[out] == NULL) {
            while (out > *owned_from) free(merged[--out]);
            free(merged);
            return NULL;
        }
        memcpy(merged[out], runtime_environment_names[i],
               strlen(runtime_environment_names[i]));
        merged[out][strlen(runtime_environment_names[i])] = '=';
        memcpy(merged[out] + strlen(runtime_environment_names[i]) + 1,
               values[i], strlen(values[i]) + 1);
        ++out;
    }
    char *ns_state = bionicx_ns_environment();
    if (ns_state != NULL) {
        int result = append_env(merged, &out, "BIONICX_NS_STATE", ns_state);
        free(ns_state);
        if (result != 0) {
            while (out > *owned_from) free(merged[--out]);
            free(merged);
            return NULL;
        }
    }
    if (wps) {
        const char *tmp = getenv("BIONICX_TMPDIR");
        char libdir[PATH_MAX];
        char ldlib[PATH_MAX * 2];
        const char *old_ld;
        const char *slash;

        if (tmp == NULL || tmp[0] == '\0')
            tmp = bionicx_captured_value("BIONICX_TMPDIR");
        if (tmp != NULL && append_env(merged, &out, "ARLINUX_A11Y_DIR", tmp) != 0) {
            while (out > *owned_from) free(merged[--out]);
            free(merged);
            return NULL;
        }
        /* libkso-a11y.so DT_NEEDs Kso Qt, which needs ICU from office6. */
        slash = strrchr(execfn, '/');
        if (slash != NULL && (size_t)(slash - execfn) < sizeof(libdir)) {
            memcpy(libdir, execfn, (size_t)(slash - execfn));
            libdir[slash - execfn] = '\0';
            old_ld = getenv("LD_LIBRARY_PATH");
            if (old_ld == NULL || old_ld[0] == '\0')
                old_ld = bionicx_captured_value("LD_LIBRARY_PATH");
            if (old_ld != NULL && old_ld[0] != '\0')
                snprintf(ldlib, sizeof(ldlib), "%s:%s", libdir, old_ld);
            else
                snprintf(ldlib, sizeof(ldlib), "%s", libdir);
            if (append_env(merged, &out, "LD_LIBRARY_PATH", ldlib) != 0) {
                while (out > *owned_from) free(merged[--out]);
                free(merged);
                return NULL;
            }
        }
    }
    merged[out] = NULL;
    return merged;
}

void bionicx_free_runtime_environment(char **merged, size_t owned_from) {
    if (merged == NULL) return;
    for (size_t i = owned_from; merged[i] != NULL; ++i) free(merged[i]);
    free(merged);
}
