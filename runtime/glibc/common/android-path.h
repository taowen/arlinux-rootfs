/* Shared loader/libc pathname semantics. This header must use raw syscalls:
   the dynamic loader runs before libc initialization. */
#ifndef ARLINUX_ANDROID_PATH_H
#define ARLINUX_ANDROID_PATH_H
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <string.h>
#include <sys/vfs.h>
#include <linux/magic.h>

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
android_root_path (long *argument, char *buffer)
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

/* Resolve links component by component: the kernel cannot translate a guest
   absolute target encountered halfway through a pathname. Keep link contents
   unchanged, and leave the final component alone for no-follow operations.
   Raw calls avoid recursion; this is path compatibility, not confinement. */
static long
android_follow_links (int directory, long *argument, char output[4096], int final,
                      long (*translate) (long *, char *))
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

#endif
