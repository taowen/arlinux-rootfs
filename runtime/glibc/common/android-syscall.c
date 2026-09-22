/* Android adaptation at the libc syscall boundary. No heap allocation,
   environment lookup, loader lookup, or public libc calls are needed here. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <android-syscall.h>
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <stdlib.h>
#include <sys/types.h>

/* The execution profile is immutable for a process. Keep it after clearenv;
   nested exec receives the profile from the process-launch policy. This never
   changes the Android UID or grants kernel privileges. */
static int virtual_root;
static void __attribute__ ((constructor))
initialize_identity (void)
{
  const char *value = getenv ("BIONICX_VIRTUAL_ROOT");
  virtual_root = value != 0 && value[0] != 0
                 && !(value[0] == '0' && value[1] == 0);
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
  if (result < 0 || second < 0) return result;
  return translate (&arguments[second], storage + 4096);
}

/* Private platform entry point while higher-level policy is being moved out
   of interposition. It shares the exact translator used by hidden libc calls. */
const char *
__arlinux_root_path (const char *path, char buffer[4096])
{
  long argument = (long) path;
  long result = translate (&argument, buffer);
  if (result < 0) { __set_errno (-result); return 0; }
  return (const char *) argument;
}

long
__arlinux_android_syscall (long number, long a0, long a1, long a2,
                        long a3, long a4, long a5)
{
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
  long arguments[6] = {a0, a1, a2, a3, a4, a5};
  char storage[8192];
  long result = __arlinux_path_prepare (number, arguments, storage);
  if (result < 0) return result;
  return INTERNAL_SYSCALL_NCS_CALL (number, arguments[0], arguments[1],
                                   arguments[2], arguments[3], arguments[4], arguments[5]);
}
