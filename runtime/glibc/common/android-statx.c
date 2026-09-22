/* Android may block statx while allowing an O_PATH descriptor and fdinfo.
   Augment upstream's fstatat fallback with the real (not st_dev) mount ID. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <android-syscall.h>
#include <fcntl.h>
#include <string.h>

unsigned long long
__arlinux_mount_id (int directory, const char *path, int flags)
{
  long args[6] = { directory, (long) path };
  char translated[8192];
  if (__arlinux_path_prepare (56, args, translated) < 0) return 0;
  int owned = path && path[0];
  long fd = owned ? INTERNAL_SYSCALL_CALL (openat, args[0], args[1],
    O_PATH | O_CLOEXEC | ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0), 0) : directory;
  if (fd < 0) return 0;
  char name[64] = "/proc/self/fdinfo/", digits[24];
  unsigned long value = fd;
  unsigned int count = 0;
  do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
  char *out = name + strlen (name);
  while (count) *out++ = digits[--count];
  *out = 0;
  long info = INTERNAL_SYSCALL_CALL (openat, AT_FDCWD, name, O_RDONLY | O_CLOEXEC, 0);
  unsigned long long id = 0;
  if (info >= 0)
    {
      char data[1024];
      long size = INTERNAL_SYSCALL_CALL (read, info, data, sizeof data - 1);
      if (size > 0)
        {
          data[size] = 0;
          const char *field = strstr (data, "mnt_id:");
          if (field)
            {
              field += 7;
              while (*field == ' ' || *field == '\t') ++field;
              while (*field >= '0' && *field <= '9') id = id * 10 + *field++ - '0';
            }
        }
      INTERNAL_SYSCALL_CALL (close, info);
    }
  if (owned) INTERNAL_SYSCALL_CALL (close, fd);
  return id;
}
