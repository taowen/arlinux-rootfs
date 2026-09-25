#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

/* The runtime honors this ELF's virtual set-id mode, never the Android UID. */
int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: sudo COMMAND [ARG...]\n");
        return 2;
    }
    if (setgid(0) != 0 || setuid(0) != 0) {
        perror("sudo: virtual root identity");
        return 1;
    }
    execvp(argv[1], argv + 1);
    fprintf(stderr, "sudo: %s: %s\n", argv[1], strerror(errno));
    return 127;
}
