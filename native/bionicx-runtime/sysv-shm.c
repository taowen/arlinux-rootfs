#define _GNU_SOURCE
#include "runtime-internal.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <unistd.h>

/* Android blocks AArch64 System V SHM syscalls for ordinary app UIDs.
 * Chrome Ozone software present still uses shmget + MIT-SHM Attach. Serve
 * memfd-backed segments and export the fd over the same abstract-socket
 * protocol Winlator's X server already imports (libandroid-shmem). */

#define BX_SHM_SOCKNAME "/dev/shm/%08x"
#define BX_SHM_MAX_SEGMENTS 64
#define BX_SHM_MAX_BYTES (64u * 1024u * 1024u)

struct bx_shm_segment {
    int id;
    int fd;
    size_t size;
    void *addr;
    int mapped;
    int marked_for_deletion;
    key_t key;
};

static pthread_mutex_t bx_shm_lock = PTHREAD_MUTEX_INITIALIZER;
static struct bx_shm_segment bx_shm_segments[BX_SHM_MAX_SEGMENTS];
static size_t bx_shm_count;
static unsigned int bx_shm_counter;
static int bx_shm_socket_id;
static int bx_shm_listen_fd = -1;
static pthread_t bx_shm_thread;

static int shmid_from_counter(unsigned int counter) {
    return (int)((unsigned int)bx_shm_socket_id * 0x10000u + (counter & 0x7fffu));
}

static int find_local_index(int shmid) {
    for (size_t i = 0; i < bx_shm_count; ++i) {
        if (bx_shm_segments[i].id == shmid) return (int)i;
    }
    return -1;
}

static void delete_index(size_t index) {
    if (bx_shm_segments[index].addr != NULL) {
        munmap(bx_shm_segments[index].addr, bx_shm_segments[index].size);
        bx_shm_segments[index].addr = NULL;
        bx_shm_segments[index].mapped = 0;
    }
    if (bx_shm_segments[index].fd >= 0) {
        close(bx_shm_segments[index].fd);
        bx_shm_segments[index].fd = -1;
    }
    if (index + 1 < bx_shm_count) {
        memmove(&bx_shm_segments[index], &bx_shm_segments[index + 1],
                (bx_shm_count - index - 1) * sizeof(bx_shm_segments[0]));
    }
    --bx_shm_count;
}

static int create_memory_fd(size_t size) {
    int fd = -1;
#ifdef SYS_memfd_create
    fd = (int)syscall(SYS_memfd_create, "bionicx-sysvshm", 0u);
    if (fd >= 0) {
        if (ftruncate(fd, (off_t)size) == 0) return fd;
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
#endif
    const char *temporary = bionicx_captured_tmpdir();
    if (temporary == NULL || temporary[0] != '/')
        temporary = bionicx_getenv("BIONICX_TMPDIR");
    if (temporary == NULL || temporary[0] != '/') {
        errno = ENOENT;
        return -1;
    }
    char directory[PATH_MAX];
    if (snprintf(directory, sizeof(directory), "%s/sysv-shm", temporary) >=
            (int)sizeof(directory)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    if (mkdir(directory, 0700) != 0 && errno != EEXIST) return -1;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/seg-XXXXXX", directory) >=
            (int)sizeof(path)) {
        errno = ENAMETOOLONG;
        return -1;
    }
    fd = mkstemp(path);
    if (fd < 0) return -1;
    (void)unlink(path);
    if (ftruncate(fd, (off_t)size) != 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    return fd;
}

static int send_segment_fd(int socket_fd, int fd, key_t key) {
    if (write(socket_fd, &key, sizeof(key)) != (ssize_t)sizeof(key)) return -1;
    char payload = '!';
    struct iovec iov = {.iov_base = &payload, .iov_len = 1};
    union {
        struct cmsghdr header;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;
    memset(&control, 0, sizeof(control));
    struct msghdr message = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.bytes,
        .msg_controllen = sizeof(control.bytes),
    };
    struct cmsghdr *header = CMSG_FIRSTHDR(&message);
    if (header == NULL) return -1;
    header->cmsg_level = SOL_SOCKET;
    header->cmsg_type = SCM_RIGHTS;
    header->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(header), &fd, sizeof(fd));
    return sendmsg(socket_fd, &message, 0) == 1 ? 0 : -1;
}

static void *listen_thread(void *argument) {
    int listen_fd = *(int *)argument;
    free(argument);
    for (;;) {
        int client = accept(listen_fd, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR) continue;
            return NULL;
        }
        int shmid = 0;
        if (read(client, &shmid, sizeof(shmid)) != (ssize_t)sizeof(shmid)) {
            close(client);
            continue;
        }
        pthread_mutex_lock(&bx_shm_lock);
        int index = find_local_index(shmid);
        if (index >= 0) {
            (void)send_segment_fd(client, bx_shm_segments[index].fd,
                                  bx_shm_segments[index].key);
        }
        pthread_mutex_unlock(&bx_shm_lock);
        close(client);
    }
}

static int ensure_listener(void) {
    if (bx_shm_listen_fd >= 0) return 0;
    int sock = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (sock < 0) return -1;
    int i;
    for (i = 0; i < 4096; ++i) {
        struct sockaddr_un address;
        memset(&address, 0, sizeof(address));
        address.sun_family = AF_UNIX;
        bx_shm_socket_id = (bionicx_host_pid() + i) & 0xffff;
        int name_length = snprintf(&address.sun_path[1],
                                   sizeof(address.sun_path) - 1,
                                   BX_SHM_SOCKNAME, bx_shm_socket_id);
        if (name_length <= 0) continue;
        socklen_t length = (socklen_t)(offsetof(struct sockaddr_un, sun_path) +
                                       1 + name_length);
        if (bind(sock, (struct sockaddr *)&address, length) == 0) break;
    }
    if (i == 4096) {
        close(sock);
        errno = ENOMEM;
        return -1;
    }
    if (listen(sock, 8) != 0) {
        int saved = errno;
        close(sock);
        errno = saved;
        return -1;
    }
    int *argument = malloc(sizeof(*argument));
    if (argument == NULL) {
        close(sock);
        errno = ENOMEM;
        return -1;
    }
    *argument = sock;
    pthread_attr_t attributes;
    pthread_attr_init(&attributes);
    pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
    int created = pthread_create(&bx_shm_thread, &attributes, listen_thread,
                                 argument);
    pthread_attr_destroy(&attributes);
    if (created != 0) {
        free(argument);
        close(sock);
        errno = created;
        return -1;
    }
    bx_shm_listen_fd = sock;
    return 0;
}

static size_t round_up_page(size_t size) {
    long page = sysconf(_SC_PAGESIZE);
    if (page <= 0) page = 4096;
    return (size + (size_t)page - 1u) & ~((size_t)page - 1u);
}

int shmget(key_t key, size_t size, int flags) {
    (void)flags;
    if (size > BX_SHM_MAX_BYTES) {
        errno = EINVAL;
        return -1;
    }
    pthread_mutex_lock(&bx_shm_lock);
    if (ensure_listener() != 0) {
        pthread_mutex_unlock(&bx_shm_lock);
        return -1;
    }
    if (key != IPC_PRIVATE) {
        for (size_t i = 0; i < bx_shm_count; ++i) {
            if (bx_shm_segments[i].key == key) {
                int id = bx_shm_segments[i].id;
                pthread_mutex_unlock(&bx_shm_lock);
                return id;
            }
        }
    }
    if (bx_shm_count >= BX_SHM_MAX_SEGMENTS) {
        pthread_mutex_unlock(&bx_shm_lock);
        errno = ENOSPC;
        return -1;
    }
    size = round_up_page(size == 0 ? 1 : size);
    int fd = create_memory_fd(size);
    if (fd < 0) {
        pthread_mutex_unlock(&bx_shm_lock);
        return -1;
    }
    bx_shm_counter = (bx_shm_counter + 1u) & 0x7fffu;
    if (bx_shm_counter == 0) bx_shm_counter = 1;
    struct bx_shm_segment *segment = &bx_shm_segments[bx_shm_count];
    memset(segment, 0, sizeof(*segment));
    segment->id = shmid_from_counter(bx_shm_counter);
    segment->fd = fd;
    segment->size = size;
    segment->key = key;
    ++bx_shm_count;
    int id = segment->id;
    pthread_mutex_unlock(&bx_shm_lock);
    return id;
}

void *shmat(int shmid, const void *shmaddr, int shmflg) {
    pthread_mutex_lock(&bx_shm_lock);
    int index = find_local_index(shmid);
    if (index < 0) {
        pthread_mutex_unlock(&bx_shm_lock);
        errno = EINVAL;
        return (void *)-1;
    }
    struct bx_shm_segment *segment = &bx_shm_segments[index];
    if (segment->addr == NULL) {
        int prot = PROT_READ | ((shmflg & SHM_RDONLY) ? 0 : PROT_WRITE);
        int map_flags = MAP_SHARED | (shmaddr != NULL ? MAP_FIXED : 0);
        void *address = mmap((void *)shmaddr, segment->size, prot, map_flags,
                             segment->fd, 0);
        if (address == MAP_FAILED) {
            pthread_mutex_unlock(&bx_shm_lock);
            return (void *)-1;
        }
        segment->addr = address;
        segment->mapped = 1;
    }
    void *address = segment->addr;
    pthread_mutex_unlock(&bx_shm_lock);
    return address;
}

int shmdt(const void *shmaddr) {
    pthread_mutex_lock(&bx_shm_lock);
    for (size_t i = 0; i < bx_shm_count; ++i) {
        if (bx_shm_segments[i].addr != shmaddr) continue;
        if (munmap(bx_shm_segments[i].addr, bx_shm_segments[i].size) != 0) {
            pthread_mutex_unlock(&bx_shm_lock);
            return -1;
        }
        bx_shm_segments[i].addr = NULL;
        bx_shm_segments[i].mapped = 0;
        if (bx_shm_segments[i].marked_for_deletion) delete_index(i);
        pthread_mutex_unlock(&bx_shm_lock);
        return 0;
    }
    pthread_mutex_unlock(&bx_shm_lock);
    return 0;
}

int shmctl(int shmid, int cmd, struct shmid_ds *buf) {
#ifdef IPC_INFO
    if (cmd == IPC_INFO) {
        struct shminfo info;
        memset(&info, 0, sizeof(info));
        info.shmmax = BX_SHM_MAX_BYTES;
        info.shmmin = 1;
        info.shmmni = BX_SHM_MAX_SEGMENTS;
        info.shmseg = BX_SHM_MAX_SEGMENTS;
        info.shmall = BX_SHM_MAX_BYTES / 4096u;
        if (buf != NULL) memcpy(buf, &info, sizeof(info));
        return 0;
    }
#endif
    pthread_mutex_lock(&bx_shm_lock);
    int index = find_local_index(shmid);
    if (index < 0) {
        pthread_mutex_unlock(&bx_shm_lock);
        if (cmd == IPC_RMID) return 0;
        errno = EINVAL;
        return -1;
    }
    if (cmd == IPC_RMID) {
        if (bx_shm_segments[index].mapped)
            bx_shm_segments[index].marked_for_deletion = 1;
        else
            delete_index((size_t)index);
        pthread_mutex_unlock(&bx_shm_lock);
        return 0;
    }
    if (cmd == IPC_STAT) {
        if (buf == NULL) {
            pthread_mutex_unlock(&bx_shm_lock);
            errno = EINVAL;
            return -1;
        }
        memset(buf, 0, sizeof(*buf));
        buf->shm_segsz = bx_shm_segments[index].size;
        buf->shm_nattch = bx_shm_segments[index].mapped ? 1 : 0;
        buf->shm_perm.mode = 0666;
        pthread_mutex_unlock(&bx_shm_lock);
        return 0;
    }
    pthread_mutex_unlock(&bx_shm_lock);
    errno = EINVAL;
    return -1;
}
