/* Android/AArch64 compatibility boundary, shared by libc syscall entry points. */
#ifndef ARLINUX_ANDROID_SYSCALL_H
#define ARLINUX_ANDROID_SYSCALL_H

/* Linux AArch64 syscall numbers, including calls absent from old NDK headers.
   Keep this predicate usable by both the assembler and C syscall stubs. */
#define ARLINUX_PATH_CALL(n) \
  ((n) == 5 || (n) == 6 || (n) == 8 || (n) == 9 || (n) == 11 || \
   (n) == 12 || (n) == 14 || (n) == 15 || (n) == 27 || (n) == 33 || \
   (n) == 34 || (n) == 35 || (n) == 36 || (n) == 37 || (n) == 38 || \
   (n) == 43 || (n) == 45 || (n) == 48 || (n) == 49 || (n) == 51 || \
   (n) == 53 || (n) == 54 || (n) == 56 || (n) == 78 || (n) == 79 || \
   (n) == 88 || (n) == 276 || (n) == 437)

#define ARLINUX_IDENTITY_CALL(n) \
  ((n) == 148 || (n) == 150 || ((n) >= 174 && (n) <= 177))
#define ARLINUX_SOCKET_CALL(n) ((n) == 200 || (n) == 203)
#define ARLINUX_NAMESPACE_CALL(n) ((n) == 48 || (n) == 51 || (n) == 56 || (n) == 63 || (n) == 64 || (n) == 90 || (n) == 91 || (n) == 95 || (n) == 97 || (n) == 129 || (n) == 131 || (n) == 138 || (n) == 159 || (n) == 172 || (n) == 173 || (n) == 220 || (n) == 240 || (n) == 260 || (n) == 434 || (n) == 439)
#define ARLINUX_POLICY_CALL(n) (((n) >= 5 && (n) <= 16) || (n) == 55 || (n) == 161 || (n) == 277 || ARLINUX_SOCKET_CALL (n))
#define ARLINUX_PLATFORM_CALL(n) (ARLINUX_PATH_CALL (n) || ARLINUX_IDENTITY_CALL (n) || ARLINUX_POLICY_CALL (n) || ARLINUX_NAMESPACE_CALL (n) || (n) == 221 || (n) == 281)

#ifndef __ASSEMBLER__
#include <stddef.h>
extern void __arlinux_namespace_init (const char *) __attribute__ ((visibility ("hidden")));
extern int __arlinux_namespace_export (char *, size_t) __attribute__ ((visibility ("hidden")));
extern long __arlinux_namespace_path (const char *, char[4096]) __attribute__ ((visibility ("hidden")));
extern int __arlinux_namespace_call (long, long[6], long *) __attribute__ ((visibility ("hidden")));
extern void __arlinux_namespace_wait_prepare (long, long[6]) __attribute__ ((visibility ("hidden")));
extern long __arlinux_namespace_wait_finish (long, long[6], long) __attribute__ ((visibility ("hidden")));
extern long __arlinux_link_copy (int, const char *, int, const char *, int)
  __attribute__ ((visibility ("hidden")));
extern long __arlinux_socket_prepare (long[6], void *, int *)
  __attribute__ ((visibility ("hidden")));
extern unsigned long long __arlinux_mount_id (int, const char *, int)
  __attribute__ ((visibility ("hidden")));
extern int __arlinux_virtual_root (void) __attribute__ ((visibility ("hidden")));
extern long __arlinux_exec (int, const char *, char *const[], char *const[], int)
  __attribute__ ((visibility ("hidden")));
extern long __arlinux_path_prepare (long, long[6], char[8192])
  __attribute__ ((visibility ("hidden")));
extern long __arlinux_android_syscall (long, long, long, long, long, long, long)
  __attribute__ ((visibility ("hidden")));
#endif
#endif
