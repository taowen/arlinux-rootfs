/* Android app-data SELinux may prohibit hard links. Package transactions need
   backup publication even there. This fallback publishes a regular-file copy
   atomically and exclusively; it is not kernel hard-link identity semantics. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <android-syscall.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>

static unsigned long sequence;

static char *
decimal (char *out, unsigned long value)
{
  char digits[24];
  unsigned int count = 0;
  do { digits[count++] = '0' + value % 10; value /= 10; } while (value);
  while (count) *out++ = digits[--count];
  *out = 0;
  return out;
}

long
__arlinux_link_copy (int fromdir, const char *from, int todir,
                     const char *to, int flags)
{
  if (flags & ~AT_SYMLINK_FOLLOW) return -ENOTSUP;
  long input = INTERNAL_SYSCALL_CALL (openat, fromdir, from,
    O_RDONLY | O_CLOEXEC | ((flags & AT_SYMLINK_FOLLOW) ? 0 : O_NOFOLLOW), 0);
  if (input < 0) return input;
  struct stat st;
  long result = INTERNAL_SYSCALL_CALL (fstat, input, &st);
  if (result >= 0 && !S_ISREG (st.st_mode)) result = -EPERM;
  char temporary[4096];
  size_t length = strnlen (to, sizeof temporary);
  if (length + 80 >= sizeof temporary) result = -ENAMETOOLONG;
  long output = -1;
  if (result >= 0)
    {
      memcpy (temporary, to, length);
      for (int attempt = 0; attempt < 8; ++attempt)
        {
          char *out = temporary + length;
          strcpy (out, ".arlinux-link-"); out += strlen (out);
          out = decimal (out, INTERNAL_SYSCALL_CALL (getpid)); *out++ = '-';
          decimal (out, __atomic_add_fetch (&sequence, 1, __ATOMIC_RELAXED));
          output = INTERNAL_SYSCALL_CALL (openat, todir, temporary,
            O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, st.st_mode & 0777);
          if (output != -EEXIST) break;
        }
      if (output < 0) result = output;
    }
  if (result >= 0)
    {
      char data[16384];
      for (;;)
        {
          long count = INTERNAL_SYSCALL_CALL (read, input, data, sizeof data);
          if (count == -EINTR) continue;
          if (count <= 0) { result = count; break; }
          for (long offset = 0; offset < count; )
            {
              long written = INTERNAL_SYSCALL_CALL (write, output, data + offset, count - offset);
              if (written == -EINTR) continue;
              if (written <= 0) { result = written < 0 ? written : -EIO; break; }
              offset += written;
            }
          if (result < 0) break;
        }
      if (result >= 0) result = INTERNAL_SYSCALL_CALL (fchmod, output, st.st_mode & 07777);
      if (result >= 0)
        {
          struct timespec times[2] = {st.st_atim, st.st_mtim};
          result = INTERNAL_SYSCALL_CALL (utimensat, output, NULL, times, 0);
        }
    }
  INTERNAL_SYSCALL_CALL (close, input);
  if (output >= 0)
    {
      long closed = INTERNAL_SYSCALL_CALL (close, output);
      if (result >= 0) result = closed;
      if (result >= 0)
        result = INTERNAL_SYSCALL_NCS_CALL (276, todir, temporary, todir, to, 1);
      if (result < 0) INTERNAL_SYSCALL_CALL (unlinkat, todir, temporary, 0);
    }
  return result;
}
