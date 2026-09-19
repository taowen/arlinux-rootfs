#define _GNU_SOURCE
#include "runtime-internal.h"

#include <dlfcn.h>
#include <sys/wait.h>

/*
 * Android kernels often have pidfd_open but not waitid(P_PIDFD)
 * (Linux 5.4). GLib child-watch then fails with EINVAL.
 */

#ifndef P_PIDFD
#define P_PIDFD 3
#endif

static pid_t pid_from_pidfd(int pidfd)
{
    char path[64];
    char line[256];
    FILE *info;
    pid_t pid = -1;

    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", pidfd);
    info = fopen(path, "r");
    if (!info)
        return -1;
    while (fgets(line, sizeof(line), info)) {
        if (sscanf(line, "Pid: %d", &pid) == 1)
            break;
    }
    fclose(info);
    return pid;
}

int waitid(idtype_t idtype, id_t id, siginfo_t *infop, int options)
{
    static int (*real_waitid)(idtype_t, id_t, siginfo_t *, int);
    int rc;
    pid_t pid;

    if (!real_waitid)
        real_waitid = (int (*)(idtype_t, id_t, siginfo_t *, int))
            dlsym(RTLD_NEXT, "waitid");
    if (!real_waitid) {
        errno = ENOSYS;
        return -1;
    }
    if (idtype == P_PID) id = (id_t)bionicx_ns_host_pid((pid_t)id);
    rc = real_waitid(idtype, id, infop, options);
    if (rc == 0 && infop) infop->si_pid = bionicx_ns_guest_pid(infop->si_pid);
    if (rc == 0 || idtype != (idtype_t)P_PIDFD || errno != EINVAL)
        return rc;
    pid = pid_from_pidfd((int)id);
    if (pid <= 0) {
        errno = EINVAL;
        return -1;
    }
    rc = real_waitid(P_PID, (id_t)pid, infop, options);
    if (rc == 0 && infop) infop->si_pid = bionicx_ns_guest_pid(infop->si_pid);
    return rc;
}
