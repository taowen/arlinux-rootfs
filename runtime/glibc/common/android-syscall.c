/* Android adaptation at the libc syscall boundary. No heap allocation,
   environment lookup, loader lookup, or public libc calls are needed here. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <android-syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <android-path.h>
#include <sys/utsname.h>

/* The execution profile is immutable for a process. Keep it after clearenv;
   nested exec receives the profile from the process-launch policy. This never
   changes the Android UID or grants kernel privileges. */
static int virtual_root;
static char executable[4096];
int __arlinux_virtual_root (void) { return virtual_root; }

static int
process_executable (const char *path)
{
  if (!path || !executable[0]) return 0;
  if (!strcmp (path, "/proc/self/exe") || !strcmp (path, "/proc/thread-self/exe"))
    return 1;
  if (strncmp (path, "/proc/", 6)) return 0;
  const char *cursor = path + 6;
  unsigned long pid = 0;
  while (*cursor >= '0' && *cursor <= '9')
    { if (pid > 214748364UL) return 0; pid = pid * 10 + *cursor++ - '0'; }
  if (pid != (unsigned long) INTERNAL_SYSCALL_CALL (getpid)) return 0;
  if (!strcmp (cursor, "/exe")) return 1;
  if (strncmp (cursor, "/task/", 6)) return 0;
  cursor += 6;
  unsigned long tid = 0;
  while (*cursor >= '0' && *cursor <= '9')
    { if (tid > 214748364UL) return 0; tid = tid * 10 + *cursor++ - '0'; }
  return tid == (unsigned long) INTERNAL_SYSCALL_CALL (gettid) && !strcmp (cursor, "/exe");
}
static void __attribute__ ((constructor))
initialize_identity (void)
{
  __arlinux_namespace_init (getenv ("BIONICX_NS_STATE"));
  const char *value = getenv ("BIONICX_VIRTUAL_ROOT");
  virtual_root = value != 0 && value[0] != 0
                 && !(value[0] == '0' && value[1] == 0);
  value = getenv ("BIONICX_EXECFN");
  if (value && value[0] == '/' && strnlen (value, sizeof executable) < sizeof executable)
    strcpy (executable, value);
  /* Startup only: remove bookkeeping without allocator or loader dependencies. */
  for (char **item = __environ; item && *item; )
    if (!strncmp (*item, "BIONICX_EXECFN=", sizeof ("BIONICX_EXECFN=") - 1) ||
        !strncmp (*item, "BIONICX_NS_STATE=", sizeof ("BIONICX_NS_STATE=") - 1))
      {
        char **next = item;
        do { next[0] = next[1]; ++next; } while (*next);
      }
    else ++item;
}

/* Expose kernel identity already available through uname when SELinux denies
   /proc/version. Publish complete contents atomically for concurrent readers. */
static long
proc_version (const char *path, char buffer[4096])
{
  if (strcmp (path, "/proc/version")) return 0;
  long native = INTERNAL_SYSCALL_CALL (openat, AT_FDCWD, path, O_RDONLY | O_CLOEXEC, 0);
  if (native >= 0)
    { INTERNAL_SYSCALL_CALL (close, native); return 0; }
  if (native != -EACCES && native != -EPERM) return 0;
  const char root[] = _PATH_TMP;
  size_t length = sizeof root - 1;
  if (length + sizeof ("arlinux-proc-version") > 4096) return -ENAMETOOLONG;
  memcpy (buffer, root, length);
  memcpy (buffer + length, "arlinux-proc-version", sizeof ("arlinux-proc-version"));
  struct utsname info;
  long result = INTERNAL_SYSCALL_CALL (uname, &info);
  if (result < 0) return result;
  char text[sizeof info + 32] = "Linux version ";
  strcat (text, info.release);
  strcat (text, " ");
  strcat (text, info.version);
  strcat (text, "\n");
  char temporary[4096];
  strcpy (temporary, buffer);
  size_t out = strlen (temporary);
  temporary[out++] = '.';
  unsigned long tid = INTERNAL_SYSCALL_CALL (gettid);
  do { temporary[out++] = "0123456789abcdef"[tid & 15]; tid >>= 4; } while (tid);
  temporary[out] = 0;
  long fd = INTERNAL_SYSCALL_CALL (openat, AT_FDCWD, temporary,
                                   O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
  if (fd < 0) return fd;
  size_t size = strlen (text);
  long wrote = INTERNAL_SYSCALL_CALL (write, fd, text, size);
  long closed = INTERNAL_SYSCALL_CALL (close, fd);
  result = wrote != size ? (wrote < 0 ? wrote : -EIO) : closed;
  if (result == 0)
    result = INTERNAL_SYSCALL_CALL (renameat, AT_FDCWD, temporary, AT_FDCWD, buffer);
  if (result < 0)
    { INTERNAL_SYSCALL_CALL (unlinkat, AT_FDCWD, temporary, 0); return result; }
  return 1;
}

static long
translate (long *argument, char *buffer)
{
  const char *path = (const char *) *argument;
  if (path == 0 || path[0] != '/') return 0;
  long namespace_path = __arlinux_namespace_path (path, buffer);
  if (namespace_path < 0) return namespace_path;
  if (namespace_path) { *argument = (long) buffer; return 0; }
  if (process_executable (path))
    { *argument = (long) executable; return 0; }
  long synthetic = proc_version (path, buffer);
  if (synthetic < 0) return synthetic;
  if (synthetic) { *argument = (long) buffer; return 0; }
  return android_root_path (argument, buffer);
}

long
__arlinux_path_prepare (long number, long arguments[6], char storage[8192])
{
  int first = 0, second = -1;
  switch (number)
    {
    case 27: case 33: case 34: case 35: case 48: case 53: case 54:
    case 56: case 78: case 79: case 88: case 437:
      first = 1; break;
    case 36: /* symlinkat: preserve link contents; translate its location. */
      first = 2; break;
    case 37: case 38: case 276:
      first = 1; second = 3; break;
    default:
      if (!ARLINUX_PATH_CALL (number)) return 0;
      break;
    }
  long result = translate (&arguments[first], storage);
  if (result < 0) return result;
  int final = 1;
  switch (number)
    {
    case 33: case 34: case 35: case 36: case 38: case 78: case 276:
      final = 0; break;
    case 37: final = !!(arguments[4] & AT_SYMLINK_FOLLOW); break;
    case 54: final = !(arguments[4] & AT_SYMLINK_NOFOLLOW); break;
    case 79: case 88: final = !(arguments[3] & AT_SYMLINK_NOFOLLOW); break;
    case 56:
      final = !(arguments[2] & O_NOFOLLOW) &&
              (arguments[2] & (O_CREAT | O_EXCL)) != (O_CREAT | O_EXCL);
      break;
    case 437:
      /* Do not defeat openat2's kernel-enforced resolution restrictions. */
      if (!arguments[2] || arguments[3] < 24 || ((unsigned long *) arguments[2])[2])
        return 0;
      final = !(*(unsigned long *) arguments[2] & O_NOFOLLOW) &&
              (*(unsigned long *) arguments[2] & (O_CREAT | O_EXCL)) != (O_CREAT | O_EXCL);
      break;
    }
  result = android_follow_links (first && number != 27 ? arguments[first - 1] : AT_FDCWD,
                         &arguments[first], storage, final, translate);
  if (result < 0 || second < 0) return result;
  result = translate (&arguments[second], storage + 4096);
  if (result < 0) return result;
  return android_follow_links (arguments[second - 1], &arguments[second], storage + 4096, 0, translate);
}

long
__arlinux_android_syscall (long number, long a0, long a1, long a2,
                        long a3, long a4, long a5)
{
  long arguments[6] = {a0, a1, a2, a3, a4, a5};
  long namespace_result;
  if (ARLINUX_NAMESPACE_CALL (number) &&
      __arlinux_namespace_call (number, arguments, &namespace_result)) return namespace_result;
  if (number == 95 || number == 260) {
    __arlinux_namespace_wait_prepare (number, arguments);
    long result = INTERNAL_SYSCALL_NCS_CALL (number, arguments[0], arguments[1],
      arguments[2], arguments[3], arguments[4], arguments[5]);
    return __arlinux_namespace_wait_finish (number, arguments, result);
  }
  /* File capabilities are guest package metadata, never Android privileges.
     Android prohibits the xattr syscall family for app processes. */
  if (number >= 5 && number <= 16)
    {
      if (number <= 7 && virtual_root && a1 &&
          !strcmp ((const char *) a1, "security.capability"))
        {
          if (!a2 && a3) return -EFAULT;
          if (a4 & ~3 || (a4 & 3) == 3 || a3 < 4) return -EINVAL;
          uint32_t magic;
          memcpy (&magic, (const void *) a2, sizeof magic);
          unsigned int version = magic >> 24;
          if ((magic & 0x00fffffe) || version < 1 || version > 3 ||
              a3 != (version == 1 ? 12 : version == 2 ? 20 : 24)) return -EINVAL;
          return 0;
        }
      return -EOPNOTSUPP;
    }
  if (number == 277) return -ENOSYS; /* Additional guest seccomp filters. */
  if (number == 51)
    {
      const char *path = (const char *) a0;
      if (!path) return -EFAULT;
      const char root[] = _PATH_TMP;
      size_t length = sizeof root - 6;
      if (!strcmp (path, "/") || (!strncmp (path, root, length) &&
          (!path[length] || (path[length] == '/' && !path[length + 1])))) return 0;
      char actual_root[sizeof root];
      memcpy (actual_root, root, length);
      actual_root[length] = 0;
      struct stat requested, expected;
      long result = INTERNAL_SYSCALL_CALL (newfstatat, AT_FDCWD, path, &requested, 0);
      if (result < 0) return result;
      if (!S_ISDIR (requested.st_mode)) return -ENOTDIR;
      result = INTERNAL_SYSCALL_CALL (newfstatat, AT_FDCWD, actual_root, &expected, 0);
      if (result < 0) return result;
      if (requested.st_dev == expected.st_dev && requested.st_ino == expected.st_ino)
        return 0;
      return -ENOTSUP;
    }
  if (number == 161 && a0 == 22) return -EINVAL; /* PR_SET_SECCOMP. */
  if (number == 221)
    return __arlinux_exec (AT_FDCWD, (const char *) a0, (char *const *) a1,
                           (char *const *) a2, 0);
  if (number == 281)
    return __arlinux_exec (a0, (const char *) a1, (char *const *) a2,
                           (char *const *) a3, a4);
  if (number == 78 && process_executable ((const char *) a1))
    {
      if (!a2) return -EFAULT;
      if (a3 <= 0) return -EINVAL;
      size_t length = strlen (executable);
      if (length > (size_t) a3) length = a3;
      memcpy ((void *) a2, executable, length);
      return length;
    }
  if (ARLINUX_IDENTITY_CALL (number))
    {
      /* Ask the kernel first, including its EFAULT validation for getres*. */
      long result = INTERNAL_SYSCALL_NCS_CALL (number, a0, a1, a2, a3, a4, a5);
      if (result < 0 || !virtual_root) return result;
      if (number == 148)
        *(uid_t *) a0 = *(uid_t *) a1 = *(uid_t *) a2 = 0;
      else if (number == 150)
        *(gid_t *) a0 = *(gid_t *) a1 = *(gid_t *) a2 = 0;
      return 0;
    }
  if (ARLINUX_SOCKET_CALL (number))
    {
      long address[16];
      int owned;
      long result = __arlinux_socket_prepare (arguments, address, &owned);
      if (result < 0) return result;
      result = INTERNAL_SYSCALL_NCS_CALL (number, arguments[0], arguments[1],
        arguments[2], arguments[3], arguments[4], arguments[5]);
      if (owned >= 0) INTERNAL_SYSCALL_CALL (close, owned);
      return result;
    }
  char storage[8192];
  long result = __arlinux_path_prepare (number, arguments, storage);
  if (result < 0) return result;
  result = INTERNAL_SYSCALL_NCS_CALL (number, arguments[0], arguments[1],
                                     arguments[2], arguments[3], arguments[4], arguments[5]);
  if ((number == 54 || number == 55) && virtual_root &&
      (result == -EPERM || result == -EACCES)) return 0;
  if (number == 37 && virtual_root &&
      (result == -EPERM || result == -EACCES || result == -EXDEV))
    return __arlinux_link_copy (arguments[0], (const char *) arguments[1],
                                arguments[2], (const char *) arguments[3], arguments[4]);
  return result;
}
