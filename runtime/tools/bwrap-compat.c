/* Bubblewrap command-launch compatibility for Android. NOT a sandbox.
 * No mount, namespace, capability or seccomp isolation is provided.
 * Only identity binds are supported; never mutate the shared rootfs to
 * imitate a private mount. Unsupported setup operations fail explicitly.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

extern char **environ;

static void fail(const char *message, const char *detail)
{
    fprintf(stderr, "arlinux-bwrap: %s: %s\n", message, detail);
    exit(125);
}

static int number(const char *s)
{
    char *end;
    errno = 0;
    long n = strtol(s, &end, 10);
    if (errno || !*s || *end || n < 0 || n > 0x7fffffff)
        fail("invalid descriptor", s);
    return (int)n;
}

static const char *take(int *i, int argc, char **argv)
{
    if (++*i >= argc) fail("missing option argument", argv[*i - 1]);
    return argv[*i];
}

static void identity_bind(const char *src, const char *dst, int optional)
{
    struct stat a, b;
    if (stat(src, &a)) {
        if (optional && errno == ENOENT) return;
        fail("bind source unavailable", src);
    }
    if (stat(dst, &b) || a.st_dev != b.st_dev || a.st_ino != b.st_ino)
        fail("non-identity bind is not supported", dst);
}

/* Expand --args just as ordinary arguments. Bound allocations independently
 * of the caller's fd size and reject truncated, recursive argument files. */
static void expand_args(int *argc, char ***argv, int index)
{
    if (index + 1 >= *argc) fail("missing option argument", "--args");
    int fd = number((*argv)[index + 1]);
    size_t cap = 2 * 1024 * 1024, used = 0;
    char *data = malloc(cap);
    if (!data) fail("out of memory", "--args");
    for (;;) {
        if (used == cap) fail("argument file too large", "--args");
        ssize_t n = read(fd, data + used, cap - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) fail("cannot read argument file", strerror(errno));
        if (!n) break;
        used += (size_t)n;
    }
    close(fd);
    if (used && data[used - 1]) fail("unterminated argument file", "--args");
    size_t count = 0;
    for (size_t j = 0; j < used; j++) if (!data[j]) count++;
    if (count + (size_t)*argc > 65536) fail("too many arguments", "--args");
    char **next = calloc(count + (size_t)*argc, sizeof(char *));
    if (!next) fail("out of memory", "--args");
    int at = 0;
    for (int j = 0; j < index; j++) next[at++] = (*argv)[j];
    for (size_t j = 0; j < used;) {
        if (!strcmp(data + j, "--args")) fail("nested argument file", "--args");
        next[at++] = data + j;
        j += strlen(data + j) + 1;
    }
    for (int j = index + 2; j < *argc; j++) next[at++] = (*argv)[j];
    *argc = at;
    *argv = next;
}

int main(int argc, char **argv)
{
    const char *cwd = NULL, *argv0 = NULL;
    int new_session = 0, die_with_parent = 0, info_fd = -1;
    pid_t parent = getppid();
    int i;
    for (i = 1; i < argc; i++) {
        const char *opt = argv[i];
        if (!strcmp(opt, "--")) { i++; break; }
        if (opt[0] != '-') break;
        if (!strcmp(opt, "--help") || !strcmp(opt, "--version")) {
            puts("ARLinux bwrap compatibility launcher (no sandbox isolation)\n"
                 "Launch options: --args FD, --chdir DIR, --argv0 NAME, --new-session, --die-with-parent\n"
                 "Environment: --clearenv, --setenv NAME VALUE, --unsetenv NAME\n"
                 "Identity mounts: --bind, --ro-bind, --dev-bind (and -try), --proc, --dev\n"
                 "Accepted without isolation: --as-pid-1, --perms MODE, --tmpfs DIR, --remount-ro DIR,\n"
                 "--unshare-all and individual --unshare-* options, --share-net, --cap-drop CAP,\n"
                 "--disable-userns, --assert-userns-disabled, --seccomp FD, --add-seccomp-fd FD\n"
                 "Descriptors: --info-fd FD, --sync-fd FD\n"
                 "Namespace, read-only, tmpfs masking and seccomp requests do not isolate commands.\n"
                 "Non-identity binds and unknown setup operations are rejected.");
            return 0;
        }
        if (!strcmp(opt, "--args")) { expand_args(&argc, &argv, i); i--; }
        else if (!strcmp(opt, "--chdir")) cwd = take(&i, argc, argv);
        else if (!strcmp(opt, "--argv0")) argv0 = take(&i, argc, argv);
        else if (!strcmp(opt, "--new-session")) new_session = 1;
        else if (!strcmp(opt, "--die-with-parent")) die_with_parent = 1;
        else if (!strcmp(opt, "--clearenv")) { if (clearenv()) fail("clearenv", strerror(errno)); }
        else if (!strcmp(opt, "--setenv")) {
            const char *name = take(&i, argc, argv), *value = take(&i, argc, argv);
            if (setenv(name, value, 1)) fail("setenv", name);
        } else if (!strcmp(opt, "--unsetenv")) {
            const char *name = take(&i, argc, argv);
            if (unsetenv(name)) fail("unsetenv", name);
        } else if (!strcmp(opt, "--bind") || !strcmp(opt, "--ro-bind") ||
                   !strcmp(opt, "--dev-bind") || !strcmp(opt, "--bind-try") ||
                   !strcmp(opt, "--ro-bind-try") || !strcmp(opt, "--dev-bind-try")) {
            const char *src = take(&i, argc, argv), *dst = take(&i, argc, argv);
            identity_bind(src, dst, strstr(opt, "-try") != NULL);
        } else if (!strcmp(opt, "--proc") || !strcmp(opt, "--dev")) {
            identity_bind(!strcmp(opt, "--proc") ? "/proc" : "/dev", take(&i, argc, argv), 0);
        } else if (!strcmp(opt, "--perms") || !strcmp(opt, "--tmpfs") ||
                   !strcmp(opt, "--remount-ro") || !strcmp(opt, "--cap-drop")) {
            /* These are isolation-only here. In particular, never chmod a
             * real directory when the caller requested a private tmpfs. */
            (void)take(&i, argc, argv);
        } else if (!strcmp(opt, "--unshare-all") || !strcmp(opt, "--unshare-user") ||
                   !strcmp(opt, "--unshare-user-try") || !strcmp(opt, "--unshare-pid") ||
                   !strcmp(opt, "--unshare-ipc") || !strcmp(opt, "--unshare-net") ||
                   !strcmp(opt, "--unshare-uts") || !strcmp(opt, "--unshare-cgroup") ||
                   !strcmp(opt, "--unshare-cgroup-try") || !strcmp(opt, "--share-net") ||
                   !strcmp(opt, "--as-pid-1") || !strcmp(opt, "--disable-userns") ||
                   !strcmp(opt, "--assert-userns-disabled")) {
            /* No nested isolation; Android retains its app boundary. */
        } else if (!strcmp(opt, "--seccomp") || !strcmp(opt, "--add-seccomp-fd")) {
            int fd = number(take(&i, argc, argv));
            if (fcntl(fd, F_GETFD) < 0) fail("invalid seccomp fd", strerror(errno));
            close(fd);
        } else if (!strcmp(opt, "--info-fd")) info_fd = number(take(&i, argc, argv));
        else if (!strcmp(opt, "--sync-fd")) {
            int fd = number(take(&i, argc, argv));
            int flags = fcntl(fd, F_GETFD);
            if (flags < 0 || fcntl(fd, F_SETFD, flags & ~FD_CLOEXEC))
                fail("invalid sync fd", strerror(errno));
        } else fail("unsupported option", opt);
    }
    if (i == argc) fail("missing command", "bwrap");
    fputs("arlinux-bwrap: compatibility mode; no nested filesystem/network isolation\n", stderr);
    if (cwd && chdir(cwd)) fail("chdir", cwd);
    if (new_session && setsid() < 0 && getsid(0) != getpid()) fail("setsid", strerror(errno));
    if (die_with_parent) {
        if (prctl(PR_SET_PDEATHSIG, SIGKILL)) fail("PR_SET_PDEATHSIG", strerror(errno));
        if (getppid() != parent) return 125;
    }
    if (info_fd >= 0) {
        if (dprintf(info_fd, "{\"child-pid\":%ld}\n", (long)getpid()) < 0)
            fail("write info fd", strerror(errno));
        close(info_fd);
    }
    const char *command = argv[i];
    if (argv0) argv[i] = (char *)argv0;
    execvpe(command, argv + i, environ);
    int error = errno;
    perror("arlinux-bwrap: exec");
    return error == ENOENT ? 127 : 126;
}
