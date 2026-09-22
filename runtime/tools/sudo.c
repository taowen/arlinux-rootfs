#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* Package-transaction launcher. This selects a guest identity, not Android
 * privilege escalation. Session variables are intentionally not inherited. */

int main(int argc, char **argv) {
    const char *root, *files, *tmp, *home, *term, *dns;
    char pathbuf[4096], homebuf[512], termbuf[256];
    char rootbuf[512], filesbuf[512], tmpbuf[512], tmpdirbuf[512];
    char dnsbuf[512], dpkgbuf[512];
    char *envp[24];
    int n = 0;

    if (argc < 2) {
        fprintf(stderr, "usage: sudo COMMAND [ARG...]\n");
        return 1;
    }
    root = getenv("BIONICX_ROOTFS");
    files = getenv("BIONICX_FILES");
    tmp = getenv("BIONICX_TMPDIR");
    if (root == NULL || root[0] != '/' || files == NULL || tmp == NULL) {
        fprintf(stderr, "sudo: missing BIONICX runtime environment\n");
        return 1;
    }
    home = getenv("HOME");
    term = getenv("TERM");
    dns = getenv("BIONICX_DNS_SERVERS");
    if (snprintf(pathbuf, sizeof(pathbuf),
            "PATH=%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin",
            root, root, root, root) >= (int)sizeof(pathbuf) ||
            snprintf(homebuf, sizeof(homebuf), "HOME=%s",
                    home != NULL && home[0] != '\0' ? home : files) >=
                    (int)sizeof(homebuf) ||
            snprintf(termbuf, sizeof(termbuf), "TERM=%s",
                    term != NULL && term[0] != '\0' ? term : "dumb") >=
                    (int)sizeof(termbuf) ||
            snprintf(rootbuf, sizeof(rootbuf), "BIONICX_ROOTFS=%s", root) >=
                    (int)sizeof(rootbuf) ||
            snprintf(filesbuf, sizeof(filesbuf), "BIONICX_FILES=%s", files) >=
                    (int)sizeof(filesbuf) ||
            snprintf(tmpbuf, sizeof(tmpbuf), "BIONICX_TMPDIR=%s", tmp) >=
                    (int)sizeof(tmpbuf) ||
            snprintf(tmpdirbuf, sizeof(tmpdirbuf), "TMPDIR=%s", tmp) >=
                    (int)sizeof(tmpdirbuf) ||
            snprintf(dpkgbuf, sizeof(dpkgbuf), "DPKG_ROOT=%s", root) >=
                    (int)sizeof(dpkgbuf)) {
        fprintf(stderr, "sudo: environment too long\n");
        return 1;
    }
    envp[n++] = pathbuf;
    envp[n++] = homebuf;
    envp[n++] = termbuf;
    envp[n++] = (char *)"LANG=C.UTF-8";
    envp[n++] = (char *)"LC_ALL=C.UTF-8";
    envp[n++] = rootbuf;
    envp[n++] = filesbuf;
    envp[n++] = tmpbuf;
    envp[n++] = tmpdirbuf;
    envp[n++] = (char *)"BIONICX_VIRTUAL_ROOT=1";
    envp[n++] = dpkgbuf;
    envp[n++] = (char *)"GIO_USE_VFS=local";
    if (dns != NULL && dns[0] != '\0' &&
            snprintf(dnsbuf, sizeof(dnsbuf), "BIONICX_DNS_SERVERS=%s", dns) <
                    (int)sizeof(dnsbuf))
        envp[n++] = dnsbuf;
    envp[n] = NULL;
    execvpe(argv[1], argv + 1, envp);
    fprintf(stderr, "sudo: %s: %s\n", argv[1], strerror(errno));
    return 127;
}
