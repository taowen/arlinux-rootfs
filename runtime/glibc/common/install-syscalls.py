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
shutil.copyfile(common / 'android-syscall.c', source / 'misc/android-syscall.c')
replace('misc/Makefile', 'include ../Rules', 'routines += android-syscall\n\ninclude ../Rules')
replace('misc/Versions', '  GLIBC_PRIVATE {', '  GLIBC_PRIVATE {\n    __arlinux_root_path;')

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
  .if ARLINUX_PLATFORM_CALL (SYS_ify (name)); \\
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
        '#define ARLINUX_RAW_SYSCALL 1\n#include "pthreadP.h"\n#include <android-syscall.h>')
replace('nptl/cancellation.c', 'long int\n__internal_syscall_cancel (', 'static long int\nandroid_raw_syscall_cancel (')
path = source / 'nptl/cancellation.c'
text = path.read_text()
marker = '/* Called by the SYSCALL_CANCEL macro'
wrapper = '''long int
__internal_syscall_cancel (__syscall_arg_t a1, __syscall_arg_t a2,
                           __syscall_arg_t a3, __syscall_arg_t a4,
                           __syscall_arg_t a5, __syscall_arg_t a6,
                           __SYSCALL_CANCEL7_ARG_DEF __syscall_arg_t nr)
{
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
