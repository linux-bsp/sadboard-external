/* Two workers perform the same sleeps and updates; only lock scope differs. */
#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <pthread.h>

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static unsigned iterations, delay_us, completed;

static void checked(int result)
{
    if (result) {
        fprintf(stderr, "pthread error: %d\n", result);
        exit(70);
    }
}

static void *worker(void *unused)
{
    (void)unused;
    for (unsigned i = 0; i < iterations; i++) {
#ifdef SADBOARD_FIXED
        lab_sleep_us(delay_us);
        checked(pthread_mutex_lock(&mutex));
#else
        checked(pthread_mutex_lock(&mutex));
        lab_sleep_us(delay_us);
#endif
        completed++;
        checked(pthread_mutex_unlock(&mutex));
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s ITERATIONS DELAY_US\n", argv[0]);
        return 64;
    }
    iterations = lab_number(argv[1], 200, 0);
    delay_us = lab_number(argv[2], 100000, 0);
    if ((uint64_t)iterations * delay_us * 2 > UINT64_C(10000000)) {
        fprintf(stderr, "duration budget exceeded\n");
        return 64;
    }
    pthread_t threads[2];
    uint64_t wall = lab_clock(CLOCK_MONOTONIC), cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID);
    for (unsigned i = 0; i < 2; i++)
        checked(pthread_create(&threads[i], NULL, worker, NULL));
    for (unsigned i = 0; i < 2; i++)
        checked(pthread_join(threads[i], NULL));
    cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    wall = lab_clock(CLOCK_MONOTONIC) - wall;
    printf("{\"iterations\":%u,\"completed\":%u,\"cpu_ns\":%" PRIu64
           ",\"wall_ns\":%" PRIu64 "}\n", iterations, completed, cpu, wall);
    return completed == iterations * 2 ? 0 : 1;
}
