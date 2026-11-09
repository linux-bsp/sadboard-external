#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <fcntl.h>
#include <sys/resource.h>
#include <unistd.h>

enum { FD_LIMIT = 64 };

/* Inspect the available descriptor range without opening another descriptor. */
static unsigned open_count(void)
{
    unsigned count = 0;
    for (int fd = 0; fd < FD_LIMIT; fd++) {
        if (fcntl(fd, F_GETFD) >= 0) count++;
        else if (errno != EBADF) { perror("fcntl"); exit(70); }
    }
    return count;
}

int main(int argc, char **argv)
{
    if (argc != 1 && argc != 3) return 64;
    unsigned requested = argc == 3 ? lab_number(argv[1], 4096, 0) : 128;
    unsigned delay = argc == 3 ? lab_number(argv[2], 50, 1) : 20;
    if ((uint64_t)requested * delay > 10000) {
        fprintf(stderr, "descriptor exercise exceeds ten-second delay budget\n");
        return 64;
    }
    struct rlimit limit;
    if (getrlimit(RLIMIT_NOFILE, &limit)) { perror("getrlimit"); return 70; }
    if (limit.rlim_cur < FD_LIMIT || limit.rlim_max < FD_LIMIT) {
        fprintf(stderr, "descriptor exercise needs an inherited NOFILE limit of at least 64\n");
        return 75;
    }
    limit.rlim_cur = limit.rlim_max = FD_LIMIT;
    if (setrlimit(RLIMIT_NOFILE, &limit)) { perror("setrlimit"); return 70; }
    unsigned baseline = open_count(), peak = baseline, completed = 0;
    int failure = 0;
    if (baseline == FD_LIMIT) return 75;
    fprintf(stderr, "pid=%ld nofile=64 baseline=%u requested=%u delay_ms=%u\n",
            (long)getpid(), baseline, requested, delay);
    for (unsigned i = 0; i < requested; i++) {
        int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (fd < 0) { failure = errno; break; }
        completed++;
        unsigned active = open_count();
        if (active > peak) peak = active;
#ifdef SADBOARD_FIXED
        if (close(fd)) { failure = errno; break; }
#endif
        /* The defect discards fd without closing it: CLOEXEC does not help here. */
        lab_sleep_us(delay * 1000);
    }
    unsigned final = open_count();
    printf("{\"limit\":64,\"requested\":%u,\"delay_ms\":%u,\"baseline\":%u,"
           "\"completed\":%u,\"peak_open\":%u,\"final_open\":%u,\"error\":%d}\n",
           requested, delay, baseline, completed, peak, final, failure);
    if (failure) { errno = failure; perror("descriptor request"); }
    return failure ? (failure == EMFILE ? EMFILE : 70) : 0;
}
