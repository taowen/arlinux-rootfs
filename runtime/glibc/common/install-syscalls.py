"""Install the Android syscall boundary in either supported glibc source tree."""
from pathlib import Path
import re
import shutil
import sys

source = Path(sys.argv[1])
common = Path(__file__).resolve().parent


def replace(path, before, after):
    path = source / path
    text = path.read_text()
    if text.count(before) != 1:
        raise SystemExit(f'glibc source contract changed: {path}: {before!r}')
    path.write_text(text.replace(before, after))


shutil.copyfile(common / 'android-syscall.h', source / 'include/android-syscall.h')
shutil.copyfile(common / 'android-path.h', source / 'include/android-path.h')
# rtld cannot call libc's initialized policy. It uses the same allocation-free
# pathname resolver directly, so alternatives/symlinked DSOs work at startup.
replace('elf/dl-load.c', '#include <sysdep.h>',
        '#include <sysdep.h>\n#include <android-path.h>')
replace('elf/dl-misc.c', '#include <unistd.h>',
        '#include <unistd.h>\n#include <sysdep.h>\n#include <android-path.h>')
replace('elf/dl-misc.c',
        '  int fd = __open64_nocancel (file, O_RDONLY | O_CLOEXEC);', '''  char rooted[4096];
  long pathname = (long) file;
  long status = android_root_path (&pathname, rooted);
  int fd = -1;
  if (status < 0)
    __set_errno (-status);
  else
    fd = __open64_nocancel ((const char *) pathname, O_RDONLY | O_CLOEXEC);''')
replace('elf/dl-load.c', '    fd = __open64_nocancel (name, O_RDONLY | O_CLOEXEC);', '''    {
      char rooted[4096], resolved[4096];
      long path = (long) name;
      long result = android_root_path (&path, rooted);
      if (result == 0)
        result = android_follow_links (AT_FDCWD, &path, resolved, 1, android_root_path);
      if (result < 0)
        __set_errno (-result);
      else
        fd = __open64_nocancel ((const char *) path, O_RDONLY | O_CLOEXEC);
    }''')
shutil.copyfile(common / 'android-syscall.c', source / 'misc/android-syscall.c')
shutil.copyfile(common / 'android-exec.c', source / 'misc/android-exec.c')
shutil.copyfile(common / 'android-trap.c', source / 'misc/android-trap.c')
shutil.copyfile(common / 'android-statx.c', source / 'misc/android-statx.c')
shutil.copyfile(common / 'android-socket.c', source / 'misc/android-socket.c')
shutil.copyfile(common / 'android-link.c', source / 'misc/android-link.c')
shutil.copyfile(common / 'android-namespace.c', source / 'misc/android-namespace.c')
# The inherited fchmodat fallback predates AT_EMPTY_PATH. Keep one descriptor
# pinned for the existing /proc/self/fd implementation, including O_PATH fds;
# never follow a symlink descriptor to change its target's permissions.
replace('sysdeps/unix/sysv/linux/fchmodat.c',
        '  if (flag != AT_SYMLINK_NOFOLLOW)',
        '  if (flag & ~(AT_SYMLINK_NOFOLLOW | AT_EMPTY_PATH))')
replace('sysdeps/unix/sysv/linux/fchmodat.c',
        '  int pathfd = __openat_nocancel (fd, file,\n\t\t\t\t  O_PATH | O_NOFOLLOW | O_CLOEXEC);',
        '''  int pathfd;
  if ((flag & AT_EMPTY_PATH) && file[0] == '\\0' && fd != AT_FDCWD)
    pathfd = INLINE_SYSCALL_CALL (fcntl, fd, F_DUPFD_CLOEXEC, 0);
  else
    pathfd = __openat_nocancel (fd,
      (flag & AT_EMPTY_PATH) && file[0] == '\\0' ? "." : file,
      O_PATH | O_CLOEXEC | ((flag & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0));''')
replace('misc/Makefile', 'include ../Rules', 'routines += android-syscall android-exec android-trap android-statx android-socket android-link android-namespace\n\ninclude ../Rules')
clone = source / 'sysdeps/unix/sysv/linux/aarch64/clone.S'
text = clone.read_text().replace('__clone', '__arlinux_kernel_clone')
text = text.replace('weak_alias (__arlinux_kernel_clone, clone)', '')
clone.write_text(text)
# Exec adaptation uses stack-owned scratch so vfork children cannot leave
# anonymous allocations behind in the parent's shared address space.
replace('sysdeps/unix/sysv/linux/spawni.c', '  argv_size += (32 * 1024);', '''  size_t envc = 0;
  while (envp && envp[envc]) ++envc;
  argv_size += (128 * 1024) + envc * sizeof (void *);''')

statx_type = 'io/bits/types/struct_statx.h'
if 'stx_mnt_id;' not in (source / statx_type).read_text():
    # Name the ABI field occupying the first reserved uint64_t in 2.41.
    # The total size and all following offsets remain unchanged.
    replace(statx_type, '__uint64_t __statx_pad2[14];',
            '__uint64_t stx_mnt_id;\n  __uint64_t __statx_pad2[13];')
replace('io/statx_generic.c', '#include <errno.h>', '#include <errno.h>\n#include <android-syscall.h>')
replace('io/statx_generic.c', '  memcpy (buf, &obuf, sizeof (obuf));', '''  if (mask & 0x1000U)
    {
      unsigned long long id = __arlinux_mount_id (fd, path, flags);
      _Static_assert (__builtin_offsetof (struct original_statx, stx_mnt_id) == 144,
                      "Linux statx mount ID offset changed");
      if (id) { obuf.stx_mnt_id = id; obuf.stx_mask |= 0x1000U; }
    }
  memcpy (buf, &obuf, sizeof (obuf));''')

# POSIX SHM and named semaphores share the normal /dev/shm namespace. The
# syscall boundary maps it once, including calls hidden inside libc.
directory = source / 'include/shm-directory.h'
text, count = re.subn(r'#define SHMDIR "[^"]+"', '#define SHMDIR "/dev/shm/"', directory.read_text())
if count != 1:
    raise SystemExit('glibc POSIX shared-memory directory changed')
directory.write_text(text)
# App-data SELinux policy can prohibit hard links. Publish the initialized
# inode atomically without replacing an existing semaphore. A symlink breaks
# O_NOFOLLOW; copying breaks sharing between the creator and later openers.
replace('sysdeps/pthread/sem_open.c', '#include <fcntl.h>',
        '#include <fcntl.h>\n#include <stdio.h>')
replace('sysdeps/pthread/sem_open.c', '__symlink (tmpfname, dirname.name)',
        '__renameat2 (AT_FDCWD, tmpfname, AT_FDCWD, dirname.name, RENAME_NOREPLACE)')

sysdep = source / 'sysdeps/unix/sysv/linux/aarch64/sysdep.h'
text = sysdep.read_text()
marker = '#else /* not __ASSEMBLER__ */'
assembly = '''#if IS_IN (libc)
#include <android-syscall.h>
#undef DO_CALL
#define DO_CALL(name, args) \\
  .if ARLINUX_PLATFORM_CALL (SYS_ify (name)) && SYS_ify (name) != 220; \\
    stp x29, x30, [sp, -16]!; \\
    cfi_adjust_cfa_offset (16); cfi_rel_offset (x29, 0); cfi_rel_offset (x30, 8); \\
    mov x6, x5; mov x5, x4; mov x4, x3; mov x3, x2; \\
    mov x2, x1; mov x1, x0; mov x0, SYS_ify (name); \\
    bl __arlinux_android_syscall; \\
    ldp x29, x30, [sp], 16; \\
    cfi_restore (x29); cfi_restore (x30); cfi_adjust_cfa_offset (-16); \\
  .else; mov x8, SYS_ify (name); svc 0; .endif
#endif

'''
if text.count(marker) != 1:
    raise SystemExit('glibc AArch64 assembly boundary changed')
text = text.replace(marker, assembly + marker)
marker = '# undef INTERNAL_SYSCALL\n'
arguments = []
for count in range(8):
    names = [f'a{i}' for i in range(count)]
    values = [f'(long) ({name})' for name in names[:6]] + ['0'] * max(0, 6-count)
    arguments.append(f'#define ARLINUX_ARGS_{count}({",".join(names)}) {", ".join(values)}')
hook = '''#if IS_IN (libc) && !defined ARLINUX_RAW_SYSCALL
#include <android-syscall.h>
''' + '\n'.join(arguments) + '''
#define ARLINUX_KERNEL_CALL(name, nr, args...) \\
  ({ long _sys_result; { LOAD_ARGS_##nr (args) \\
     register long _x8 asm ("x8") = (name); \\
     asm volatile ("svc 0" : "=r" (_x0) : "r" (_x8) ASM_ARGS_##nr : "memory"); \\
     _sys_result = _x0; } _sys_result; })
#undef INTERNAL_SYSCALL_RAW
#define INTERNAL_SYSCALL_RAW(name, nr, args...) \\
  (ARLINUX_PLATFORM_CALL (name) \\
   ? __arlinux_android_syscall (name, ARLINUX_ARGS_##nr (args)) \\
   : ARLINUX_KERNEL_CALL (name, nr, args))
#endif

'''
if text.count(marker) != 1:
    raise SystemExit('glibc AArch64 C boundary changed')
sysdep.write_text(text.replace(marker, hook + marker))

# Translate before entering the upstream cancellation region. Keep its markers
# and side-effect handling unchanged; only the syscall arguments are adapted.
replace('nptl/cancellation.c', '#include "pthreadP.h"',
        '#define ARLINUX_RAW_SYSCALL 1\n#include "pthreadP.h"\n#include <libc-lock.h>\n#include <android-syscall.h>')
replace('nptl/cancellation.c', 'long int\n__internal_syscall_cancel (', 'static long int\nandroid_raw_syscall_cancel (')
path = source / 'nptl/cancellation.c'
text = path.read_text()
marker = '/* Called by the SYSCALL_CANCEL macro'
wrapper = '''static void
android_close_socket_directory (void *opaque)
{
  int fd = *(int *) opaque;
  if (fd >= 0) INTERNAL_SYSCALL_CALL (close, fd);
}

long int
__internal_syscall_cancel (__syscall_arg_t a1, __syscall_arg_t a2,
                           __syscall_arg_t a3, __syscall_arg_t a4,
                           __syscall_arg_t a5, __syscall_arg_t a6,
                           __SYSCALL_CANCEL7_ARG_DEF __syscall_arg_t nr)
{
  if (ARLINUX_NAMESPACE_CALL (nr))
    {
      long arguments[6] = {a1, a2, a3, a4, a5, a6}, result;
      if (__arlinux_namespace_call (nr, arguments, &result)) return result;
      if (nr == 95 || nr == 260)
        {
          __arlinux_namespace_wait_prepare (nr, arguments);
          result = android_raw_syscall_cancel (arguments[0], arguments[1], arguments[2],
            arguments[3], arguments[4], arguments[5], __SYSCALL_CANCEL7_ARG nr);
          return __arlinux_namespace_wait_finish (nr, arguments, result);
        }
    }
  if (ARLINUX_SOCKET_CALL (nr))
    {
      long arguments[6] = {a1, a2, a3, a4, a5, a6}, address[16];
      int owned;
      long result = __arlinux_socket_prepare (arguments, address, &owned);
      if (result < 0) return result;
      __libc_cleanup_push (android_close_socket_directory, &owned);
      result = android_raw_syscall_cancel (arguments[0], arguments[1], arguments[2],
        arguments[3], arguments[4], arguments[5], __SYSCALL_CANCEL7_ARG nr);
      __libc_cleanup_pop (1);
      return result;
    }
  if (ARLINUX_PATH_CALL (nr))
    {
      long arguments[6] = {a1, a2, a3, a4, a5, a6};
      char storage[8192];
      long result = __arlinux_path_prepare (nr, arguments, storage);
      if (result < 0) return result;
      return android_raw_syscall_cancel (arguments[0], arguments[1], arguments[2],
          arguments[3], arguments[4], arguments[5], __SYSCALL_CANCEL7_ARG nr);
    }
  return android_raw_syscall_cancel (a1, a2, a3, a4, a5, a6, __SYSCALL_CANCEL7_ARG nr);
}

'''
if text.count(marker) != 1:
    raise SystemExit('glibc cancellation boundary changed')
path.write_text(text.replace(marker, wrapper + marker))

replace('sysdeps/unix/sysv/linux/syscall.c', '#include <stdarg.h>', '#include <stdarg.h>\n#include <sysdep.h>')
replace('sysdeps/unix/sysv/linux/syscall.c', 'return syscallS (number, a0, a1, a2, a3, a4, a5);',
        '''long result = INTERNAL_SYSCALL_NCS_CALL (number, a0, a1, a2, a3, a4, a5);
      if (INTERNAL_SYSCALL_ERROR_P (result))
        { __set_errno (INTERNAL_SYSCALL_ERRNO (result)); return -1; }
      return result;''')
