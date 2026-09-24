/* Android exec adaptation shared by public, hidden and syscall entry points.
   Keep execvp, spawn, popen and system in upstream glibc.  This code runs in
   spawn's CLONE_VM child too: no malloc, loader calls or global mutations. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <android-syscall.h>
#include <elf.h>
#include <fcntl.h>
#include <paths.h>
#include <stddef.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* Stack-owned scratch also works in vfork/CLONE_VM children: successful exec
   must not leak a new mapping into the suspended parent's address space.
   spawni reserves this bounded scratch area plus the actual pointer vectors. */
#define VECTOR_LIMIT 131072
struct exec_storage
{
  char path[4096], script[5][4096], line[5][256];
  char loader[4096], libraries[16384], executable[4120], root[4120], identity[32];
  char namespaces[16384];
  char **arguments, **environment;
};

static long
root_path (int directory, const char *path, int flags, char output[4096])
{
  long arguments[6] = {directory, (long) path,
                      (flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0};
  char storage[8192];
  long result = __arlinux_path_prepare (56, arguments, storage);
  if (result < 0) return result;
  const char *actual = (const char *) arguments[1];
  size_t length = strnlen (actual, 4096);
  if (length == 4096) return -ENAMETOOLONG;
  memcpy (output, actual, length + 1);
  return 0;
}

static int
entry (const char *value, const char *name)
{
  size_t length = strlen (name);
  return strncmp (value, name, length) == 0 && value[length] == '=';
}

static void
descriptor_path (char path[64], unsigned long fd)
{
  char digits[24];
  unsigned int count = 0;
  do { digits[count++] = '0' + fd % 10; fd /= 10; } while (fd);
  strcpy (path, "/proc/self/fd/");
  char *out = path + 14;
  while (count) *out++ = digits[--count];
  *out = 0;
}

static long
execute (struct exec_storage *s, int directory, const char *path,
         char *const argv[], char *const envp[], int flags)
{
  if (!path) return -EFAULT;
  if (flags & ~(AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW)) return -EINVAL;
  long error = root_path (directory, path, flags, s->path);
  if (error < 0) return error;
  size_t argc = 0, envc = 0;
  if (argv)
    while (argv[argc])
      {
        if (argc >= VECTOR_LIMIT - 16) return -E2BIG;
        s->arguments[argc] = argv[argc];
        ++argc;
      }
  if (argc == 0) s->arguments[argc++] = (char *) "";
  s->arguments[argc] = NULL;
  if (envp)
    for (size_t i = 0; envp[i]; ++i)
      {
        if (envc >= VECTOR_LIMIT - 8) return -E2BIG;
        /* Executable identity is runtime bookkeeping, never inherited from
           the previous program or copied back from a shell's env output. */
        if (!entry (envp[i], "BIONICX_EXECFN") && !entry (envp[i], "BIONICX_NS_STATE"))
          s->environment[envc++] = envp[i];
      }

  for (unsigned depth = 0; depth < 5; ++depth)
    {
      const char *program = s->path;
      long fd;
      if (!program[0] && (flags & AT_EMPTY_PATH))
        fd = INTERNAL_SYSCALL_CALL (fcntl, directory, F_DUPFD_CLOEXEC, 3);
      else
        fd = INTERNAL_SYSCALL_CALL (openat, directory, program,
                                   O_RDONLY | O_CLOEXEC |
                                   ((flags & AT_SYMLINK_NOFOLLOW) ? O_NOFOLLOW : 0), 0);
      if (fd < 0) return fd;
      struct stat st;
      error = INTERNAL_SYSCALL_CALL (fstat, fd, &st);
      if (error >= 0 && (!S_ISREG (st.st_mode) || !(st.st_mode & 0111)))
        error = -EACCES;
      char link[64];
      descriptor_path (link, fd);
      long length = error < 0 ? error : INTERNAL_SYSCALL_CALL (
        readlinkat, AT_FDCWD, link, s->script[depth], 4095);
      char *line = s->line[depth];
      long bytes = length < 0 ? length : INTERNAL_SYSCALL_CALL (pread64, fd, line, 255, 0);
      int dynamic = 0, android = 0;
      if (bytes >= (long) sizeof (Elf64_Ehdr) && !memcmp (line, ELFMAG, SELFMAG))
        {
          Elf64_Ehdr header;
          memcpy (&header, line, sizeof header);
          if (header.e_ident[EI_CLASS] != ELFCLASS64 || header.e_machine != EM_AARCH64 ||
              header.e_phentsize != sizeof (Elf64_Phdr))
            { INTERNAL_SYSCALL_CALL (close, fd); return -ENOEXEC; }
          for (unsigned i = 0; i < header.e_phnum; ++i)
            {
              Elf64_Phdr phdr;
              long got = INTERNAL_SYSCALL_CALL (pread64, fd, &phdr, sizeof phdr,
                header.e_phoff + (unsigned long) i * sizeof phdr);
              if (got != sizeof phdr)
                { INTERNAL_SYSCALL_CALL (close, fd); return got < 0 ? got : -ENOEXEC; }
              if (phdr.p_type == PT_INTERP)
                {
                  char interpreter[4096];
                  if (!phdr.p_filesz || phdr.p_filesz > sizeof interpreter)
                    { INTERNAL_SYSCALL_CALL (close, fd); return -ENOEXEC; }
                  got = INTERNAL_SYSCALL_CALL (pread64, fd, interpreter,
                                                phdr.p_filesz, phdr.p_offset);
                  if (got != phdr.p_filesz || interpreter[phdr.p_filesz - 1])
                    { INTERNAL_SYSCALL_CALL (close, fd); return got < 0 ? got : -ENOEXEC; }
                  dynamic = 1;
                  android = !strcmp (interpreter, "/system/bin/linker64") ||
                            !strcmp (interpreter, "/apex/com.android.runtime/bin/linker64");
                }
            }
        }
      INTERNAL_SYSCALL_CALL (close, fd);
      if (bytes < 0) return bytes;
      s->script[depth][length] = 0;
      line[bytes] = 0;
      /* Passing a deleted descriptor's former pathname would execute the
         wrong inode. Do not silently reinterpret that operation. */
      if (strstr (s->script[depth], " (deleted)")) return -ENOTSUP;
      if (bytes >= 2 && line[0] == '#' && line[1] == '!')
        {
          if (depth == 4) return -ELOOP;
          char *end = memchr (line, '\n', bytes);
          if (!end)
            {
              if (bytes == 255) return -ENOEXEC;
              end = line + bytes;
            }
          while (end > line && (end[-1] == ' ' || end[-1] == '\t')) --end;
          *end = 0;
          char *interpreter = line + 2;
          while (*interpreter == ' ' || *interpreter == '\t') ++interpreter;
          if (!*interpreter) return -ENOEXEC;
          char *option = interpreter;
          while (*option && *option != ' ' && *option != '\t') ++option;
          if (*option) *option++ = 0;
          while (*option == ' ' || *option == '\t') ++option;
          size_t extra = *option ? 2 : 1;
          if (argc + extra >= VECTOR_LIMIT - 8) return -E2BIG;
          memmove (s->arguments + 1 + extra, s->arguments + 1,
                   argc * sizeof (char *));
          s->arguments[0] = interpreter;
          if (*option) s->arguments[1] = option;
          s->arguments[extra] = s->script[depth];
          argc += extra;
          error = root_path (AT_FDCWD, interpreter, 0, s->path);
          if (error < 0) return error;
          directory = AT_FDCWD;
          flags = 0;
          continue;
        }
      if (bytes < (long) sizeof (Elf64_Ehdr) || memcmp (line, ELFMAG, SELFMAG))
        return -ENOEXEC;
      /* Android executables use their system linker, not this GNU loader. */
      if (android || !dynamic)
        return INTERNAL_SYSCALL_CALL (execve, s->script[depth], s->arguments, envp);

      const char *root = __arlinux_rootfs ();
      if (root == NULL) return -ENOENT;
      size_t root_length = strlen (root);
      if (root_length + sizeof ("/usr/lib/arlinux-platform/ld-linux-aarch64.so.1")
          >= sizeof s->loader) return -ENAMETOOLONG;
      memcpy (s->loader, root, root_length);
      strcpy (s->loader + root_length, "/usr/lib/arlinux-platform/ld-linux-aarch64.so.1");
      /* Keep the ABI implementation present even with an explicitly empty
         envp. Application-private search entries remain in their given order. */
      memcpy (s->libraries, root, root_length);
      strcpy (s->libraries + root_length, "/usr/lib/mesa:");
      for (size_t i = 0; envp && envp[i]; ++i)
        if (entry (envp[i], "LD_LIBRARY_PATH"))
          {
            const char *value = envp[i] + sizeof ("LD_LIBRARY_PATH");
            if (strlen (s->libraries) + strlen (value) + root_length + 40 >= sizeof s->libraries)
              return -E2BIG;
            strcat (s->libraries, value);
            if (*value) strcat (s->libraries, ":");
            break;
          }
      size_t library_length = strlen (s->libraries);
      memcpy (s->libraries + library_length, root, root_length);
      s->libraries[library_length + root_length] = 0;
      strcat (s->libraries, "/usr/lib/arlinux-platform");
      strcpy (s->executable, "BIONICX_EXECFN=");
      strcat (s->executable, s->script[depth]);
      s->environment[envc++] = s->executable;
      long namespace_result = __arlinux_namespace_export (s->namespaces, sizeof s->namespaces);
      if (namespace_result < 0) return namespace_result;
      if (namespace_result) s->environment[envc++] = s->namespaces;
      int has_root = 0, has_identity = 0;
      for (size_t i = 0; i < envc; ++i)
        {
          has_root |= entry (s->environment[i], "BIONICX_ROOTFS");
          has_identity |= entry (s->environment[i], "BIONICX_VIRTUAL_ROOT");
        }
      if (!has_root)
        {
          strcpy (s->root, "BIONICX_ROOTFS=");
          memcpy (s->root + strlen ("BIONICX_ROOTFS="), root, root_length);
          s->root[strlen ("BIONICX_ROOTFS=") + root_length] = 0;
          s->environment[envc++] = s->root;
        }
      if (!has_identity)
        {
          strcpy (s->identity, __arlinux_virtual_root ()
                  ? "BIONICX_VIRTUAL_ROOT=1" : "BIONICX_VIRTUAL_ROOT=0");
          s->environment[envc++] = s->identity;
        }
      s->environment[envc] = NULL;
      memmove (s->arguments + 6, s->arguments + 1, argc * sizeof (char *));
      /* A normal absolute argv[0] still names the guest path. Programs such
         as Python locate their runtime relative to argv[0], before they can
         use our filesystem boundary. Give them the corresponding physical
         path while preserving deliberately chosen argv[0] values. */
      s->arguments[4] = s->arguments[0] && s->arguments[0][0] == '/'
                        && strcmp (s->arguments[0], path) == 0
                        ? s->path : s->arguments[0];
      s->arguments[0] = s->loader;
      s->arguments[1] = (char *) "--library-path";
      s->arguments[2] = s->libraries;
      s->arguments[3] = (char *) "--argv0";
      s->arguments[5] = s->script[depth];
      return INTERNAL_SYSCALL_CALL (execve, s->loader, s->arguments, s->environment);
    }
  return -ELOOP;
}

long
__arlinux_exec (int directory, const char *path, char *const argv[],
                char *const envp[], int flags)
{
  size_t argc = 0, envc = 0;
  while (argv && argv[argc]) { if (++argc >= VECTOR_LIMIT - 16) return -E2BIG; }
  while (envp && envp[envc]) { if (++envc + argc >= VECTOR_LIMIT - 24) return -E2BIG; }
  struct exec_storage storage;
  storage.arguments = __builtin_alloca ((argc + 16) * sizeof (char *));
  storage.environment = __builtin_alloca ((envc + 8) * sizeof (char *));
  return execute (&storage, directory, path, argv, envp, flags);
}
