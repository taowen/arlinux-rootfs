#define _GNU_SOURCE
#include "runtime-internal.h"

#include <arpa/inet.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <resolv.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>


static char synthetic_ifaddrs_tag;

struct synthetic_ifaddrs {
    struct ifaddrs addrs[2];
    struct sockaddr_in addr[2];
    struct sockaddr_in netmask[2];
    char lo_name[3];
    char wan_name[5];
};

static int has_non_loopback_ipv4(const struct ifaddrs *ifa) {
    for (; ifa != NULL; ifa = ifa->ifa_next) {
        if (ifa->ifa_addr == NULL || ifa->ifa_addr->sa_family != AF_INET)
            continue;
        if ((ifa->ifa_flags & IFF_LOOPBACK) != 0)
            continue;
        const struct sockaddr_in *in =
                (const struct sockaddr_in *)ifa->ifa_addr;
        if (in->sin_addr.s_addr != htonl(INADDR_LOOPBACK) &&
                in->sin_addr.s_addr != INADDR_ANY)
            return 1;
    }
    return 0;
}

static void discard_ifaddrs(struct ifaddrs *ifa) {
    static void (*next)(struct ifaddrs *);
    if (ifa == NULL) return;
    if (ifa->ifa_data == &synthetic_ifaddrs_tag) {
        free(ifa);
        return;
    }
    if (next == NULL) next = dlsym(RTLD_NEXT, "freeifaddrs");
    if (next != NULL) next(ifa);
}

static int synthetic_lo_and_wan(struct ifaddrs **ifap) {
    struct synthetic_ifaddrs *block = calloc(1, sizeof(*block));
    if (block == NULL) return -1;
    memcpy(block->lo_name, "lo", 3);
    memcpy(block->wan_name, "eth0", 5);
    block->addr[0].sin_family = AF_INET;
    block->addr[0].sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    block->netmask[0].sin_family = AF_INET;
    block->netmask[0].sin_addr.s_addr = htonl(0xff000000);
    block->addrs[0].ifa_next = &block->addrs[1];
    block->addrs[0].ifa_name = block->lo_name;
    block->addrs[0].ifa_flags = IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
    block->addrs[0].ifa_addr = (struct sockaddr *)&block->addr[0];
    block->addrs[0].ifa_netmask = (struct sockaddr *)&block->netmask[0];
    block->addrs[0].ifa_data = &synthetic_ifaddrs_tag;
    block->addr[1].sin_family = AF_INET;
    block->addr[1].sin_addr.s_addr = htonl(0x0a000002);
    block->netmask[1].sin_family = AF_INET;
    block->netmask[1].sin_addr.s_addr = htonl(0xff000000);
    block->addrs[1].ifa_name = block->wan_name;
    block->addrs[1].ifa_flags = IFF_UP | IFF_RUNNING | IFF_BROADCAST;
    block->addrs[1].ifa_addr = (struct sockaddr *)&block->addr[1];
    block->addrs[1].ifa_netmask = (struct sockaddr *)&block->netmask[1];
    block->addrs[1].ifa_data = &synthetic_ifaddrs_tag;
    *ifap = &block->addrs[0];
    return 0;
}

int getifaddrs(struct ifaddrs **ifap) {
    static int (*next)(struct ifaddrs **);
    if (ifap == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (next == NULL) next = dlsym(RTLD_NEXT, "getifaddrs");
    if (next != NULL && next(ifap) == 0) {
        if (has_non_loopback_ipv4(*ifap)) return 0;
        discard_ifaddrs(*ifap);
        *ifap = NULL;
    }
    return synthetic_lo_and_wan(ifap);
}

void freeifaddrs(struct ifaddrs *ifa) {
    discard_ifaddrs(ifa);
}

/* Chrome's system resolver passes AI_ADDRCONFIG. glibc then dumps
 * RTM_GETADDR over a netlink socket that Android will bind without
 * groups; the dump only reports loopback, so public A records vanish.
 * wget/getent do not set that flag, which is why they already work. */
int getaddrinfo(const char *node, const char *service,
                const struct addrinfo *hints, struct addrinfo **res) {
    static int (*next)(const char *, const char *, const struct addrinfo *,
                       struct addrinfo **);
    if (next == NULL) next = dlsym(RTLD_NEXT, "getaddrinfo");
    if (next == NULL) {
        errno = ENOSYS;
        return EAI_SYSTEM;
    }
    if (hints == NULL || (hints->ai_flags & AI_ADDRCONFIG) == 0)
        return next(node, service, hints, res);
    struct addrinfo copy = *hints;
    copy.ai_flags &= ~(int)AI_ADDRCONFIG;
    return next(node, service, &copy, res);
}
