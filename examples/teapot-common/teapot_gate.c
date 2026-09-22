#define _POSIX_C_SOURCE 200809L

#include "teapot_gate.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

int teapot_gate_enabled(void)
{
    return teapot_env_flag("TEAPOT_GATE");
}

void teapot_gate_hello(void)
{
    printf("TEAPOT_CLIENT pid=%ld\n", (long)getpid());
    if (teapot_env_flag("TEAPOT_GATE_ACK"))
        puts("TEAPOT_SYNC ack-v1");
    fflush(stdout);
}

void teapot_gate_phase(const char *name)
{
    const char *dir = getenv("TEAPOT_EVIDENCE");
    char ack[512];
    int handshake = teapot_env_flag("TEAPOT_GATE_ACK") && dir && dir[0];
    if (handshake) {
        snprintf(ack, sizeof(ack), "%s/gate.ack", dir);
        unlink(ack);
    }
    printf("TEAPOT_PHASE %s\n", name);
    fflush(stdout);
    if (!handshake) {
        sleep(4);
        return;
    }
    /* Pause only the test's main loop, never vendor driver worker threads. */
    const struct timespec interval = { .tv_sec = 0, .tv_nsec = 100000000 };
    /* Android screencap can exceed 30 seconds while the GPU matrix is under
     * load. Keep this in sync with the host/device evidence handshake. */
    for (int attempt = 0; attempt < 900; attempt++) {
        if (access(ack, F_OK) == 0) {
            unlink(ack);
            return;
        }
        nanosleep(&interval, NULL);
    }
    fprintf(stderr, "TEAPOT_CLIENT missing screenshot acknowledgement\n");
    exit(2);
}

void teapot_gate_size(const char *phase, int width, int height)
{
    printf("TEAPOT_SIZE phase=%s %dx%d\n", phase, width, height);
    fflush(stdout);
}

void teapot_gate_maps(void)
{
    const char *dir = getenv("TEAPOT_EVIDENCE");
    char path[512];
    FILE *maps;
    FILE *saved;
    char line[4096];

    snprintf(path, sizeof(path), "%s/maps.txt", dir && dir[0] ? dir : ".");
    maps = fopen("/proc/self/maps", "r");
    saved = fopen(path, "w");
    if (!maps || !saved)
        exit(2);
    while (fgets(line, (int)sizeof(line), maps)) {
        if (fputs(line, saved) == EOF)
            exit(2);
    }
    if (ferror(maps) || fclose(saved) || fclose(maps))
        exit(2);
}

void teapot_gate_pass(void)
{
    puts("TEAPOT_CLIENT PASS");
    fflush(stdout);
}

void teapot_result(const char *name, int ok, const char *detail)
{
    printf("BXTEST %s %s%s%s\n", ok ? "PASS" : "FAIL", name,
           detail && detail[0] ? " " : "", detail ? detail : "");
    fflush(stdout);
}

double teapot_now(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

int teapot_env_flag(const char *name)
{
    const char *value = getenv(name);

    return value && value[0] && strcmp(value, "0") != 0;
}

int teapot_env_int(const char *name, int fallback)
{
    const char *value = getenv(name);

    return value && value[0] ? atoi(value) : fallback;
}

int teapot_software_renderer(const char *renderer)
{
    const char *text = renderer ? renderer : "";

    return strstr(text, "llvmpipe") != NULL || strstr(text, "softpipe") != NULL ||
           strstr(text, "swrast") != NULL || strstr(text, "software") != NULL;
}

void teapot_body_color(float *r, float *g, float *b)
{
    *r = 0.95f;
    *g = 0.55f;
    *b = 0.15f;
}
