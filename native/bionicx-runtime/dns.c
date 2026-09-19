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

static int write_all(int descriptor, const char *buffer, size_t length) {
    while (length != 0) {
        ssize_t written = write(descriptor, buffer, length);
        if (written <= 0) return -1;
        buffer += written;
        length -= (size_t)written;
    }
    return 0;
}

static void publish_android_resolver_config(void) {
    const char *root = bionicx_getenv("BIONICX_ROOTFS");
    const char *configured = bionicx_getenv("BIONICX_DNS_SERVERS");
    if (root == NULL || root[0] != '/' || configured == NULL ||
            configured[0] == '\0') return;

    char path[PATH_MAX], temporary[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/etc/resolv.conf", root) >=
            (int)sizeof(path)) return;
    if (snprintf(temporary, sizeof(temporary),
                 "%s/etc/.resolv.conf.bionicx.%ld", root, (long)bionicx_host_pid()) >=
            (int)sizeof(temporary)) return;
    int descriptor = open(temporary,
                          O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (descriptor < 0) return;

    char servers[256];
    size_t length = strlen(configured);
    if (length >= sizeof(servers)) {
        close(descriptor);
        unlink(temporary);
        return;
    }
    memcpy(servers, configured, length + 1);
    int count = 0;
    char *save = NULL;
    for (char *item = strtok_r(servers, ",", &save); item != NULL;
         item = strtok_r(NULL, ",", &save)) {
        struct in6_addr address;
        if (inet_pton(AF_INET, item, &address) != 1 &&
                inet_pton(AF_INET6, item, &address) != 1) continue;
        char line[INET6_ADDRSTRLEN + 16];
        int line_length = snprintf(line, sizeof(line), "nameserver %s\n", item);
        if (line_length <= 0 ||
                write_all(descriptor, line, (size_t)line_length) != 0) {
            close(descriptor);
            unlink(temporary);
            return;
        }
        ++count;
    }
    if (close(descriptor) != 0 || count == 0 || rename(temporary, path) != 0)
        unlink(temporary);
}

__attribute__((constructor)) static void initialize_android_dns(void) {
    publish_android_resolver_config();
}

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

/* Chrome DnsConfigServiceLinux calls res_ninit() on a private res_state.
 * libc then fopen()s /etc/resolv.conf with an in-DSO symbol, so FHS open()
 * never sees it. On Android that file is empty and Chrome reports
 * DNS_PROBE_FINISHED_BAD_CONFIG even though guest resolv.conf is correct.
 * Re-apply the host nameservers already published from ConnectivityManager. */
static void apply_android_nameservers(res_state statp) {
    if (statp == NULL) return;
    const char *configured = getenv("BIONICX_DNS_SERVERS");
    if (configured == NULL || configured[0] == '\0') return;
    char servers[256];
    size_t length = strlen(configured);
    if (length >= sizeof(servers)) return;
    memcpy(servers, configured, length + 1);
    int count = 0;
    char *save = NULL;
    for (char *item = strtok_r(servers, ",", &save); item != NULL && count < MAXNS;
         item = strtok_r(NULL, ",", &save)) {
        struct in_addr address;
        if (inet_pton(AF_INET, item, &address) != 1) continue;
        memset(&statp->nsaddr_list[count], 0, sizeof(statp->nsaddr_list[count]));
        statp->nsaddr_list[count].sin_family = AF_INET;
        statp->nsaddr_list[count].sin_port = htons(53);
        statp->nsaddr_list[count].sin_addr = address;
        ++count;
    }
    if (count == 0) return;
    statp->nscount = count;
    statp->options |= RES_INIT | RES_RECURSE | RES_DEFNAMES | RES_DNSRCH;
    if (statp->ndots == 0) statp->ndots = 1;
    if (statp->retrans == 0) statp->retrans = 5;
    if (statp->retry == 0) statp->retry = 2;
}

static int wrap_res_ninit(int (*next)(res_state), res_state statp) {
    int rc = next != NULL ? next(statp) : -1;
    apply_android_nameservers(statp);
    if (statp != NULL && statp->nscount > 0) {
        statp->options |= RES_INIT | RES_RECURSE | RES_DEFNAMES | RES_DNSRCH;
        return 0;
    }
    return rc;
}

#undef res_ninit
#undef res_init
#undef __res_ninit
#undef __res_init

int res_ninit(res_state statp) {
    static int (*next)(res_state);
    if (next == NULL) {
        next = dlsym(RTLD_NEXT, "res_ninit");
        if (next == NULL) next = dlsym(RTLD_NEXT, "__res_ninit");
    }
    return wrap_res_ninit(next, statp);
}

int res_init(void) {
    static int (*next)(void);
    if (next == NULL) {
        next = dlsym(RTLD_NEXT, "res_init");
        if (next == NULL) next = dlsym(RTLD_NEXT, "__res_init");
    }
    int rc = next != NULL ? next() : res_ninit(&_res);
    apply_android_nameservers(&_res);
    if (_res.nscount > 0) {
        _res.options |= RES_INIT | RES_RECURSE | RES_DEFNAMES | RES_DNSRCH;
        return 0;
    }
    return rc;
}

extern int __res_ninit(res_state) __attribute__((alias("res_ninit")));
extern int __res_init(void) __attribute__((alias("res_init")));
