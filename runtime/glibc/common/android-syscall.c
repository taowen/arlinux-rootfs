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

static int
under (const char *path, const char *directory)
{
  unsigned int n = 0;
  while (directory[n])
    {
      if (directory[n] != path[n]) return 0;
      ++n;
    }
  return path[n] == '/' || path[n] == 0;
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
  const char *suffix = path;
  const char *extra = "";
  if (under (path, "/dev/shm"))
    {
      extra = "/tmp/dev-shm";
      suffix = path + 8;
    }
  else if (!(under (path, "/usr") || under (path, "/bin") ||
             under (path, "/sbin") || under (path, "/lib") ||
             under (path, "/lib64") || under (path, "/etc") ||
             under (path, "/opt") || under (path, "/var") ||
             under (path, "/tmp") || under (path, "/run") ||
             path[1] == 0 || (path[1] == '.' && path[2] == 0)))
    return 0;
  /* _PATH_TMP is the configured app-private prefix followed by /tmp/. */
  const char root[] = _PATH_TMP;
  unsigned int out = 0;
  for (; out < sizeof (root) - 6; ++out) buffer[out] = root[out];
  for (unsigned int i = 0; extra[i]; ++i) buffer[out++] = extra[i];
  if (extra[0])
    {
      buffer[out] = 0;
      long result = INTERNAL_SYSCALL_CALL (mkdirat, AT_FDCWD, buffer, 01777);
      if (result < 0 && result != -EEXIST) return result;
    }
  for (unsigned int i = 0;; ++i)
    {
      if (out == 4095) return -ENAMETOOLONG;
      buffer[out++] = suffix[i];
      if (suffix[i] == 0) break;
    }
  *argument = (long) buffer;
  return 0;
}

/* Resolve links component by component: the kernel cannot translate a guest
   absolute target encountered halfway through a pathname. Keep link contents
   unchanged, and leave the final component alone for no-follow operations.
   Raw calls avoid recursion; this is path compatibility, not confinement. */
static long
follow_links (int directory, long *argument, char output[4096], int final)
{
  const char *source = (const char *) *argument;
  if (!source || !*source) return 0;
  char path[4096], target[4096], mapped[4096];
  size_t length = strnlen (source, sizeof path);
  if (length == sizeof path) return -ENAMETOOLONG;
  memcpy (path, source, length + 1);
  unsigned links = 0;
  for (size_t end = path[0] == '/' ? 1 : 0; path[end]; )
    {
      while (path[end] == '/') ++end;
      if (!path[end]) break;
      size_t begin = end;
      while (path[end] && path[end] != '/') ++end;
      if (!path[end] && !final) break;
      char separator = path[end];
      path[end] = 0;
      long size = INTERNAL_SYSCALL_CALL (readlinkat, directory, path, target, 4095);
      int magic = 0;
      if (size >= 0)
        {
          /* procfs links are kernel handles, not pathname substitutions:
             pipe:[N], anon_inode:[N] and deleted files must keep their fd. */
          long fd = INTERNAL_SYSCALL_CALL (openat, directory, path,
                                          O_PATH | O_NOFOLLOW | O_CLOEXEC, 0);
          if (fd >= 0)
            {
              struct statfs fs;
              magic = INTERNAL_SYSCALL_CALL (fstatfs, fd, &fs) == 0 &&
                      fs.f_type == PROC_SUPER_MAGIC;
              INTERNAL_SYSCALL_CALL (close, fd);
            }
        }
      path[end] = separator;
      if (magic) continue;
      if (size == -EINVAL) continue; /* Ordinary directory or file. */
      if (size < 0) break; /* Let the actual operation report its own error. */
      if (size >= 4095) return -ENAMETOOLONG;
      target[size] = 0;
      long translated = (long) target;
      if (target[0] == '/')
        {
          long result = translate (&translated, mapped);
          if (result < 0) return result;
          /* Keep native absolute/magic links under kernel control. */
          if (translated == (long) target) continue;
          begin = 0;
        }
      if (++links > 40) return -ELOOP;
      size_t replacement = strlen ((char *) translated);
      size_t rest = strlen (path + end);
      if (begin + replacement + rest >= sizeof path) return -ENAMETOOLONG;
      memmove (path + begin + replacement, path + end, rest + 1);
      memcpy (path + begin, (char *) translated, replacement);
      end = path[0] == '/' ? 1 : 0;
    }
  strcpy (output, path);
  *argument = (long) output;
  return 0;
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
  result = follow_links (first && number != 27 ? arguments[first - 1] : AT_FDCWD,
                         &arguments[first], storage, final);
  if (result < 0 || second < 0) return result;
  result = translate (&arguments[second], storage + 4096);
  if (result < 0) return result;
  return follow_links (arguments[second - 1], &arguments[second], storage + 4096, 0);
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
