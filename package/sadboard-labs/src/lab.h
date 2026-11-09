#ifndef SADBOARD_LAB_H
#define SADBOARD_LAB_H
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static inline unsigned lab_number(const char *text, unsigned maximum, int zero)
{
    char *end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (errno || !*text || *text == '-' || *end || value > maximum || (!zero && !value)) {
        fprintf(stderr, "invalid numeric argument: %s\n", text);
        exit(64);
    }
    return (unsigned)value;
}

static inline uint64_t lab_clock(clockid_t clock)
{
    struct timespec time;
    if (clock_gettime(clock, &time)) {
        perror("clock_gettime");
        exit(70);
    }
    return (uint64_t)time.tv_sec * UINT64_C(1000000000) + (uint64_t)time.tv_nsec;
}

static inline void lab_sleep_us(unsigned microseconds)
{
    struct timespec delay = {.tv_sec = microseconds / 1000000,
                             .tv_nsec = (long)(microseconds % 1000000) * 1000};
    while (nanosleep(&delay, &delay)) {
        if (errno != EINTR) {
            perror("nanosleep");
            exit(70);
        }
    }
}
#endif
