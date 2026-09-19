#define _GNU_SOURCE
#include "runtime-internal.h"

#include <pthread.h>
#include <signal.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define FAKE_TIMERFD_MAX 8

struct fake_timerfd {
    int fd;
    int used;
    timer_t timer;
};

static struct fake_timerfd table[FAKE_TIMERFD_MAX];
static pthread_mutex_t table_lock = PTHREAD_MUTEX_INITIALIZER;

static void timerfd_notify(union sigval value)
{
    uint64_t tick = 1;
    (void)write(value.sival_int, &tick, sizeof(tick));
}

static struct fake_timerfd *slot_for_fd(int fd)
{
    int i;

    for (i = 0; i < FAKE_TIMERFD_MAX; i++) {
        if (table[i].used && table[i].fd == fd)
            return &table[i];
    }
    return NULL;
}

BIONICX_INTERNAL int bionicx_timerfd_create(int clockid, int flags)
{
    struct sigevent sev;
    struct fake_timerfd *slot = NULL;
    int eflags = EFD_CLOEXEC;
    int efd;
    int i;

    if (flags & TFD_NONBLOCK)
        eflags |= EFD_NONBLOCK;
    efd = eventfd(0, eflags);
    if (efd < 0)
        return -1;

    memset(&sev, 0, sizeof(sev));
    sev.sigev_notify = SIGEV_THREAD;
    sev.sigev_notify_function = timerfd_notify;
    sev.sigev_value.sival_int = efd;

    pthread_mutex_lock(&table_lock);
    for (i = 0; i < FAKE_TIMERFD_MAX; i++) {
        if (!table[i].used) {
            slot = &table[i];
            break;
        }
    }
    if (slot == NULL) {
        pthread_mutex_unlock(&table_lock);
        close(efd);
        errno = EMFILE;
        return -1;
    }
    if (timer_create(clockid, &sev, &slot->timer) != 0 &&
        timer_create(CLOCK_MONOTONIC, &sev, &slot->timer) != 0) {
        pthread_mutex_unlock(&table_lock);
        close(efd);
        return -1;
    }
    slot->fd = efd;
    slot->used = 1;
    pthread_mutex_unlock(&table_lock);
    return efd;
}

BIONICX_INTERNAL int bionicx_timerfd_settime(int fd, int flags,
                                             const void *new_value,
                                             void *old_value)
{
    struct fake_timerfd *slot;
    int posix_flags = 0;
    int result;

    if (new_value == NULL) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&table_lock);
    slot = slot_for_fd(fd);
    if (slot == NULL) {
        pthread_mutex_unlock(&table_lock);
        errno = EINVAL;
        return -1;
    }
    if (flags & TFD_TIMER_ABSTIME)
        posix_flags |= TIMER_ABSTIME;
    result = timer_settime(slot->timer, posix_flags, new_value, old_value);
    pthread_mutex_unlock(&table_lock);
    return result;
}

BIONICX_INTERNAL int bionicx_timerfd_gettime(int fd, void *curr)
{
    struct fake_timerfd *slot;
    int result;

    if (curr == NULL) {
        errno = EFAULT;
        return -1;
    }
    pthread_mutex_lock(&table_lock);
    slot = slot_for_fd(fd);
    if (slot == NULL) {
        pthread_mutex_unlock(&table_lock);
        errno = EINVAL;
        return -1;
    }
    result = timer_gettime(slot->timer, curr);
    pthread_mutex_unlock(&table_lock);
    return result;
}

int timerfd_create(int clockid, int flags)
{
    return bionicx_timerfd_create(clockid, flags);
}

int timerfd_settime(int fd, int flags, const struct itimerspec *new_value,
                    struct itimerspec *old_value)
{
    return bionicx_timerfd_settime(fd, flags, new_value, old_value);
}

int timerfd_gettime(int fd, struct itimerspec *curr_value)
{
    return bionicx_timerfd_gettime(fd, curr_value);
}
