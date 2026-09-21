#include "runtime-internal.h"
#include <signal.h>
#include <sys/syscall.h>
#include <sys/wait.h>

/* Isolated fork/exec mode for pidfd-backed vfork callbacks. Our exec adapters
 * allocate memory, can exceed the caller's child stack, and change loader state.
 * Running them in the parent's VM can corrupt the calling application.
 * fork() also runs libc's atfork handlers; clearing CLONE_VM alone can leave
 * allocator locks owned by a vanished thread in a multithreaded caller.
 * Preserve the pidfd and wait-until-exec/exit behavior. Pre-exec memory changes
 * are deliberately isolated from the parent;
 * this is an exec compatibility path, not general shared-memory clone support.
 * Other clone flag combinations retain their semantics in namespace.c. */
int bionicx_fork_exec(int (*fn)(void *), void *arg, int *pidfd)
{
    int ready[2];
    if (!pidfd) { errno = EFAULT; return -1; }
    if (pipe2(ready, O_CLOEXEC) != 0) return -1;
    pid_t child = fork();
    if (child == 0) {
        close(ready[0]);
        _exit(fn(arg));
    }
    int saved = errno;
    close(ready[1]);
    if (child < 0) {
        close(ready[0]);
        errno = saved;
        return -1;
    }
    int fd = syscall(SYS_pidfd_open, child, 0);
    if (fd < 0) {
        saved = errno;
        kill(child, SIGKILL);
        while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {}
        close(ready[0]);
        errno = saved;
        return -1;
    }
    char unused;
    while (read(ready[0], &unused, 1) < 0 && errno == EINTR) {}
    close(ready[0]);
    *pidfd = fd;
    return child;
}
