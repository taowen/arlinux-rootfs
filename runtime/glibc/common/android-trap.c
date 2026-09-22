/* Android's inherited seccomp filter can report unsupported operations with
   SIGSYS instead of ENOSYS. Inline application syscalls bypass libc; report
   unavailability without pretending they acquired kernel privileges. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <signal.h>
#include <ucontext.h>

static void
unsupported (int number, siginfo_t *info, void *opaque)
{
  ucontext_t *context = opaque;
  if (number != SIGSYS || !context || !info) return;
  context->uc_mcontext.regs[0] = -ENOSYS;
}

static void __attribute__ ((constructor))
initialize_traps (void)
{
  struct sigaction action = {0};
  action.sa_sigaction = unsupported;
  action.sa_flags = SA_SIGINFO;
  __sigaction (SIGSYS, &action, NULL);
  sigset_t unblocked;
  sigemptyset (&unblocked);
  sigaddset (&unblocked, SIGSYS);
  __sigprocmask (SIG_UNBLOCK, &unblocked, NULL);
}
