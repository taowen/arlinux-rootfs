/* Prepare pathname Unix sockets once, for ordinary and cancellable calls.
   Abstract sockets and other address families retain their kernel ABI. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <android-syscall.h>
#include <fcntl.h>
#include <stddef.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>

long
__arlinux_socket_prepare (long arguments[6], void *storage, int *owned)
{
  *owned = -1;
  const struct sockaddr_un *original = (const void *) arguments[1];
  size_t length = arguments[2], offset = offsetof (struct sockaddr_un, sun_path);
  if (!original || length < sizeof original->sun_family || original->sun_family != AF_UNIX)
    return 0;
  if (length <= offset || !original->sun_path[0]) return 0;
  if (length > sizeof *original) return -EINVAL;
  char path[sizeof original->sun_path + 1];
  size_t path_size = strnlen (original->sun_path, length - offset);
  memcpy (path, original->sun_path, path_size);
  path[path_size] = 0;
  long translated[6] = { AT_FDCWD, (long) path };
  char buffer[8192];
  long result = __arlinux_path_prepare (56, translated, buffer);
  if (result < 0) return result;
  const char *actual = (const char *) translated[1];
  if (actual == path) return 0;
  struct sockaddr_un *address = storage;
  memset (address, 0, sizeof *address);
  address->sun_family = AF_UNIX;
  size_t size = strnlen (actual, 4096);
  if (size == 4096) return -ENAMETOOLONG;
  if (size < sizeof address->sun_path)
    memcpy (address->sun_path, actual, size + 1);
  else
    {
      char *slash = strrchr (buffer, '/');
      if (!slash || !slash[1]) return -ENAMETOOLONG;
      *slash = 0;
      long fd = INTERNAL_SYSCALL_CALL (openat, AT_FDCWD, buffer, O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
      if (fd < 0) return fd;
      *owned = fd;
      char digits[24];
      unsigned int count = 0;
      unsigned long value = fd;
      do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
      strcpy (address->sun_path, "/proc/self/fd/");
      char *out = address->sun_path + strlen (address->sun_path);
      while (count) *out++ = digits[--count];
      *out++ = '/';
      size_t leaf = strlen (slash + 1);
      if ((size_t) (out - address->sun_path) + leaf >= sizeof address->sun_path)
        { INTERNAL_SYSCALL_CALL (close, fd); *owned = -1; return -ENAMETOOLONG; }
      memcpy (out, slash + 1, leaf + 1);
      size = strlen (address->sun_path);
    }
  arguments[1] = (long) address;
  arguments[2] = offset + size + 1;
  return 0;
}
