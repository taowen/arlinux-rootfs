#include "runtime-internal.h"

#include <dlfcn.h>
#include <pthread.h>
#include <pty.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>

#define SHELL_CHILD_SLOTS 64

struct shell_child {
    FILE *stream;
    pid_t pid;
};

static pthread_mutex_t shell_children_lock = PTHREAD_MUTEX_INITIALIZER;
static struct shell_child shell_children[SHELL_CHILD_SLOTS];

/* libutil forkpty() calls login_tty() in the child. If TIOCSCTTY fails,
 * glibc leaves stdin on the inherited /dev/null and the child either
 * _exit(1) or execs bash, which then sees EOF and exits 0. Always attach
 * the slave to 0/1/2 even when TIOCSCTTY is denied. */
static void trace_forkpty(const char *phase, pid_t pid, int err) {
    if (bionicx_captured_value("BIONICX_LOG_EXEC") == NULL &&
            getenv("BIONICX_LOG_EXEC") == NULL)
        return;
    const char *tmp = bionicx_captured_tmpdir();
    if (tmp == NULL || tmp[0] != '/') tmp = bionicx_getenv("BIONICX_TMPDIR");
    if (tmp == NULL || tmp[0] != '/') return;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/forkpty.log", tmp) >= PATH_MAX)
        return;
    int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd < 0) return;
    char line[160];
    int n = snprintf(line, sizeof(line), "%s pid=%d errno=%d\n",
                     phase, (int)pid, err);
    if (n > 0) (void)write(fd, line, (size_t)n);
    close(fd);
}

pid_t forkpty(int *amaster, char *name, const struct termios *termp,
              const struct winsize *winp) {
    static int (*real_openpty)(int *, int *, char *, const struct termios *,
                               const struct winsize *);
    if (real_openpty == NULL)
        real_openpty = dlsym(RTLD_NEXT, "openpty");
    if (real_openpty == NULL || amaster == NULL) {
        errno = ENOSYS;
        trace_forkpty("enosys", -1, errno);
        return -1;
    }
    int master = -1, slave = -1;
    if (real_openpty(&master, &slave, name, termp, winp) < 0) {
        trace_forkpty("openpty", -1, errno);
        return -1;
    }
    pid_t pid = fork();
    if (pid < 0) {
        int saved = errno;
        close(master);
        close(slave);
        errno = saved;
        trace_forkpty("fork", -1, saved);
        return -1;
    }
    if (pid == 0) {
        close(master);
        (void)setsid();
        (void)ioctl(slave, TIOCSCTTY, 0);
        if (dup2(slave, STDIN_FILENO) < 0 ||
                dup2(slave, STDOUT_FILENO) < 0 ||
                dup2(slave, STDERR_FILENO) < 0)
            _exit(127);
        if (slave > STDERR_FILENO) close(slave);
        trace_forkpty("child", 0, 0);
        return 0;
    }
    close(slave);
    *amaster = master;
    trace_forkpty("parent", pid, 0);
    return pid;
}

char *ptsname(int fd) {
    static __thread char buffer[64];
    unsigned int ptyno = 0;
    if (ioctl(fd, TIOCGPTN, &ptyno) == 0) {
        int count = snprintf(buffer, sizeof(buffer), "/dev/pts/%u", ptyno);
        if (count > 0 && count < (int)sizeof(buffer)) return buffer;
    }
    static char *(*next)(int);
    if (next == NULL) next = dlsym(RTLD_NEXT, "ptsname");
    return next != NULL ? next(fd) : NULL;
}

int ptsname_r(int fd, char *buffer, size_t length) {
    unsigned int ptyno = 0;
    if (ioctl(fd, TIOCGPTN, &ptyno) == 0) {
        int count = snprintf(buffer, length, "/dev/pts/%u", ptyno);
        if (count > 0 && (size_t)count < length) return 0;
        errno = ERANGE;
        return ERANGE;
    }
    static int (*next)(int, char *, size_t);
    if (next == NULL) next = dlsym(RTLD_NEXT, "ptsname_r");
    if (next == NULL) return errno != 0 ? errno : ENOSYS;
    return next(fd, buffer, length);
}

static const char *rootfs_shell(char path[PATH_MAX]) {
    const char *root = bionicx_getenv("BIONICX_ROOTFS");
    if (root == NULL || root[0] != '/' ||
            snprintf(path, PATH_MAX, "%s/bin/sh", root) >= PATH_MAX) {
        errno = ENOENT;
        return NULL;
    }
    return path;
}

FILE *popen(const char *command, const char *type) {
    if (command == NULL || type == NULL ||
            (strcmp(type, "r") != 0 && strcmp(type, "re") != 0 &&
             strcmp(type, "w") != 0 && strcmp(type, "we") != 0)) {
        errno = EINVAL;
        return NULL;
    }
    int descriptors[2];
    if (pipe2(descriptors, O_CLOEXEC) != 0) return NULL;
    int reading = type[0] == 'r';
    pid_t child = fork();
    if (child < 0) {
        int saved = errno;
        close(descriptors[0]);
        close(descriptors[1]);
        errno = saved;
        return NULL;
    }
    if (child == 0) {
        int child_end = reading ? descriptors[1] : descriptors[0];
        int standard = reading ? STDOUT_FILENO : STDIN_FILENO;
        close(reading ? descriptors[0] : descriptors[1]);
        if (dup2(child_end, standard) < 0) _exit(126);
        close(child_end);
        char shell[PATH_MAX];
        if (rootfs_shell(shell) == NULL) _exit(126);
        char *const arguments[] = {
            (char *)"sh", (char *)"-c", (char *)"--", (char *)command, NULL
        };
        execv(shell, arguments);
        _exit(127);
    }
    int parent_end = reading ? descriptors[0] : descriptors[1];
    close(reading ? descriptors[1] : descriptors[0]);
    if (type[1] != 'e') {
        int flags = fcntl(parent_end, F_GETFD);
        if (flags >= 0) (void)fcntl(parent_end, F_SETFD, flags & ~FD_CLOEXEC);
    }
    FILE *stream = fdopen(parent_end, reading ? "r" : "w");
    if (stream == NULL) {
        int saved = errno;
        close(parent_end);
        (void)waitpid(child, NULL, 0);
        errno = saved;
        return NULL;
    }
    pthread_mutex_lock(&shell_children_lock);
    for (size_t index = 0; index < SHELL_CHILD_SLOTS; ++index) {
        if (shell_children[index].stream != NULL) continue;
        shell_children[index].stream = stream;
        shell_children[index].pid = child;
        pthread_mutex_unlock(&shell_children_lock);
        return stream;
    }
    pthread_mutex_unlock(&shell_children_lock);
    fclose(stream);
    (void)waitpid(child, NULL, 0);
    errno = EMFILE;
    return NULL;
}

int pclose(FILE *stream) {
    pid_t child = -1;
    pthread_mutex_lock(&shell_children_lock);
    for (size_t index = 0; index < SHELL_CHILD_SLOTS; ++index) {
        if (shell_children[index].stream != stream) continue;
        child = shell_children[index].pid;
        shell_children[index].stream = NULL;
        shell_children[index].pid = 0;
        break;
    }
    pthread_mutex_unlock(&shell_children_lock);
    if (child < 0) { errno = EINVAL; return -1; }
    int close_result = fclose(stream);
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return close_result == 0 ? status : -1;
}

int system(const char *command) {
    if (command == NULL) {
        char shell[PATH_MAX];
        return rootfs_shell(shell) != NULL && access(shell, X_OK) == 0;
    }
    pid_t child = fork();
    if (child < 0) return -1;
    if (child == 0) {
        char shell[PATH_MAX];
        if (rootfs_shell(shell) == NULL) _exit(126);
        char *const arguments[] = {
            (char *)"sh", (char *)"-c", (char *)"--", (char *)command, NULL
        };
        execv(shell, arguments);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    return status;
}
