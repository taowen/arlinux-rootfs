#include "runtime-internal.h"
#include "library-path.h"

#include <dlfcn.h>
#include <elf.h>
#include <signal.h>
#include <spawn.h>
#include <sys/ioctl.h>
#include <sys/wait.h>

static int (*real_access_fn(void))(const char *, int) {
    static int (*real_access)(const char *, int);
    if (real_access == NULL) real_access = dlsym(RTLD_NEXT, "access");
    return real_access;
}

static int fd_is_pipe_or_regular_file(int fd) {
    struct stat st;
    if (fstat(fd, &st) != 0)
        return 0;
    return S_ISFIFO(st.st_mode) || S_ISSOCK(st.st_mode) || S_ISREG(st.st_mode);
}

static char **script_arguments(const char *path, char *const arguments[],
                               char program[PATH_MAX],
                               char interpreter_argument[PATH_MAX]) {
    char line[512];
    int descriptor = open(path, O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) return NULL;
    ssize_t length = read(descriptor, line, sizeof(line) - 1);
    close(descriptor);
    if (length < 3 || line[0] != '#' || line[1] != '!') return NULL;
    line[length] = '\0';
    char *cursor = line + 2;
    while (*cursor == ' ' || *cursor == '\t') ++cursor;
    char *interpreter = cursor;
    while (*cursor != '\0' && *cursor != '\n' && *cursor != ' ' &&
            *cursor != '\t') ++cursor;
    if (cursor == interpreter) return NULL;
    char separator = *cursor;
    *cursor = '\0';
    char redirected[PATH_MAX];
    const char *actual = bionicx_redirect_path(interpreter, redirected);
    if (actual == NULL || snprintf(program, PATH_MAX, "%s", actual) >= PATH_MAX)
        return NULL;

    int has_interpreter_argument = 0;
    if (separator != '\0' && separator != '\n') {
        ++cursor;
        while (*cursor == ' ' || *cursor == '\t') ++cursor;
        char *end = cursor;
        while (*end != '\0' && *end != '\n') ++end;
        while (end > cursor && (end[-1] == ' ' || end[-1] == '\t')) --end;
        if (end > cursor) {
            size_t argument_length = (size_t)(end - cursor);
            if (argument_length >= PATH_MAX) return NULL;
            memcpy(interpreter_argument, cursor, argument_length);
            interpreter_argument[argument_length] = '\0';
            has_interpreter_argument = 1;
        }
    }
    size_t argument_count = 0;
    while (arguments[argument_count] != NULL) ++argument_count;
    char **result = calloc(argument_count + 2 + has_interpreter_argument,
                           sizeof(*result));
    if (result == NULL) return NULL;
    result[0] = program;
    size_t out = 1;
    if (has_interpreter_argument) result[out++] = interpreter_argument;
    result[out++] = (char *)path;
    for (size_t i = 1; i < argument_count; ++i) result[out++] = arguments[i];
    return result;
}

static int overlay_loader_path(char loader[PATH_MAX]) {
    const char *root = bionicx_getenv("BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/')
        root = bionicx_captured_value("BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/')
        return 0;
    if (snprintf(loader, PATH_MAX, "%s/usr/lib/arlinux-platform/ld-linux-aarch64.so.1", root)
            >= PATH_MAX)
        return 0;
    int (*real_access)(const char *, int) = real_access_fn();
    return real_access != NULL && real_access(loader, X_OK) == 0;
}

static int same_path_alias(const char *left, const char *right) {
    char alias[PATH_MAX];
    if (left == NULL || right == NULL)
        return 0;
    if (strcmp(left, right) == 0)
        return 1;
    if (strncmp(left, "/data/user/0/", 13) == 0) {
        if (snprintf(alias, sizeof(alias), "/data/data/%s", left + 13)
                >= (int)sizeof(alias))
            return 0;
        return strcmp(alias, right) == 0;
    }
    if (strncmp(left, "/data/data/", 11) == 0) {
        if (snprintf(alias, sizeof(alias), "/data/user/0/%s", left + 11)
                >= (int)sizeof(alias))
            return 0;
        return strcmp(alias, right) == 0;
    }
    return 0;
}

static int read_pt_interp(const char *path, char *out, size_t out_size) {
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    Elf64_Ehdr header;
    if (fd < 0)
        return 0;
    if (pread(fd, &header, sizeof(header), 0) != (ssize_t)sizeof(header) ||
            memcmp(header.e_ident, ELFMAG, SELFMAG) != 0 ||
            header.e_ident[EI_CLASS] != ELFCLASS64 ||
            header.e_phentsize != sizeof(Elf64_Phdr)) {
        close(fd);
        return 0;
    }
    for (Elf64_Half i = 0; i < header.e_phnum; ++i) {
        Elf64_Phdr phdr;
        off_t offset = (off_t)header.e_phoff +
                (off_t)i * (off_t)header.e_phentsize;
        if (pread(fd, &phdr, sizeof(phdr), offset) != (ssize_t)sizeof(phdr))
            break;
        if (phdr.p_type != PT_INTERP)
            continue;
        if (phdr.p_filesz == 0 || phdr.p_filesz >= out_size) {
            close(fd);
            return 0;
        }
        if (pread(fd, out, phdr.p_filesz, (off_t)phdr.p_offset)
                != (ssize_t)phdr.p_filesz) {
            close(fd);
            return 0;
        }
        out[phdr.p_filesz] = '\0';
        close(fd);
        return 1;
    }
    close(fd);
    return 0;
}

static int interp_needs_wrap(const char *interp, const char *loader) {
    if (interp == NULL || interp[0] == '\0' || loader == NULL)
        return 0;
    if (same_path_alias(interp, loader))
        return 0;
    /* Kernel opens PT_INTERP without LD_PRELOAD. App-private absolute
     * interpreters already work; Debian FHS paths do not. */
    if (strncmp(interp, "/data/", 6) == 0)
        return 0;
    return 1;
}

/* Fake exe paths (proc self exe or overlay ld.so) map to BIONICX_EXECFN. */
static const char *guest_elf_for_fake_exe(const char *path) {
    if (path == NULL)
        return NULL;
    if (!bionicx_path_is_proc_exe(path) && !bionicx_path_is_overlay_loader(path))
        return NULL;
    return bionicx_guest_execfn();
}

/* bash execve()s its own envp, not libc environ. --library-path and the
 * child environ must read that array; getenv() here is still the parent. */
static char **loader_arguments(const char *path, char *const arguments[],
                               char loader[PATH_MAX],
                               char *const environment[]) {
    static char library_path[16384];
    char interp[PATH_MAX];
    const char *root;
    const char *argv0;
    const char *guest;
    if (path == NULL || path[0] != '/')
        return NULL;
    /* Chrome passes /proc/self/exe as path and argv0; never hand that to
     * ld.so as the ELF to load. */
    guest = guest_elf_for_fake_exe(path);
    if (guest != NULL)
        path = guest;
    else if (bionicx_path_is_proc_exe(path) || bionicx_path_is_overlay_loader(path))
        return NULL;
    if (!overlay_loader_path(loader))
        return NULL;
    if (same_path_alias(path, loader))
        return NULL;
    if (!read_pt_interp(path, interp, sizeof(interp)))
        return NULL;
    if (!interp_needs_wrap(interp, loader))
        return NULL;
    root = bionicx_env_lookup(environment, "BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/')
        return NULL;
    /* The loader owns private library paths and caller-dependent scope. */
    {
        const char *ld = bionicx_env_lookup(environment, "LD_LIBRARY_PATH");
        int n;
        if (bionicx_library_path_init(root, ld, library_path, &n,
                                     sizeof(library_path)) != 0)
            return NULL;
        bionicx_library_path_put_mesa(root, library_path, &n, sizeof(library_path));
    }
    size_t argument_count = 0;
    if (arguments != NULL)
        while (arguments[argument_count] != NULL) ++argument_count;
    char **result = calloc(argument_count + 6, sizeof(*result));
    if (result == NULL)
        return NULL;
    argv0 = (arguments != NULL && arguments[0] != NULL &&
            arguments[0][0] != '\0') ? arguments[0] : path;
    guest = guest_elf_for_fake_exe(argv0);
    if (guest != NULL)
        argv0 = guest;
    else if (bionicx_path_is_proc_exe(argv0))
        argv0 = path;
    result[0] = loader;
    result[1] = "--library-path";
    result[2] = library_path;
    result[3] = "--argv0";
    result[4] = (char *)argv0;
    result[5] = (char *)path;
    for (size_t i = 1; i < argument_count; ++i)
        result[5 + i] = arguments[i];
    return result;
}

static void log_exec(const char *path, char *const arguments[]);


struct guest_exec {
    const char *run_path;
    char *const *run_args;
    const char *execfn;
    char **script;
    char **wrapped;
    char loader[PATH_MAX];
    char program[PATH_MAX];
    char canonical[PATH_MAX];
    char interp_arg[PATH_MAX];
};

static int prepare_guest_exec(const char *path, char *const arguments[],
                              int reject_unparsed_shebang,
                              struct guest_exec *out,
                              char *const environment[]);
static void free_guest_exec(struct guest_exec *g);

static void keep_standard_fds(void) {
    for (int fd = STDIN_FILENO; fd <= STDERR_FILENO; ++fd) {
        int flags = fcntl(fd, F_GETFD);
        if (flags >= 0 && (flags & FD_CLOEXEC) != 0)
            (void)fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC);
    }
    /* Package transactions must keep dpkg-deb's data-tar pipe. Never steal
     * stdout onto the xterm pty, and do not claim a controlling tty. */
    if (bionicx_package_transaction())
        return;
    /* node-pty can leave stdin on the slave with stdout/stderr on /dev/null.
     * Interactive bash then writes the prompt off the pty. Do not steal a
     * pipe or regular file. */
    if (isatty(STDIN_FILENO)) {
        if (!isatty(STDOUT_FILENO) && !fd_is_pipe_or_regular_file(STDOUT_FILENO))
            (void)dup2(STDIN_FILENO, STDOUT_FILENO);
        if (!isatty(STDERR_FILENO) && !fd_is_pipe_or_regular_file(STDERR_FILENO))
            (void)dup2(STDIN_FILENO, STDERR_FILENO);
        /* node-pty resets signals after forkpty() returns in the child and
         * can drop the controlling tty. bash -i then skips rc and exits 0
         * on EOF. Re-acquire /dev/tty before exec. */
        int devtty = open("/dev/tty", O_RDWR | O_NOCTTY);
        if (devtty >= 0) {
            close(devtty);
        } else {
            if (getsid(0) != bionicx_host_pid())
                (void)setsid();
            (void)ioctl(STDIN_FILENO, TIOCSCTTY, 1);
        }
    }
}

static int exec_script(const char *path, char *const arguments[],
                       int (*execute)(const char *, char *const[],
                                      char *const[])) {
    struct guest_exec guest;
    char saved_fn[PATH_MAX];
    int had_fn;
    int result;
    int saved_errno;
    size_t owned_from = 0;
    char **merged;

    if (prepare_guest_exec(path, arguments, 1, &guest, NULL) != 0)
        return -1;
    had_fn = bionicx_save_execfn(saved_fn);
    bionicx_restore_runtime_environment();
    bionicx_assign_execfn(guest.execfn);
    /* libc's execv/execvp call hidden execve implementations. Pass the
     * child's environment explicitly instead of inheriting the parent's
     * executable identity through those un-interposed calls. */
    merged = bionicx_with_runtime_environment(environ, &owned_from);
    if (merged == NULL) {
        bionicx_restore_execfn(had_fn, saved_fn);
        free_guest_exec(&guest);
        return -1;
    }
    log_exec(guest.run_path, (char *const *)guest.run_args);
    keep_standard_fds();
    result = execute(guest.run_path, (char *const *)guest.run_args, merged);
    saved_errno = errno;
    bionicx_restore_execfn(had_fn, saved_fn);
    free_guest_exec(&guest);
    bionicx_free_runtime_environment(merged, owned_from);
    errno = saved_errno;
    return result;
}

static void free_guest_exec(struct guest_exec *g) {
    if (g == NULL)
        return;
    free(g->script);
    free(g->wrapped);
    g->script = NULL;
    g->wrapped = NULL;
}

static int prepare_guest_exec(const char *path, char *const arguments[],
                              int reject_unparsed_shebang,
                              struct guest_exec *out,
                              char *const environment[]) {
    memset(out, 0, sizeof(*out));
    out->run_path = path;
    out->run_args = arguments;
    out->execfn = path;
    out->script = script_arguments(path, arguments, out->program,
                                   out->interp_arg);
    if (out->script != NULL) {
        const char *interpreter = realpath(out->program, out->canonical)
            ? out->canonical : out->program;
        out->run_path = interpreter;
        out->run_args = out->script;
        out->execfn = interpreter;
        out->wrapped = loader_arguments(interpreter,
                                        (char *const *)out->script,
                                        out->loader, environment);
        if (out->wrapped != NULL) {
            out->run_path = out->loader;
            out->run_args = out->wrapped;
        }
        return 0;
    }
    /* A missing shebang is a real ELF. A present shebang must not reach
     * the kernel: #!/bin/sh would start Android's Bionic shell. */
    if (reject_unparsed_shebang) {
        int descriptor = open(path, O_RDONLY | O_CLOEXEC);
        if (descriptor >= 0) {
            char magic[2] = {0, 0};
            ssize_t n = read(descriptor, magic, 2);
            close(descriptor);
            if (n == 2 && magic[0] == '#' && magic[1] == '!') {
                errno = ENOEXEC;
                free_guest_exec(out);
                return -1;
            }
        }
    }
    /* Linux /proc/self/exe identifies the resolved ELF, while argv[0]
     * retains the invocation name. Symlink entry points must not change
     * application resource discovery or the loader's $ORIGIN. */
    if (realpath(path, out->canonical))
        path = out->canonical;
    out->run_path = path;
    out->execfn = path;
    out->wrapped = loader_arguments(path, arguments,
                                    out->loader, environment);
    if (out->wrapped != NULL) {
        out->run_path = out->loader;
        out->run_args = out->wrapped;
        out->execfn = path;
    }
    return 0;
}

static void log_exec(const char *path, char *const arguments[]) {
    const char *enabled = bionicx_captured_value("BIONICX_LOG_EXEC");
    if (enabled == NULL) enabled = getenv("BIONICX_LOG_EXEC");
    if (enabled == NULL || enabled[0] == '\0') return;
    int log_file = path != NULL && strstr(path, "bash") != NULL;
    char line[512];
    size_t used = 0;
    used = (size_t)snprintf(line, sizeof(line), "bionicx-execve:");
    if (path != NULL && used < sizeof(line))
        used += (size_t)snprintf(line + used, sizeof(line) - used, " %s", path);
    if (log_file && used < sizeof(line)) {
        int cloexec = fcntl(STDIN_FILENO, F_GETFD);
        int nenv = 0, has_ld = 0;
        const char *live_home = "";
        if (environ != NULL) {
            for (char **entry = environ; *entry != NULL; ++entry) {
                ++nenv;
                if (strncmp(*entry, "HOME=", 5) == 0) live_home = *entry + 5;
                if (strncmp(*entry, "LD_PRELOAD=", 11) == 0) has_ld = 1;
            }
        }
        used += (size_t)snprintf(line + used, sizeof(line) - used,
                                 " isatty0=%d isatty1=%d isatty2=%d cloexec0=%d nenv=%d ld=%d HOME=%s",
                                 isatty(STDIN_FILENO), isatty(STDOUT_FILENO),
                                 isatty(STDERR_FILENO),
                                 cloexec >= 0 && (cloexec & FD_CLOEXEC) ? 1 : 0,
                                 nenv, has_ld, live_home);
    }
    if (arguments != NULL) {
        for (size_t i = 0; arguments[i] != NULL && i < 8 && used < sizeof(line);
                ++i)
            used += (size_t)snprintf(line + used, sizeof(line) - used, " %s",
                                     arguments[i]);
    }
    if (used >= sizeof(line)) used = sizeof(line) - 1;
    line[used++] = '\n';
    fwrite(line, 1, used, stderr);
    fflush(stderr);
    {
        const char *guest = bionicx_guest_execfn();
        fprintf(stderr, "bionicx-execfn: %s\n",
                guest != NULL ? guest : "?");
        fflush(stderr);
    }
    if (!log_file) return;
    const char *tmp = bionicx_captured_tmpdir();
    if (tmp == NULL || tmp[0] != '/') tmp = bionicx_getenv("BIONICX_TMPDIR");
    if (tmp == NULL || tmp[0] != '/') return;
    char log_path[PATH_MAX];
    if (snprintf(log_path, sizeof(log_path), "%s/exec.log", tmp) >= PATH_MAX)
        return;
    int fd = open(log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    (void)write(fd, line, used);
    close(fd);
}

int execv(const char *path, char *const arguments[]) {
    static int (*next)(const char *, char *const[], char *const[]);
    if (next == NULL) next = dlsym(RTLD_NEXT, "execve");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    bionicx_restore_runtime_environment();
    bionicx_ensure_rootfs_path();
    return exec_script(actual, arguments, next);
}

/* Resolve guest ELF for execve/posix_spawn. Map /proc/self/exe to EXECFN. */
static const char *resolve_guest_exec_path(const char *path,
                                           char buffer[PATH_MAX]) {
    const char *guest;
    if (path == NULL) {
        errno = EFAULT;
        return NULL;
    }
    guest = guest_elf_for_fake_exe(path);
    if (guest != NULL)
        return guest;
    if (bionicx_path_is_proc_exe(path) || bionicx_path_is_overlay_loader(path)) {
        errno = ENOENT;
        return NULL;
    }
    return bionicx_redirect_path(path, buffer);
}

static int read_proc_fd_path(int fd, char out[PATH_MAX]) {
    char link[64];
    ssize_t n;
    if (snprintf(link, sizeof(link), "/proc/self/fd/%d", fd) >=
            (int)sizeof(link)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    n = readlink(link, out, PATH_MAX - 1);
    if (n < 0)
        return -1;
    out[n] = '\0';
    {
        char *deleted = strstr(out, " (deleted)");
        if (deleted != NULL)
            *deleted = '\0';
    }
    return 0;
}

static const char *resolve_execveat_path(int dirfd, const char *path, int flags,
                                         char buffer[PATH_MAX]) {
    char fd_target[PATH_MAX];
    const char *guest;
    const char *base;

#ifdef AT_EMPTY_PATH
    if ((flags & AT_EMPTY_PATH) != 0 && path[0] == '\0') {
        if (read_proc_fd_path(dirfd, fd_target) != 0)
            return NULL;
        guest = guest_elf_for_fake_exe(fd_target);
        if (guest != NULL)
            return guest;
        if (bionicx_path_is_proc_exe(fd_target) ||
                bionicx_path_is_overlay_loader(fd_target)) {
            errno = ENOENT;
            return NULL;
        }
        if (snprintf(buffer, PATH_MAX, "%s", fd_target) >= PATH_MAX) {
            errno = ENAMETOOLONG;
            return NULL;
        }
        return buffer;
    }
#endif
    if (path[0] == '/')
        return resolve_guest_exec_path(path, buffer);
    if (dirfd == AT_FDCWD)
        return resolve_guest_exec_path(path, buffer);
    if (read_proc_fd_path(dirfd, fd_target) != 0)
        return NULL;
    guest = guest_elf_for_fake_exe(fd_target);
    base = guest != NULL ? guest : fd_target;
    if (snprintf(buffer, PATH_MAX, "%s/%s", base, path) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return NULL;
    }
    {
        char redirect[PATH_MAX];
        const char *resolved = resolve_guest_exec_path(buffer, redirect);
        if (resolved == NULL)
            return NULL;
        if (resolved != buffer) {
            if (snprintf(buffer, PATH_MAX, "%s", resolved) >= PATH_MAX) {
                errno = ENAMETOOLONG;
                return NULL;
            }
        }
        return buffer;
    }
}

static const char *resolve_rootfs_command(const char *name,
                                         char buffer[PATH_MAX]) {
    const char *root;
    static const char *const directories[] = {
        "/usr/sbin", "/usr/bin", "/sbin", "/bin"
    };
    if (name == NULL || strchr(name, '/') != NULL)
        return NULL;
    /* Respect product/user PATH overlays before distribution fallback paths.
     * Rust/GLib use execvp/spawnp too; resolving /usr/bin first silently skips
     * desktop launch adapters whenever the distribution ships the same name. */
    const char *search = bionicx_getenv("PATH");
    if (search != NULL) {
        const char *entry = search;
        do {
            const char *end = strchr(entry, ':');
            size_t length = end != NULL ? (size_t)(end - entry) : strlen(entry);
            char candidate[PATH_MAX], redirected[PATH_MAX];
            int count = length == 0
                ? snprintf(candidate, sizeof(candidate), "./%s", name)
                : snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)length, entry, name);
            if (count > 0 && count < (int)sizeof(candidate)) {
                const char *actual = bionicx_redirect_path(candidate, redirected);
                if (actual != NULL && access(actual, X_OK) == 0) {
                    snprintf(buffer, PATH_MAX, "%s", actual);
                    return buffer;
                }
            }
            if (end == NULL) break;
            entry = end + 1;
        } while (1);
    }
    root = bionicx_getenv("BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/')
        return NULL;
    for (size_t i = 0; i < sizeof(directories) / sizeof(directories[0]); ++i) {
        int count = snprintf(buffer, PATH_MAX, "%s%s/%s", root,
                             directories[i], name);
        if (count > 0 && count < (int)PATH_MAX && access(buffer, X_OK) == 0)
            return buffer;
    }
    return NULL;
}

int execve(const char *path, char *const arguments[],
           char *const environment[]) {
    static int (*next)(const char *, char *const[], char *const[]);
    struct guest_exec guest;
    char buffer[PATH_MAX];
    char saved_fn[PATH_MAX];
    const char *actual;
    size_t owned_from = 0;
    char **merged;
    char *const *actual_environment;
    int had_fn;
    int result;
    int saved_errno;

    if (next == NULL) next = dlsym(RTLD_NEXT, "execve");
    actual = resolve_guest_exec_path(path, buffer);
    if (actual == NULL) return -1;
    bionicx_restore_runtime_environment();
    bionicx_ensure_rootfs_path();
    if (prepare_guest_exec(actual, arguments, 0, &guest, environment) != 0)
        return -1;
    had_fn = bionicx_save_execfn(saved_fn);
    bionicx_assign_execfn(guest.execfn);
    merged = bionicx_with_runtime_environment(environment, &owned_from);
    if (environment != NULL && merged == NULL) {
        bionicx_restore_execfn(had_fn, saved_fn);
        free_guest_exec(&guest);
        return -1;
    }
    actual_environment = merged != NULL ? merged : environment;
    log_exec(guest.run_path, (char *const *)guest.run_args);
    keep_standard_fds();
    result = next(guest.run_path, (char *const *)guest.run_args,
                  actual_environment);
    saved_errno = errno;
    bionicx_restore_execfn(had_fn, saved_fn);
    free_guest_exec(&guest);
    bionicx_free_runtime_environment(merged, owned_from);
    errno = saved_errno;
    return result;
}

int execveat(int dirfd, const char *path, char *const arguments[],
             char *const environment[], int flags) {
    char buffer[PATH_MAX];
    const char *actual = resolve_execveat_path(dirfd, path, flags, buffer);
    if (actual == NULL)
        return -1;
    return execve(actual, arguments, environment);
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *file_actions,
                const posix_spawnattr_t *attrp,
                char *const arguments[], char *const environment[]) {
    static int (*next)(pid_t *, const char *,
                       const posix_spawn_file_actions_t *,
                       const posix_spawnattr_t *,
                       char *const[], char *const[]);
    struct guest_exec guest;
    char buffer[PATH_MAX];
    char saved_fn[PATH_MAX];
    const char *actual;
    size_t owned_from = 0;
    char **merged;
    char *const *actual_environment;
    int had_fn;
    int result;

    if (next == NULL) next = dlsym(RTLD_NEXT, "posix_spawn");
    actual = resolve_guest_exec_path(path, buffer);
    if (actual == NULL) return errno != 0 ? errno : ENOENT;
    if (prepare_guest_exec(actual, arguments, 0, &guest, environment) != 0)
        return errno != 0 ? errno : ENOEXEC;
    had_fn = bionicx_save_execfn(saved_fn);
    bionicx_assign_execfn(guest.execfn);
    merged = bionicx_with_runtime_environment(environment, &owned_from);
    if (environment != NULL && merged == NULL) {
        bionicx_restore_execfn(had_fn, saved_fn);
        free_guest_exec(&guest);
        return ENOMEM;
    }
    actual_environment = merged != NULL ? merged : environment;
    log_exec(guest.run_path, (char *const *)guest.run_args);
    result = next(pid, guest.run_path, file_actions, attrp,
                  (char *const *)guest.run_args, actual_environment);
    bionicx_restore_execfn(had_fn, saved_fn);
    free_guest_exec(&guest);
    bionicx_free_runtime_environment(merged, owned_from);
    return result;
}

int posix_spawnp(pid_t *pid, const char *path,
                 const posix_spawn_file_actions_t *file_actions,
                 const posix_spawnattr_t *attrp,
                 char *const arguments[], char *const environment[]) {
    char buffer[PATH_MAX];
    const char *actual = path;
    const char *found = resolve_rootfs_command(path, buffer);
    if (found != NULL)
        actual = found;
    return posix_spawn(pid, actual, file_actions, attrp, arguments,
                       environment);
}

int execvp(const char *path, char *const arguments[]) {
    static int (*next)(const char *, char *const[], char *const[]);
    if (next == NULL) next = dlsym(RTLD_NEXT, "execvpe");
    char buffer[PATH_MAX];
    const char *actual = bionicx_redirect_path(path, buffer);
    if (actual == NULL) return -1;
    bionicx_restore_runtime_environment();
    bionicx_ensure_rootfs_path();
    if (actual == path && strchr(path, '/') == NULL) {
        const char *found = resolve_rootfs_command(path, buffer);
        if (found != NULL)
            actual = found;
    }
    return exec_script(actual, arguments, next);
}

static char **vararg_arguments(const char *argument, va_list values,
                               size_t *count_out) {
    va_list count_values;
    va_copy(count_values, values);
    size_t count = 1;
    while (va_arg(count_values, const char *) != NULL) ++count;
    va_end(count_values);
    char **arguments = calloc(count + 1, sizeof(*arguments));
    if (arguments == NULL) return NULL;
    arguments[0] = (char *)argument;
    for (size_t i = 1; i < count; ++i)
        arguments[i] = va_arg(values, char *);
    (void)va_arg(values, char *);
    arguments[count] = NULL;
    *count_out = count;
    return arguments;
}

int execl(const char *path, const char *argument, ...) {
    if (getenv("BIONICX_LOG_EXEC") != NULL)
        (void)write(STDERR_FILENO, "bionicx-execl\n", 14);
    va_list values;
    va_start(values, argument);
    size_t count = 0;
    char **arguments = vararg_arguments(argument, values, &count);
    va_end(values);
    if (arguments == NULL) return -1;
    int result = execv(path, arguments);
    int saved_errno = errno;
    free(arguments);
    errno = saved_errno;
    return result;
}

int execle(const char *path, const char *argument, ...) {
    va_list values;
    va_start(values, argument);
    size_t count = 0;
    char **arguments = vararg_arguments(argument, values, &count);
    char *const *environment = va_arg(values, char *const *);
    va_end(values);
    if (arguments == NULL) return -1;
    int result = execve(path, arguments, (char *const *)environment);
    int saved_errno = errno;
    free(arguments);
    errno = saved_errno;
    return result;
}

int execvpe(const char *path, char *const arguments[],
            char *const environment[]) {
    char buffer[PATH_MAX];
    const char *actual = path;
    const char *found = resolve_rootfs_command(path, buffer);
    if (found != NULL)
        actual = found;
    return execve(actual, arguments, environment);
}

int execlp(const char *path, const char *argument, ...) {
    va_list values;
    va_start(values, argument);
    va_list count_values;
    va_copy(count_values, values);
    size_t count = 1;
    while (va_arg(count_values, const char *) != NULL) ++count;
    va_end(count_values);

    char **arguments = calloc(count + 1, sizeof(*arguments));
    if (arguments == NULL) {
        va_end(values);
        return -1;
    }
    arguments[0] = (char *)argument;
    for (size_t i = 1; i < count; ++i)
        arguments[i] = va_arg(values, char *);
    (void)va_arg(values, char *);
    va_end(values);
    arguments[count] = NULL;
    int result = execvp(path, arguments);
    int saved_errno = errno;
    free(arguments);
    errno = saved_errno;
    return result;
}
