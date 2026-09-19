#define _GNU_SOURCE
#include "runtime-internal.h"
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

static int is_denied_id_syscall(long number) {
#ifdef SYS_setregid
    if (number == SYS_setregid) return 1;
#endif
#ifdef SYS_setgid
    if (number == SYS_setgid) return 1;
#endif
#ifdef SYS_setreuid
    if (number == SYS_setreuid) return 1;
#endif
#ifdef SYS_setuid
    if (number == SYS_setuid) return 1;
#endif
#ifdef SYS_setresuid
    if (number == SYS_setresuid) return 1;
#endif
#ifdef SYS_setresgid
    if (number == SYS_setresgid) return 1;
#endif
#ifdef SYS_setfsuid
    if (number == SYS_setfsuid) return 1;
#endif
#ifdef SYS_setfsgid
    if (number == SYS_setfsgid) return 1;
#endif
#ifdef SYS_setgroups
    if (number == SYS_setgroups) return 1;
#endif
#ifdef SYS_capset
    if (number == SYS_capset) return 1;
#endif
    return 0;
}

static int is_android_seccomp_probe(long number) {
#ifdef SYS_rseq
    /* Debian trixie glibc registers rseq before/during libc init. */
    if (number == SYS_rseq) return 1;
#endif
#ifdef SYS_clone3
    /* glibc pthread_create selects clone when clone3 is unavailable. */
    if (number == SYS_clone3) return 1;
#endif
#ifdef SYS_landlock_create_ruleset
    if (number == SYS_landlock_create_ruleset) return 1;
#endif
#ifdef SYS_name_to_handle_at
    if (number == SYS_name_to_handle_at) return 1;
#endif
#ifdef SYS_set_robust_list
    if (number == SYS_set_robust_list) return 1;
#endif
#ifdef SYS_set_tid_address
    /* glibc records clear_child_tid at startup; zygote traps 96. */
    if (number == SYS_set_tid_address) return 1;
#endif
#ifdef SYS_unshare
    /* App UIDs cannot create namespaces. Real flags fit in 32 bits;
     * smeared x0 is handled in the SIGSYS path, not here. */
    if (number == SYS_unshare) return 1;
#endif
#ifdef SYS_shmget
    if (number == SYS_shmget) return 1;
#endif
#ifdef SYS_shmctl
    if (number == SYS_shmctl) return 1;
#endif
#ifdef SYS_shmat
    if (number == SYS_shmat) return 1;
#endif
#ifdef SYS_shmdt
    if (number == SYS_shmdt) return 1;
#endif
#ifdef SYS_statx
    if (number == SYS_statx) return 1;
#endif
#ifdef SYS_personality
    if (number == SYS_personality) return 1;
#endif
#ifdef SYS_setxattr
    if (number == SYS_setxattr) return 1;
#endif
#ifdef SYS_lsetxattr
    if (number == SYS_lsetxattr) return 1;
#endif
#ifdef SYS_fsetxattr
    /* xterm touches the pty xattr; zygote traps 7/fsetxattr. */
    if (number == SYS_fsetxattr) return 1;
#endif
#ifdef SYS_getxattr
    if (number == SYS_getxattr) return 1;
#endif
#ifdef SYS_lgetxattr
    if (number == SYS_lgetxattr) return 1;
#endif
#ifdef SYS_fgetxattr
    if (number == SYS_fgetxattr) return 1;
#endif
#ifdef SYS_listxattr
    if (number == SYS_listxattr) return 1;
#endif
#ifdef SYS_llistxattr
    if (number == SYS_llistxattr) return 1;
#endif
#ifdef SYS_flistxattr
    if (number == SYS_flistxattr) return 1;
#endif
#ifdef SYS_removexattr
    if (number == SYS_removexattr) return 1;
#endif
#ifdef SYS_lremovexattr
    if (number == SYS_lremovexattr) return 1;
#endif
#ifdef SYS_fremovexattr
    if (number == SYS_fremovexattr) return 1;
#endif
    /* SYS_io_uring_setup/enter/register (425-427) are blocked for app UIDs. */
    if (number == 425 || number == 426 || number == 427) return 1;
#ifdef SYS_io_setup
    /* aarch64 io_setup is 0; zygote traps the whole POSIX AIO family. */
    if (number == SYS_io_setup) return 1;
#endif
#ifdef SYS_io_destroy
    if (number == SYS_io_destroy) return 1;
#endif
#ifdef SYS_io_submit
    if (number == SYS_io_submit) return 1;
#endif
#ifdef SYS_io_cancel
    if (number == SYS_io_cancel) return 1;
#endif
#ifdef SYS_io_getevents
    if (number == SYS_io_getevents) return 1;
#endif
#ifdef SYS_io_pgetevents
    if (number == SYS_io_pgetevents) return 1;
#endif
    return 0;
}

static long trapped_syscall(const siginfo_t *info, void *raw_context)
{
#if defined(__aarch64__)
    /* si_syscall is 0 both for io_setup and when the field is unset.
     * x8 is the syscall the trap actually saw. */
    if (raw_context != NULL)
        return (long)((ucontext_t *)raw_context)->uc_mcontext.regs[8];
#endif
    return info->si_syscall;
}

static void append_decimal(char *message, size_t *length, long number)
{
    unsigned long value = number < 0 ? (unsigned long)-number :
                                       (unsigned long)number;
    char digits[24];
    size_t digit_count = 0;

    if (number < 0)
        message[(*length)++] = '-';
    do {
        digits[digit_count++] = (char)('0' + value % 10);
        value /= 10;
    } while (value != 0 && digit_count < sizeof(digits));
    while (digit_count != 0)
        message[(*length)++] = digits[--digit_count];
}

static void report_unknown_probe(long number, const siginfo_t *info,
                                 void *raw_context)
{
    static const char prefix[] = "BIONICX trap nr=";
    char message[160];
    size_t length = 0;
    const unsigned char *raw = (const unsigned char *)info;
    int off24 = 0;
    unsigned arch = 0;
#if defined(__aarch64__)
    ucontext_t *context = raw_context;
#else
    (void)raw_context;
#endif

    memcpy(&off24, raw + 24, sizeof(off24));
    memcpy(&arch, raw + 28, sizeof(arch));
    for (size_t index = 0; index < sizeof(prefix) - 1; ++index)
        message[length++] = prefix[index];
    append_decimal(message, &length, number);
    memcpy(message + length, " si=", 4);
    length += 4;
    append_decimal(message, &length, info->si_syscall);
    memcpy(message + length, " o24=", 5);
    length += 5;
    append_decimal(message, &length, off24);
    memcpy(message + length, " code=", 6);
    length += 6;
    append_decimal(message, &length, info->si_code);
#if defined(__aarch64__)
    memcpy(message + length, " x8=", 4);
    length += 4;
    append_decimal(message, &length,
                   context ? (long)context->uc_mcontext.regs[8] : 0);
    memcpy(message + length, " x0=", 4);
    length += 4;
    append_decimal(message, &length,
                   context ? (long)context->uc_mcontext.regs[0] : 0);
#endif
    message[length++] = '\n';
    (void)write(STDERR_FILENO, message, length);
    (void)arch;
}

#if defined(__aarch64__)
/* User VAs are 48-bit (bits 48-55 zero; bits 56-63 may be a TBI tag).
 * A leftover x8 plus a smeared x0 such as 0xce88abe00000005f is not a
 * live syscall; rewriting x0 has already killed Xwayland. */
static int aarch64_user_value_plausible(unsigned long value)
{
    return ((value >> 48) & 0xffUL) == 0 || (value >> 48) == 0xffffUL;
}

static int trap_args_look_live(void *raw_context)
{
    ucontext_t *context = raw_context;
    if (context == NULL)
        return 0;
    return aarch64_user_value_plausible((unsigned long)context->uc_mcontext.regs[0]);
}

static void set_syscall_result(void *raw_context, uint64_t value) {
    ucontext_t *context = raw_context;
    if (context == NULL)
        return;
    context->uc_mcontext.regs[0] = value;
}

static long raw_svc6(long number, long a1, long a2, long a3, long a4,
                     long a5, long a6)
{
    register long x8 __asm__("x8") = number;
    register long x0 __asm__("x0") = a1;
    register long x1 __asm__("x1") = a2;
    register long x2 __asm__("x2") = a3;
    register long x3 __asm__("x3") = a4;
    register long x4 __asm__("x4") = a5;
    register long x5 __asm__("x5") = a6;
    __asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3),
                     "r"(x4), "r"(x5) : "memory", "cc");
    return x0;
}

static volatile sig_atomic_t in_reissue;

static long reissue_trapped(void *raw_context, long number)
{
    ucontext_t *context = raw_context;
    long result;

    if (context == NULL)
        return -ENOSYS;
    if (in_reissue)
        return -ENOSYS;
    in_reissue = 1;
    result = raw_svc6(number,
                    (long)context->uc_mcontext.regs[0],
                    (long)context->uc_mcontext.regs[1],
                    (long)context->uc_mcontext.regs[2],
                    (long)context->uc_mcontext.regs[3],
                    (long)context->uc_mcontext.regs[4],
                    (long)context->uc_mcontext.regs[5]);
    in_reissue = 0;
    return result;
}

static int is_passthrough_syscall(long number)
{
#ifdef SYS_futex
    if (number == SYS_futex)
        return 1;
#endif
#ifdef SYS_futex_waitv
    if (number == SYS_futex_waitv)
        return 1;
#endif
#ifdef SYS_clock_gettime
    if (number == SYS_clock_gettime)
        return 1;
#endif
#ifdef SYS_clock_nanosleep
    if (number == SYS_clock_nanosleep)
        return 1;
#endif
#ifdef SYS_nanosleep
    if (number == SYS_nanosleep)
        return 1;
#endif
#ifdef SYS_ppoll
    if (number == SYS_ppoll)
        return 1;
#endif
#ifdef SYS_pselect6
    if (number == SYS_pselect6)
        return 1;
#endif
#ifdef SYS_epoll_pwait
    if (number == SYS_epoll_pwait)
        return 1;
#endif
#ifdef SYS_epoll_pwait2
    if (number == SYS_epoll_pwait2)
        return 1;
#endif
#ifdef SYS_epoll_ctl
    if (number == SYS_epoll_ctl)
        return 1;
#endif
#ifdef SYS_epoll_wait
    if (number == SYS_epoll_wait)
        return 1;
#endif
    return 0;
}
#endif

BIONICX_INTERNAL int bionicx_seccomp_deny_id(long number) {
    return is_denied_id_syscall(number);
}

BIONICX_INTERNAL int bionicx_seccomp_probe(long number) {
    return is_android_seccomp_probe(number);
}

static void handle_seccomp_trap(int signal_number, siginfo_t *info,
                                void *raw_context) {
    long number;

    (void)signal_number;
    number = trapped_syscall(info, raw_context);
#if defined(__aarch64__) && defined(SYS_accept) && defined(SYS_accept4)
    /* Zygote blocks accept(2) and often delivers that trap as smeared
     * SIGSYS (si_code=0). Skipping it leaves x0 as the listen fd. */
    if (number == SYS_accept) {
        ucontext_t *context = raw_context;
        if (context == NULL)
            return;
        long listen_fd = (long)context->uc_mcontext.regs[0];
        long result = syscall(SYS_accept4,
                              listen_fd,
                              (void *)(uintptr_t)context->uc_mcontext.regs[1],
                              (void *)(uintptr_t)context->uc_mcontext.regs[2],
                              0L);
        if (result < 0)
            result = -errno;
        else if (result == listen_fd)
            result = -EAGAIN;
        set_syscall_result(raw_context, (uint64_t)(int64_t)result);
        return;
    }
#endif
#if defined(__aarch64__)
    if (is_denied_id_syscall(number)) {
        set_syscall_result(raw_context, 0);
        return;
    }
#ifdef SYS_setxattr
    if (raw_context != NULL && (number == SYS_setxattr
#ifdef SYS_lsetxattr
            || number == SYS_lsetxattr
#endif
#ifdef SYS_fsetxattr
            || number == SYS_fsetxattr
#endif
            ) && bionicx_is_file_capability_xattr(
                    (const char *)(uintptr_t)((ucontext_t *)raw_context)
                            ->uc_mcontext.regs[1])) {
        set_syscall_result(raw_context, 0);
        return;
    }
#endif
#ifdef SYS_statx
    if (number == SYS_statx) {
        ucontext_t *context = raw_context;
        const char *path;
        char buffer[PATH_MAX];
        const char *actual;
        int flags;
        int result;

        if (context == NULL)
            return;
        path = (const char *)(uintptr_t)context->uc_mcontext.regs[1];
        flags = (int)context->uc_mcontext.regs[2];
        actual = path;
        if (path != NULL && path[0] != '\0'
#ifdef AT_EMPTY_PATH
            && (flags & AT_EMPTY_PATH) == 0
#endif
            ) {
            actual = bionicx_redirect_path(path, buffer);
            if (actual == NULL) {
                set_syscall_result(raw_context, (uint64_t)(int64_t)-errno);
                return;
            }
        }
        result = bionicx_statx((int)context->uc_mcontext.regs[0], actual, flags,
                               (unsigned int)context->uc_mcontext.regs[3],
                               (void *)(uintptr_t)context->uc_mcontext.regs[4]);
        set_syscall_result(raw_context, result < 0
            ? (uint64_t)(int64_t)-errno
            : 0);
        return;
    }
#endif
    if (is_android_seccomp_probe(number)) {
        set_syscall_result(raw_context, (uint64_t)-ENOSYS);
        return;
    }
    if (!trap_args_look_live(raw_context)) {
        set_syscall_result(raw_context, (uint64_t)(int64_t)-ENOSYS);
        return;
    }
#endif
    if (is_denied_id_syscall(number)) {
#if defined(__aarch64__)
        set_syscall_result(raw_context, 0);
#endif
        return;
    }
#if defined(__aarch64__) && defined(SYS_setxattr)
    if ((number == SYS_setxattr
#ifdef SYS_lsetxattr
            || number == SYS_lsetxattr
#endif
#ifdef SYS_fsetxattr
            || number == SYS_fsetxattr
#endif
            ) && raw_context != NULL && bionicx_is_file_capability_xattr(
                    (const char *)(uintptr_t)((ucontext_t *)raw_context)
                            ->uc_mcontext.regs[1])) {
        set_syscall_result(raw_context, 0);
        return;
    }
#endif
    if (is_android_seccomp_probe(number)) {
#if defined(__aarch64__)
        set_syscall_result(raw_context, (uint64_t)-ENOSYS);
#endif
        return;
    }
#if defined(__aarch64__)
#ifdef SYS_timerfd_create
    if (number == SYS_timerfd_create) {
        ucontext_t *context = raw_context;
        int result;

        if (context == NULL)
            return;
        result = bionicx_timerfd_create((int)context->uc_mcontext.regs[0],
                                        (int)context->uc_mcontext.regs[1]);
        set_syscall_result(raw_context, result < 0
            ? (uint64_t)(int64_t)-errno
            : (uint64_t)(int64_t)result);
        return;
    }
#endif
#ifdef SYS_timerfd_settime
    if (number == SYS_timerfd_settime) {
        ucontext_t *context = raw_context;
        int result;

        if (context == NULL)
            return;
        result = bionicx_timerfd_settime((int)context->uc_mcontext.regs[0],
                                         (int)context->uc_mcontext.regs[1],
                                         (const void *)(uintptr_t)context->uc_mcontext.regs[2],
                                         (void *)(uintptr_t)context->uc_mcontext.regs[3]);
        set_syscall_result(raw_context, result < 0
            ? (uint64_t)(int64_t)-errno
            : (uint64_t)(int64_t)result);
        return;
    }
#endif
#ifdef SYS_timerfd_gettime
    if (number == SYS_timerfd_gettime) {
        ucontext_t *context = raw_context;
        int result;

        if (context == NULL)
            return;
        result = bionicx_timerfd_gettime((int)context->uc_mcontext.regs[0],
                                         (void *)(uintptr_t)context->uc_mcontext.regs[1]);
        set_syscall_result(raw_context, result < 0
            ? (uint64_t)(int64_t)-errno
            : (uint64_t)(int64_t)result);
        return;
    }
#endif
#endif
#ifdef SYS_exit
    if (number == SYS_exit) {
#if defined(__aarch64__)
        ucontext_t *context = raw_context;
        _exit(context ? (int)context->uc_mcontext.regs[0] : 1);
#else
        _exit(1);
#endif
    }
#endif
#ifdef SYS_exit_group
    if (number == SYS_exit_group) {
#if defined(__aarch64__)
        ucontext_t *context = raw_context;
        _exit(context ? (int)context->uc_mcontext.regs[0] : 1);
#else
        _exit(1);
#endif
    }
#endif
#if defined(__aarch64__)
    if (is_passthrough_syscall(number)) {
        set_syscall_result(raw_context,
                           (uint64_t)(int64_t)reissue_trapped(raw_context,
                                                              number));
        return;
    }
#endif
    report_unknown_probe(number, info, raw_context);
#if defined(__aarch64__)
    /* ENOSYS here has already crashed Xwayland (futex/acct). Reissue
     * so an allowed syscall still runs; a blocked one gets EPERM. */
    set_syscall_result(raw_context,
                       (uint64_t)(int64_t)reissue_trapped(raw_context, number));
    return;
#else
    _exit(128 + SIGSYS);
#endif
}

static int install_seccomp_handler(void) {
    struct sigaction action = {0};
    action.sa_sigaction = handle_seccomp_trap;
    action.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&action.sa_mask);
    return syscall(SYS_rt_sigaction, SIGSYS, &action, NULL, (size_t)8);
}

int sigaction(int sig, const struct sigaction *act, struct sigaction *oact) {
    static int (*real_sigaction)(int, const struct sigaction *,
                                 struct sigaction *);
    if (sig == SIGSYS) {
        if (oact) {
            memset(oact, 0, sizeof(*oact));
            oact->sa_sigaction = handle_seccomp_trap;
            oact->sa_flags = SA_SIGINFO;
        }
        (void)act;
        return 0;
    }
    if (!real_sigaction)
        real_sigaction = (int (*)(int, const struct sigaction *,
                                  struct sigaction *))dlsym(RTLD_NEXT,
                                                            "sigaction");
    if (!real_sigaction) {
        errno = ENOSYS;
        return -1;
    }
    return real_sigaction(sig, act, oact);
}

typedef void (*bx_sighandler)(int);

bx_sighandler signal(int sig, bx_sighandler handler) {
    static bx_sighandler (*real_signal)(int, bx_sighandler);
    if (sig == SIGSYS)
        return SIG_DFL;
    if (!real_signal)
        real_signal = (bx_sighandler (*)(int, bx_sighandler))dlsym(RTLD_NEXT,
                                                                   "signal");
    if (!real_signal)
        return SIG_ERR;
    return real_signal(sig, handler);
}

__attribute__((constructor(101))) static void install_android_kernel_contract(void) {
    sigset_t unblock;
    /* ART/zygote may leave SIGSYS blocked. A blocked SIGSYS turns
     * seccomp RET_TRAP into a kill, which is what we see when the
     * Activity forks Xwayland on vivo but run-as launches survive. */
    sigemptyset(&unblock);
    sigaddset(&unblock, SIGSYS);
    (void)sigprocmask(SIG_UNBLOCK, &unblock, NULL);
    (void)install_seccomp_handler();
}
