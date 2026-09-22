#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <paths.h>
#include <sysdep.h>
#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* Android blocks the AArch64 System V IPC syscalls for ordinary app UIDs.
 * Keep one semaphore namespace in the app-private runtime directory.  State
 * is file-backed and shared across fork/exec, while fcntl locks serialize
 * atomic operation sets and a shared futex sequence supplies blocking waits. */

#define BX_SEM_MAGIC 0x4258534du
#define BX_SEM_VERSION 2u
#define BX_SEM_UNDO_PROCESSES 64
#define BX_SEM_MAX 64
#define BX_SEM_ID_BASE 0x42500000

union bx_semun {
    int val;
    struct semid_ds *buf;
    unsigned short *array;
};

struct bx_sem_undo {
    pid_t pid;
    unsigned long long birth;
    int adjustments[BX_SEM_MAX];
};

struct bx_sem_state {
    uint32_t magic;
    uint32_t version;
    int id;
    key_t key;
    unsigned int nsems;
    unsigned int removed;
    _Atomic unsigned int sequence;
    int values[BX_SEM_MAX];
    pid_t last_pid[BX_SEM_MAX];
    unsigned int wait_negative[BX_SEM_MAX];
    unsigned int wait_zero[BX_SEM_MAX];
    struct bx_sem_undo undo[BX_SEM_UNDO_PROCESSES];
};

struct bx_mapping {
    int fd;
    struct bx_sem_state *state;
    char id_path[PATH_MAX];
    char key_path[PATH_MAX];
};

static int make_namespace(char *directory, size_t capacity) {
    const char *temporary = getenv("BIONICX_TMPDIR");
    if (temporary == NULL || temporary[0] != '/') {
        temporary = _PATH_TMP;
    }
    if (snprintf(directory, capacity, "%s/sysv-sem-v2", temporary) >=
            (int)capacity) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return -1;
    return 0;
}

static int make_paths(int id, key_t key, char *id_path, char *key_path) {
    char directory[PATH_MAX];
    if (make_namespace(directory, sizeof(directory)) != 0) return -1;
    if (snprintf(id_path, PATH_MAX, "%s/id-%08x", directory,
                 (unsigned int)id) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (key_path != NULL && snprintf(key_path, PATH_MAX, "%s/key-%08x",
            directory, (unsigned int)key) >= PATH_MAX) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

static int lock_file(int descriptor, short type) {
    struct flock lock = {.l_type = type, .l_whence = SEEK_SET};
    int command = type == F_UNLCK ? F_OFD_SETLK : F_OFD_SETLKW;
    while (fcntl(descriptor, command, &lock) != 0) {
        if (errno != EINTR) return -1;
    }
    return 0;
}

static void close_mapping(struct bx_mapping *mapping) {
    if (mapping->state != MAP_FAILED)
        (void)munmap(mapping->state, sizeof(*mapping->state));
    if (mapping->fd >= 0) (void)close(mapping->fd);
}

static int map_descriptor(struct bx_mapping *mapping, int descriptor) {
    mapping->fd = descriptor;
    mapping->state = mmap(NULL, sizeof(*mapping->state),
            PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
    if (mapping->state == MAP_FAILED) {
        int saved = errno;
        close(descriptor);
        mapping->fd = -1;
        errno = saved;
        return -1;
    }
    return 0;
}

static int open_id(int id, struct bx_mapping *mapping) {
    memset(mapping, 0, sizeof(*mapping));
    mapping->fd = -1;
    mapping->state = MAP_FAILED;
    if (make_paths(id, 0, mapping->id_path, NULL) != 0) return -1;
    int descriptor = open(mapping->id_path, O_RDWR | O_CLOEXEC);
    if (descriptor < 0 || map_descriptor(mapping, descriptor) != 0) return -1;
    if (mapping->state->magic != BX_SEM_MAGIC ||
            mapping->state->version != BX_SEM_VERSION ||
            mapping->state->id != id || mapping->state->removed) {
        close_mapping(mapping);
        errno = EINVAL;
        return -1;
    }
    return 0;
}

static int allocate_id(void) {
    struct timespec time;
    (void)clock_gettime(CLOCK_MONOTONIC, &time);
    uint32_t seed = (uint32_t)time.tv_nsec ^ (uint32_t)INTERNAL_SYSCALL_CALL(getpid) * 2654435761u;
    return BX_SEM_ID_BASE | (int)(seed & 0x000fffffu);
}

static int create_state(key_t key, int nsems, int *created_id) {
    int candidate = allocate_id();
    for (unsigned int attempt = 0; attempt < 0x100000u; ++attempt) {
        candidate = BX_SEM_ID_BASE | ((candidate + (int)attempt) & 0x000fffff);
        struct bx_mapping mapping;
        memset(&mapping, 0, sizeof(mapping));
        mapping.fd = -1;
        mapping.state = MAP_FAILED;
        if (make_paths(candidate, key, mapping.id_path, mapping.key_path) != 0)
            return -1;
        int descriptor = open(mapping.id_path,
                O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            if (errno == EEXIST) continue;
            return -1;
        }
        if (ftruncate(descriptor, sizeof(struct bx_sem_state)) != 0 ||
                map_descriptor(&mapping, descriptor) != 0) {
            int saved = errno;
            (void)unlink(mapping.id_path);
            errno = saved;
            return -1;
        }
        memset(mapping.state, 0, sizeof(*mapping.state));
        mapping.state->version = BX_SEM_VERSION;
        mapping.state->id = candidate;
        mapping.state->key = key;
        mapping.state->nsems = (unsigned int)nsems;
        atomic_store_explicit(&mapping.state->sequence, 1,
                              memory_order_relaxed);
        atomic_thread_fence(memory_order_release);
        mapping.state->magic = BX_SEM_MAGIC;
        if (msync(mapping.state, sizeof(*mapping.state), MS_SYNC) != 0) {
            int saved = errno;
            close_mapping(&mapping);
            (void)unlink(mapping.id_path);
            errno = saved;
            return -1;
        }
        close_mapping(&mapping);
        *created_id = candidate;
        return 0;
    }
    errno = ENOSPC;
    return -1;
}

int semget(key_t key, int nsems, int semflg) {
    if (nsems < 0 || nsems > BX_SEM_MAX || (key == IPC_PRIVATE && nsems == 0)) {
        errno = EINVAL;
        return -1;
    }
    char directory[PATH_MAX], key_path[PATH_MAX];
    if (make_namespace(directory, sizeof(directory)) != 0) return -1;
    if (key != IPC_PRIVATE) {
        if (snprintf(key_path, sizeof(key_path), "%s/key-%08x", directory,
                     (unsigned int)key) >= (int)sizeof(key_path)) {
            errno = ENAMETOOLONG;
            return -1;
        }
        int descriptor = open(key_path, O_RDWR | O_CLOEXEC);
        if (descriptor >= 0) {
            struct bx_mapping mapping;
            if (map_descriptor(&mapping, descriptor) != 0) return -1;
            int id = mapping.state->id;
            unsigned int count = mapping.state->nsems;
            int valid = mapping.state->magic == BX_SEM_MAGIC &&
                    !mapping.state->removed;
            close_mapping(&mapping);
            if (!valid) { errno = EINVAL; return -1; }
            if ((semflg & IPC_CREAT) && (semflg & IPC_EXCL)) {
                errno = EEXIST;
                return -1;
            }
            if (nsems != 0 && (unsigned int)nsems > count) {
                errno = EINVAL;
                return -1;
            }
            return id;
        }
        if (errno != ENOENT) return -1;
        if (!(semflg & IPC_CREAT)) return -1;
    }

    int id;
    if (create_state(key, nsems, &id) != 0) return -1;
    if (key == IPC_PRIVATE) return id;
    char id_path[PATH_MAX];
    if (make_paths(id, key, id_path, key_path) != 0) return -1;
    if (link(id_path, key_path) == 0) return id;
    int saved = errno;
    (void)unlink(id_path);
    if (saved == EEXIST) return semget(key, nsems, semflg);
    errno = saved;
    return -1;
}

static int wake_waiters(struct bx_sem_state *state) {
    atomic_fetch_add_explicit(&state->sequence, 1, memory_order_release);
#ifdef SYS_futex
    return (int)INLINE_SYSCALL_CALL(futex, &state->sequence, 1 /* FUTEX_WAKE */, INT_MAX,
                        NULL, NULL, 0);
#else
    return 0;
#endif
}

/* Process identity survives exec, differs after fork, and includes start time
 * so a reused PID cannot inherit another process's undo adjustments. Zombies
 * have already exited and must release their adjustments before waitpid. */
static int process_identity(pid_t pid, unsigned long long *birth) {
    char path[64], buffer[2048];
    snprintf(path, sizeof(path), "/proc/%d/stat", pid);
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT || errno == ESRCH ? 0 : -1;
    ssize_t length = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (length <= 0) return -1;
    buffer[length] = '\0';
    char *fields = strrchr(buffer, ')');
    if (fields == NULL || fields[1] != ' ') return -1;
    fields += 2;
    if (*fields == 'Z' || *fields == 'X') return 0;
    for (int field = 3; field < 22; ++field) {
        fields = strchr(fields, ' ');
        if (fields == NULL) return -1;
        ++fields;
    }
    *birth = strtoull(fields, NULL, 10);
    return 1;
}

static void reap_undo(struct bx_sem_state *state) {
    int changed = 0, saved = errno;
    for (int i = 0; i < BX_SEM_UNDO_PROCESSES; ++i) {
        struct bx_sem_undo *undo = &state->undo[i];
        if (undo->pid == 0) continue;
        unsigned long long birth = 0;
        int alive = process_identity(undo->pid, &birth);
        if (alive < 0 || (alive == 1 && birth == undo->birth)) continue;
        for (unsigned int n = 0; n < state->nsems; ++n) {
            if (!undo->adjustments[n]) continue;
            int value = state->values[n] + undo->adjustments[n];
            state->values[n] = value < 0 ? 0 : value > 32767 ? 32767 : value;
            state->last_pid[n] = undo->pid;
            changed = 1;
        }
        memset(undo, 0, sizeof(*undo));
    }
    if (changed) (void)wake_waiters(state);
    errno = saved;
}

static int undo_slot(struct bx_sem_state *state, pid_t pid,
                      unsigned long long birth) {
    int available = -1;
    for (int i = 0; i < BX_SEM_UNDO_PROCESSES; ++i) {
        if (state->undo[i].pid == pid && state->undo[i].birth == birth) return i;
        int used = 0;
        for (unsigned int n = 0; n < state->nsems; ++n)
            used |= state->undo[i].adjustments[n] != 0;
        if (!used) available = i;
    }
    return available;
}

int semctl(int semid, int semnum, int command, ...) {
    struct bx_mapping mapping;
    if (open_id(semid, &mapping) != 0) return -1;
    union bx_semun argument = {0};
    if (command == SETVAL || command == SETALL || command == GETALL ||
            command == IPC_STAT || command == IPC_SET) {
        va_list arguments;
        va_start(arguments, command);
        argument = va_arg(arguments, union bx_semun);
        va_end(arguments);
    }
    if (lock_file(mapping.fd, F_WRLCK) != 0) {
        close_mapping(&mapping);
        return -1;
    }
    struct bx_sem_state *state = mapping.state;
    reap_undo(state);
    int result = 0;
    if (command != IPC_RMID && command != IPC_STAT && command != IPC_SET &&
            (semnum < 0 || (unsigned int)semnum >= state->nsems)) {
        errno = EINVAL;
        result = -1;
    } else switch (command) {
        case GETVAL: result = state->values[semnum]; break;
        case GETPID: result = state->last_pid[semnum]; break;
        case GETNCNT: result = (int)state->wait_negative[semnum]; break;
        case GETZCNT: result = (int)state->wait_zero[semnum]; break;
        case SETVAL:
            if (argument.val < 0 || argument.val > 32767) {
                errno = ERANGE; result = -1; break;
            }
            for (int i = 0; i < BX_SEM_UNDO_PROCESSES; ++i)
                state->undo[i].adjustments[semnum] = 0;
            state->values[semnum] = argument.val;
            state->last_pid[semnum] = getpid();
            (void)wake_waiters(state);
            break;
        case GETALL:
            if (argument.array == NULL) { errno = EFAULT; result = -1; break; }
            for (unsigned int i = 0; i < state->nsems; ++i)
                argument.array[i] = (unsigned short)state->values[i];
            break;
        case SETALL:
            if (argument.array == NULL) { errno = EFAULT; result = -1; break; }
            for (unsigned int i = 0; i < state->nsems; ++i) {
                if (argument.array[i] > 32767) {
                    errno = ERANGE; result = -1; break;
                }
            }
            if (result == 0) {
                memset(state->undo, 0, sizeof(state->undo));
                for (unsigned int i = 0; i < state->nsems; ++i) {
                    state->values[i] = argument.array[i];
                    state->last_pid[i] = getpid();
                }
                (void)wake_waiters(state);
            }
            break;
        case IPC_STAT:
            if (argument.buf == NULL) { errno = EFAULT; result = -1; break; }
            memset(argument.buf, 0, sizeof(*argument.buf));
            argument.buf->sem_perm.uid = geteuid();
            argument.buf->sem_perm.gid = getegid();
            argument.buf->sem_perm.cuid = geteuid();
            argument.buf->sem_perm.cgid = getegid();
            argument.buf->sem_perm.mode = 0600;
            argument.buf->sem_nsems = state->nsems;
            break;
        case IPC_SET: break; /* one Android UID owns the namespace */
        case IPC_RMID:
            state->removed = 1;
            (void)wake_waiters(state);
            if (state->key != IPC_PRIVATE)
                (void)make_paths(semid, state->key, mapping.id_path,
                                 mapping.key_path);
            (void)unlink(mapping.id_path);
            if (state->key != IPC_PRIVATE) (void)unlink(mapping.key_path);
            break;
        default: errno = EINVAL; result = -1; break;
    }
    (void)lock_file(mapping.fd, F_UNLCK);
    close_mapping(&mapping);
    return result;
}

static int apply_operations(int semid, struct sembuf *operations, size_t count,
                            const struct timespec *timeout) {
    if (operations == NULL || count == 0) { errno = EINVAL; return -1; }
    if (timeout && (timeout->tv_sec < 0 || timeout->tv_nsec < 0 ||
                    timeout->tv_nsec >= 1000000000L)) { errno = EINVAL; return -1; }
    struct timespec deadline = {0};
    if (timeout) {
        clock_gettime(CLOCK_MONOTONIC, &deadline);
        deadline.tv_sec += timeout->tv_sec;
        deadline.tv_nsec += timeout->tv_nsec;
        if (deadline.tv_nsec >= 1000000000L) {
            ++deadline.tv_sec;
            deadline.tv_nsec -= 1000000000L;
        }
    }
    pid_t pid = INTERNAL_SYSCALL_CALL(getpid);
    unsigned long long birth = 0;
    int needs_undo = 0;
    for (size_t i = 0; i < count; ++i)
        needs_undo |= (operations[i].sem_flg & SEM_UNDO) && operations[i].sem_op;
    if (needs_undo && process_identity(pid, &birth) != 1) { errno = EIO; return -1; }
    struct bx_mapping mapping;
    if (open_id(semid, &mapping) != 0) return -1;
    for (;;) {
        if (lock_file(mapping.fd, F_WRLCK) != 0) break;
        struct bx_sem_state *state = mapping.state;
        reap_undo(state);
        int possible = !state->removed;
        int values[BX_SEM_MAX], adjustments[BX_SEM_MAX] = {0};
        memcpy(values, state->values, sizeof(values));
        int slot = needs_undo ? undo_slot(state, pid, birth) : -1;
        if (needs_undo && slot < 0) { possible = -1; errno = ENOSPC; }
        if (slot >= 0 && state->undo[slot].pid == pid && state->undo[slot].birth == birth)
            memcpy(adjustments, state->undo[slot].adjustments, sizeof(adjustments));
        size_t blocked = count;
        for (size_t i = 0; possible == 1 && i < count; ++i) {
            unsigned int number = operations[i].sem_num;
            if (number >= state->nsems) {
                errno = EFBIG; possible = -1; break;
            }
            int operation = operations[i].sem_op;
            if (operation < 0 && values[number] < -operation) {
                blocked = i; possible = 0;
            } else if (operation == 0 && values[number] != 0) {
                blocked = i; possible = 0;
            } else {
                values[number] += operation;
                if (operations[i].sem_flg & SEM_UNDO) adjustments[number] -= operation;
                if (values[number] > 32767 || adjustments[number] > 32767 ||
                        adjustments[number] < -32767) {
                    errno = ERANGE; possible = -1; break;
                }
            }
        }
        if (possible == 1) {
            memcpy(state->values, values, sizeof(values));
            if (slot >= 0) {
                state->undo[slot].pid = pid;
                state->undo[slot].birth = birth;
                memcpy(state->undo[slot].adjustments, adjustments, sizeof(adjustments));
            }
            for (size_t i = 0; i < count; ++i)
                state->last_pid[operations[i].sem_num] = getpid();
            (void)wake_waiters(state);
            (void)lock_file(mapping.fd, F_UNLCK);
            close_mapping(&mapping);
            return 0;
        }
        if (possible < 0 || state->removed) {
            if (state->removed) errno = EIDRM;
            (void)lock_file(mapping.fd, F_UNLCK);
            break;
        }
        if (operations[blocked].sem_flg & IPC_NOWAIT) {
            errno = EAGAIN;
            (void)lock_file(mapping.fd, F_UNLCK);
            break;
        }
        /* Kernel IPC normally applies undo on exit. Poll bounded waits so
         * SIGKILL and _exit are observed even without a userspace destructor. */
        struct timespec interval = {0, 100000000L};
        if (timeout) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            time_t seconds = deadline.tv_sec - now.tv_sec;
            long nanos = deadline.tv_nsec - now.tv_nsec;
            if (nanos < 0) { --seconds; nanos += 1000000000L; }
            if (seconds < 0 || (seconds == 0 && nanos == 0)) {
                errno = EAGAIN;
                (void)lock_file(mapping.fd, F_UNLCK);
                break;
            }
            if (seconds == 0 && nanos < interval.tv_nsec) interval.tv_nsec = nanos;
        }
        unsigned int number = operations[blocked].sem_num;
        unsigned int expected = atomic_load_explicit(&state->sequence,
                                                      memory_order_acquire);
        if (operations[blocked].sem_op == 0) ++state->wait_zero[number];
        else ++state->wait_negative[number];
        (void)lock_file(mapping.fd, F_UNLCK);
        int wait_result = (int)INLINE_SYSCALL_CALL(futex, &state->sequence,
                0 /* FUTEX_WAIT */, expected, &interval, NULL, 0);
        int saved = errno;
        if (lock_file(mapping.fd, F_WRLCK) != 0) break;
        if (operations[blocked].sem_op == 0) --state->wait_zero[number];
        else --state->wait_negative[number];
        (void)lock_file(mapping.fd, F_UNLCK);
        if (wait_result != 0 && saved != EAGAIN && saved != ETIMEDOUT) {
            errno = saved;
            break;
        }
    }
    int saved = errno;
    close_mapping(&mapping);
    errno = saved;
    return -1;
}

int semop(int semid, struct sembuf *operations, size_t count) {
    return apply_operations(semid, operations, count, NULL);
}

int semtimedop(int semid, struct sembuf *operations, size_t count,
               const struct timespec *timeout) {
    return apply_operations(semid, operations, count, timeout);
}
