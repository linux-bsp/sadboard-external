/* Periodic telemetry: relative sleeps accumulate callback time into phase drift. */
#define _POSIX_C_SOURCE 200809L
#include "lab.h"

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

#ifdef SADBOARD_FIXED
static void sleep_until(uint64_t deadline)
{
    struct timespec time = {.tv_sec = (time_t)(deadline / 1000000000),
                            .tv_nsec = (long)(deadline % 1000000000)};
    int result;
    do { result = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &time, NULL); }
    while (result == EINTR);
    if (result) { errno = result; perror("clock_nanosleep"); exit(70); }
}
#endif

int main(int argc, char **argv)
{
    if (argc != 5) { fprintf(stderr, "Usage: %s CYCLES PERIOD_US WORK_US CSV\n", argv[0]); return 64; }
    unsigned cycles = lab_number(argv[1], 500, 0), period = lab_number(argv[2], 50000, 0);
    unsigned work = lab_number(argv[3], 10000, 0);
    if (cycles < 2 || work * 2 > period || (uint64_t)cycles * (period + work) > 5000000) return 64;
    FILE *csv = fopen(argv[4], "w");
    if (!csv) { perror("CSV"); return 71; }
    fprintf(csv, "index,release_ns,wake_ns,finish_ns\n");
    uint64_t late[500], checksum = 0, cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID);
    uint64_t start = lab_clock(CLOCK_MONOTONIC);
    for (unsigned i = 0; i < cycles; i++) {
        uint64_t release = start + (uint64_t)(i + 1) * period * 1000;
#ifdef SADBOARD_FIXED
        sleep_until(release);
#else
        lab_sleep_us(period);
#endif
        uint64_t wake = lab_clock(CLOCK_MONOTONIC);
        late[i] = wake > release ? wake - release : 0;
        /* Same simulated device I/O and same completed work in both versions. */
        lab_sleep_us(work);
        checksum += (uint64_t)i * 3 + 7;
        uint64_t finish = lab_clock(CLOCK_MONOTONIC);
        fprintf(csv, "%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n", i, release, wake, finish);
    }
    uint64_t wall = lab_clock(CLOCK_MONOTONIC) - start, drift = late[cycles - 1];
    cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    if (fclose(csv)) return 74;
    qsort(late, cycles, sizeof(late[0]), compare_u64);
    printf("{\"cycles\":%u,\"period_us\":%u,\"work_us\":%u,\"completed\":%u,\"checksum\":%" PRIu64
           ",\"cpu_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64 ",\"p50_ns\":%" PRIu64
           ",\"p99_ns\":%" PRIu64 ",\"max_ns\":%" PRIu64 ",\"drift_ns\":%" PRIu64 "}\n",
           cycles, period, work, cycles, checksum, cpu, wall, late[(cycles + 1) / 2 - 1],
           late[(cycles * 99 + 99) / 100 - 1], late[cycles - 1], drift);
    return 0;
}
