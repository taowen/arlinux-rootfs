/* Application-UID namespace compatibility, not kernel isolation. The Android
   UID and SELinux domain remain the security boundary. Keep process state here,
   and route public, hidden and syscall entry points through the same helpers. */
#define ARLINUX_RAW_SYSCALL 1
#include <sysdep.h>
#include <android-syscall.h>
#include <fcntl.h>
#include <linux/capability.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define NS_FLAGS (CLONE_NEWUSER | CLONE_NEWPID | CLONE_NEWNET)
struct namespace_state {
  unsigned version, active, user;
  int init;
  struct __user_cap_data_struct capabilities[2];
  char maps[3][128];
};
static struct namespace_state state = {.version = 1};
static char *filesystem;
static int mutex;
struct map_descriptor { unsigned long device, inode; int kind, mode; };
static struct map_descriptor descriptors[32];

static void lock (void) {
  while (__atomic_exchange_n (&mutex, 1, __ATOMIC_ACQUIRE))
    INTERNAL_SYSCALL_CALL (sched_yield);
}
static void unlock (void) { __atomic_store_n (&mutex, 0, __ATOMIC_RELEASE); }
static long host_pid (void) { return INTERNAL_SYSCALL_CALL (getpid); }
static long guest_pid (long pid) { return pid > 0 && pid == state.init ? 1 : pid; }
static long actual_pid (long pid) { return pid == 1 && state.init ? state.init : pid; }

static char *decimal (char *out, unsigned long number) {
  char digits[24]; unsigned count = 0;
  do { digits[count++] = '0' + number % 10; number /= 10; } while (number);
  while (count) *out++ = digits[--count];
  *out = 0;
  return out;
}

static long copy_filesystem (void) {
  long result = INTERNAL_SYSCALL_CALL (mmap, NULL, 4096, PROT_READ | PROT_WRITE,
                                      MAP_ANONYMOUS | MAP_SHARED, -1, 0);
  if (INTERNAL_SYSCALL_ERROR_P (result)) return result;
  char *copy = (void *) result;
  if (filesystem) {
    memcpy (copy, filesystem, 4096);
    INTERNAL_SYSCALL_CALL (munmap, filesystem, 4096);
  }
  filesystem = copy;
  return 0;
}

static void new_user (void) {
  state.active = state.user = 1;
  memset (state.maps, 0, sizeof state.maps);
  memset (descriptors, 0, sizeof descriptors);
  strcpy (state.maps[2], "allow\n");
  uint64_t all = (UINT64_C (1) << (CAP_LAST_CAP + 1)) - 1;
  memset (state.capabilities, 0, sizeof state.capabilities);
  state.capabilities[0].effective = state.capabilities[0].permitted = all;
  state.capabilities[1].effective = state.capabilities[1].permitted = all >> 32;
}

static void child_state (unsigned flags) {
  if (!(flags & CLONE_FS) && filesystem && copy_filesystem () < 0)
    INTERNAL_SYSCALL_CALL (exit_group, 125);
  if (flags & CLONE_NEWPID) state.init = host_pid ();
  if (flags & CLONE_NEWUSER) new_user ();
  if (flags & NS_FLAGS) state.active = 1;
}

/* clone's stack switch remains in upstream assembly. A non-CLONE_VM child can
   safely read this context from its private copy of the parent's stack. */
extern int __arlinux_kernel_clone (int (*) (void *), void *, int, void *, ...)
  attribute_hidden;
struct callback { int (*function) (void *); void *argument; unsigned flags; };
static int cloned (void *opaque) {
  struct callback call = *(struct callback *) opaque;
  child_state (call.flags);
  unlock ();
  return call.function (call.argument);
}
int __clone (int (*function) (void *), void *stack, int flags, void *argument, ...) {
  __gnuc_va_list ap;
  __builtin_va_start (ap, argument);
  void *ptid = __builtin_va_arg (ap, void *);
  void *tls = __builtin_va_arg (ap, void *);
  void *ctid = __builtin_va_arg (ap, void *);
  __builtin_va_end (ap);
  if ((flags & NS_FLAGS) && (flags & (CLONE_VM | CLONE_THREAD)))
    { __set_errno (ENOTSUP); return -1; }
  if ((flags & CLONE_NEWUSER) && (flags & CLONE_FS))
    { __set_errno (EINVAL); return -1; }
  if (!function || !stack) { __set_errno (EINVAL); return -1; }
  if (flags & CLONE_VM)
    return __arlinux_kernel_clone (function, stack, flags, argument, ptid, tls, ctid);
  struct callback call = {function, argument, flags};
  lock ();
  if (flags & NS_FLAGS) state.active = 1;
  int result = __arlinux_kernel_clone (cloned, stack, flags & ~NS_FLAGS, &call,
                                      ptid, tls, ctid);
  unlock ();
  return result;
}
libc_hidden_def (__clone)
weak_alias (__clone, clone)

/* Exec carries only live process state, not open descriptor snapshots. */
void __arlinux_namespace_init (const char *encoded) {
  if (!encoded) return;
  unsigned char decoded[sizeof state + 4096];
  size_t length = strnlen (encoded, sizeof decoded * 2 + 1);
  if ((length & 1) || length / 2 <= sizeof state || length / 2 > sizeof decoded) return;
  for (size_t i = 0; i < length; i += 2) {
    unsigned value = 0;
    for (size_t j = 0; j < 2; ++j) {
      unsigned char c = encoded[i + j];
      if (c >= '0' && c <= '9') value = value * 16 + c - '0';
      else if (c >= 'a' && c <= 'f') value = value * 16 + c - 'a' + 10;
      else return;
    }
    decoded[i / 2] = value;
  }
  struct namespace_state restored;
  memcpy (&restored, decoded, sizeof restored);
  if (restored.version != 1 || restored.init < 0 || decoded[length / 2 - 1]) return;
  for (unsigned i = 0; i < 3; ++i)
    if (!memchr (restored.maps[i], 0, sizeof restored.maps[i])) return;
  if (copy_filesystem () < 0) INTERNAL_SYSCALL_CALL (exit_group, 125);
  state = restored;
  memcpy (filesystem, decoded + sizeof state, length / 2 - sizeof state);
}
int __arlinux_namespace_export (char *out, size_t capacity) {
  if (!state.active && (!filesystem || !filesystem[0])) return 0;
  size_t root = filesystem ? strnlen (filesystem, 4096) : 0;
  size_t bytes = sizeof state + root + 1;
  const char prefix[] = "BIONICX_NS_STATE=";
  if (root == 4096 || sizeof prefix + bytes * 2 > capacity) return -E2BIG;
  memcpy (out, prefix, sizeof prefix - 1);
  out += sizeof prefix - 1;
  const char hex[] = "0123456789abcdef";
  for (size_t i = 0; i < bytes; ++i) {
    unsigned char c = i < sizeof state ? ((unsigned char *) &state)[i]
      : filesystem ? filesystem[i - sizeof state] : 0;
    *out++ = hex[c >> 4]; *out++ = hex[c & 15];
  }
  *out = 0;
  return 1;
}

long __arlinux_namespace_path (const char *path, char out[4096]) {
  if (!path || path[0] != '/') return 0;
  if (filesystem && filesystem[0]) {
    size_t length = strnlen (filesystem, 4096);
    if (!strncmp (path, filesystem, length) && (!path[length] || path[length] == '/')) return 0;
    /* Absolute Android backing paths remain usable, as in the FHS boundary.
       This compatibility view is deliberately not a confinement mechanism. */
    if (!strncmp (path, "/data/", 6) || !strncmp (path, "/system/", 8) ||
        !strncmp (path, "/vendor/", 8) || !strncmp (path, "/apex/", 6)) return 0;
    size_t suffix = strnlen (path, 4096);
    if (length + suffix >= 4096) return -ENAMETOOLONG;
    memcpy (out, filesystem, length);
    memcpy (out + length, path, suffix + 1);
    return 1;
  }
  if (state.init && !strncmp (path, "/proc/1", 7) && (!path[7] || path[7] == '/')) {
    strcpy (out, "/proc/");
    char *tail = decimal (out + 6, state.init);
    size_t suffix = strnlen (path + 7, 4096);
    if ((size_t) (tail - out) + suffix >= 4096) return -ENAMETOOLONG;
    memcpy (tail, path + 7, suffix + 1);
    return 1;
  }
  return 0;
}

static long change_root (const char *path) {
  long args[6] = {AT_FDCWD, (long) path}; char translated[8192];
  long result = __arlinux_path_prepare (56, args, translated);
  if (result < 0) return result;
  long fd = INTERNAL_SYSCALL_CALL (openat, AT_FDCWD, args[1], O_PATH | O_DIRECTORY | O_CLOEXEC, 0);
  if (fd < 0) return fd;
  char link[64], target[4096];
  strcpy (link, "/proc/self/fd/"); decimal (link + 14, fd);
  result = INTERNAL_SYSCALL_CALL (readlinkat, AT_FDCWD, link, target, sizeof target - 1);
  INTERNAL_SYSCALL_CALL (close, fd);
  if (result < 0) return result;
  target[result] = 0;
  if (!filesystem && (result = copy_filesystem ()) < 0) return result;
  strcpy (filesystem, target);
  return 0;
}

static int map_kind (int directory, const char *path) {
  if (!state.user || !path || (filesystem && filesystem[0])) return -1;
  char absolute[4096];
  if (path[0] != '/' && directory != AT_FDCWD) {
    char descriptor[64]; strcpy (descriptor, "/proc/self/fd/");
    decimal (descriptor + 14, directory);
    long count = INTERNAL_SYSCALL_CALL (readlinkat, AT_FDCWD, descriptor, absolute, 4095);
    if (count < 0 || count + strnlen (path, 4096) + 2 > sizeof absolute) return -1;
    absolute[count++] = '/'; strcpy (absolute + count, path); path = absolute;
  }
  char prefix[64]; strcpy (prefix, "/proc/");
  char *tail = decimal (prefix + 6, host_pid ()); *tail++ = '/'; *tail = 0;
  const char *leaf;
  if (!strncmp (path, "/proc/self/", 11)) leaf = path + 11;
  else if (!strncmp (path, prefix, tail - prefix)) leaf = path + (tail - prefix);
  else if (state.init == host_pid () && !strncmp (path, "/proc/1/", 8)) leaf = path + 8;
  else return -1;
  return !strcmp (leaf, "uid_map") ? 0 : !strcmp (leaf, "gid_map") ? 1
    : !strcmp (leaf, "setgroups") ? 2 : -1;
}

static long open_map (int kind, int flags) {
  if (flags & (O_CREAT | O_EXCL | O_DIRECTORY | O_PATH)) return -EINVAL;
  /* Inode identity, rather than fd number, makes dup and fd reuse safe. */
  unsigned slot;
  for (slot = 0; slot < 32 && descriptors[slot].inode; ++slot) {}
  if (slot == 32) {
    unsigned char alive[32] = {0};
    long directory = INTERNAL_SYSCALL_CALL (openat, AT_FDCWD, "/proc/self/fd", O_RDONLY | O_DIRECTORY | O_CLOEXEC, 0);
    if (directory < 0) return directory;
    struct directory_entry {
      uint64_t inode; int64_t offset; unsigned short size; unsigned char type; char name[];
    };
    char buffer[4096]; long count;
    while ((count = INTERNAL_SYSCALL_CALL (getdents64, directory, buffer, sizeof buffer)) > 0)
      for (long offset = 0; offset < count; ) {
        struct directory_entry *entry = (void *) (buffer + offset);
        if (entry->size < 20 || offset + entry->size > count) { count = -EIO; break; }
        unsigned long fd = 0; const char *name = entry->name;
        while (*name >= '0' && *name <= '9') fd = fd * 10 + *name++ - '0';
        struct stat st;
        if (name != entry->name && !*name && fd <= INT32_MAX &&
            INTERNAL_SYSCALL_CALL (fstat, fd, &st) == 0)
          for (unsigned i = 0; i < 32; ++i)
            if (descriptors[i].inode == st.st_ino && descriptors[i].device == st.st_dev) alive[i] = 1;
        offset += entry->size;
      }
    INTERNAL_SYSCALL_CALL (close, directory);
    if (count < 0) return count;
    for (unsigned i = 0; i < 32; ++i)
      if (!alive[i]) memset (&descriptors[i], 0, sizeof descriptors[i]);
    for (slot = 0; slot < 32 && descriptors[slot].inode; ++slot) {}
    if (slot == 32) return -EMFILE;
  }
  long fd = INTERNAL_SYSCALL_CALL (memfd_create, "arlinux-ns-map", (flags & O_CLOEXEC) ? MFD_CLOEXEC : 0);
  if (fd < 0) return fd;
  size_t size = strlen (state.maps[kind]);
  long result = INTERNAL_SYSCALL_CALL (write, fd, state.maps[kind], size);
  if (result == size) result = INTERNAL_SYSCALL_CALL (lseek, fd, 0, SEEK_SET);
  struct stat st;
  if (result >= 0) result = INTERNAL_SYSCALL_CALL (fstat, fd, &st);
  if (result < 0) { INTERNAL_SYSCALL_CALL (close, fd); return result; }
  descriptors[slot] = (struct map_descriptor) {st.st_dev, st.st_ino, kind, flags & O_ACCMODE};
  return fd;
}

static int valid_extent (const char *text) {
  unsigned long values[3];
  for (int i = 0; i < 3; ++i) {
    while (*text == ' ' || *text == '\t' || *text == '\n') ++text;
    if (*text < '0' || *text > '9') return 0;
    unsigned long value = 0;
    while (*text >= '0' && *text <= '9') {
      value = value * 10 + *text++ - '0';
      if (value > UINT32_MAX) return 0;
    }
    values[i] = value;
  }
  while (*text == ' ' || *text == '\t' || *text == '\n') ++text;
  return !*text && values[2] && values[0] + values[2] <= UINT32_MAX &&
         values[1] + values[2] <= UINT32_MAX;
}

/* Return handled/nonhandled separately from the kernel-convention result. */
int __arlinux_namespace_call (long n, long args[6], long *result) {
  long a = args[0], b = args[1], c = args[2];
  /* Stack-switching clone belongs in the assembly-backed clone() API. */
  if (n == 220 && (a & CLONE_VM)) { *result = -ENOTSUP; return 1; }
  if ((n == 48 || n == 439) && b && c == F_OK &&
      (!strcmp ((char *) b, "/proc/self/ns/user") ||
       !strcmp ((char *) b, "/proc/self/ns/pid") ||
       !strcmp ((char *) b, "/proc/self/ns/net"))) {
    state.active = 1; *result = 0; return 1;
  }
  if (n == 220 && !(a & CLONE_VM)) {
    if (b || (a & CLONE_THREAD)) { *result = -ENOTSUP; return 1; }
    if ((a & CLONE_NEWUSER) && (a & CLONE_FS)) { *result = -EINVAL; return 1; }
    lock ();
    if (a & NS_FLAGS) state.active = 1;
    *result = INTERNAL_SYSCALL_CALL (clone, a & ~NS_FLAGS, b, c, args[3], args[4]);
    if (!*result) child_state (a);
    unlock (); return 1;
  }
  if (n == 97) {
    if (a & ~(CLONE_NEWUSER | CLONE_NEWNET | CLONE_FS)) *result = -EINVAL;
    else {
      lock ();
      *result = (a & CLONE_FS) ? copy_filesystem () : 0;
      if (!*result) {
        if (a & CLONE_NEWUSER) new_user ();
        if (a & NS_FLAGS) state.active = 1;
      }
      unlock ();
    }
    return 1;
  }
  if (!state.active) return 0;
  if (n == 172) { *result = guest_pid (host_pid ()); return 1; }
  if (n == 173) {
    *result = state.init == host_pid () ? 0 : guest_pid (INTERNAL_SYSCALL_CALL (getppid));
    return 1;
  }
  if (n == 129 || n == 131 || n == 138 || n == 240 || n == 434) {
    *result = INTERNAL_SYSCALL_NCS_CALL (n, actual_pid (a), b, c, args[3], args[4], args[5]);
    return 1;
  }
  if (n == 51) { lock (); *result = change_root ((char *) a); unlock (); return 1; }
  if (n == 159 && state.user) {
    *result = !strncmp (state.maps[2], "deny", 4) ? -EPERM : a == 0 ? 0 : -ENOTSUP;
    return 1;
  }
  if ((n == 90 || n == 91) && state.user) {
    struct __user_cap_header_struct *header = (void *) a;
    struct __user_cap_data_struct *data = (void *) b;
    *result = -EFAULT;
    if (!header) return 1;
    if (header->version != _LINUX_CAPABILITY_VERSION_1 &&
        header->version != _LINUX_CAPABILITY_VERSION_2 && header->version != _LINUX_CAPABILITY_VERSION_3) {
      header->version = _LINUX_CAPABILITY_VERSION_3; *result = -EINVAL; return 1;
    }
    if (header->pid && header->pid != host_pid () && header->pid != guest_pid (host_pid ()))
      { *result = -ESRCH; return 1; }
    if (!data) return 1;
    unsigned count = header->version == _LINUX_CAPABILITY_VERSION_1 ? 1 : 2;
    lock ();
    if (n == 90) { memcpy (data, state.capabilities, count * sizeof *data); *result = 0; }
    else {
      *result = 0;
      for (unsigned i = 0; i < count; ++i)
        if ((data[i].effective & ~data[i].permitted) ||
            (data[i].permitted & ~state.capabilities[i].permitted) ||
            (data[i].inheritable & ~(state.capabilities[i].inheritable | state.capabilities[i].permitted)))
          *result = -EPERM;
      if (!*result) {
        memcpy (state.capabilities, data, count * sizeof *data);
        if (count == 1) memset (state.capabilities + 1, 0, sizeof *data);
      }
    }
    unlock (); return 1;
  }
  if (n == 56) {
    int kind = map_kind (a, (char *) b);
    if (kind < 0) return 0;
    lock (); *result = open_map (kind, c); unlock (); return 1;
  }
  if ((n == 63 || n == 64) && state.user) {
    struct stat st;
    if (INTERNAL_SYSCALL_CALL (fstat, a, &st) < 0) return 0;
    lock ();
    struct map_descriptor *description = NULL;
    for (unsigned i = 0; i < 32; ++i)
      if (descriptors[i].inode == st.st_ino && descriptors[i].device == st.st_dev)
        { description = descriptors + i; break; }
    if (!description) { unlock (); return 0; }
    int kind = description->kind;
    *result = -EBADF;
    if (n == 63 && description->mode != O_WRONLY)
      *result = INTERNAL_SYSCALL_CALL (read, a, b, c);
    if (n == 64 && description->mode != O_RDONLY) {
      *result = -EINVAL;
      if (!b) *result = -EFAULT;
      else if (c > 0 && c < sizeof state.maps[kind]) {
        char text[128]; memcpy (text, (void *) b, c); text[c] = 0;
        if (strnlen (text, c) == c) {
          if (kind == 2) {
            int deny = !strcmp (text, "deny") || !strcmp (text, "deny\n");
            int allow = !strcmp (text, "allow") || !strcmp (text, "allow\n");
            if (deny || allow) *result = allow && !strncmp (state.maps[2], "deny", 4) ? -EPERM : 0;
          } else if (state.maps[kind][0]) *result = -EPERM;
          else if (valid_extent (text)) *result = 0;
          if (!*result) {
            *result = INTERNAL_SYSCALL_CALL (write, a, b, c);
            if (*result == c) memcpy (state.maps[kind], text, c + 1);
          }
        }
      }
    }
    unlock (); return 1;
  }
  return 0;
}

void __arlinux_namespace_wait_prepare (long n, long args[6]) {
  if (n == 260) args[0] = actual_pid (args[0]);
  if (n == 95 && args[0] == P_PID) args[1] = actual_pid (args[1]);
}
long __arlinux_namespace_wait_finish (long n, long args[6], long result) {
  if (n == 260 && result > 0) return guest_pid (result);
  if (n == 95 && !result && args[2]) {
    siginfo_t *info = (void *) args[2]; info->si_pid = guest_pid (info->si_pid);
  }
  return result;
}
