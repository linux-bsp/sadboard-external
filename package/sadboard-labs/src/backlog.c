/* A slow downstream consumer retains valid, reachable messages in an input queue. */
#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <pthread.h>
#include <string.h>
#include <sys/resource.h>

struct message { struct message *next; unsigned char data[]; };
struct pipeline {
    pthread_mutex_t lock;
    pthread_cond_t ready, space;
    struct message *head, *tail;
    unsigned total, queued, peak, consumed, allocations, frees, waits, limit;
    size_t bytes;
    uint64_t live, peak_live, checksum;
    FILE *csv;
};

static void must(int result)
{
    if (result) { errno = result; perror("pthread"); exit(70); }
}

static void *consume(void *argument)
{
    struct pipeline *p = argument;
    /* A bounded downstream startup/reconnect stall, identical in both versions. */
    lab_sleep_us(100000);
    for (unsigned n = 0; n < p->total; n++) {
        must(pthread_mutex_lock(&p->lock));
        while (!p->head) must(pthread_cond_wait(&p->ready, &p->lock));
        struct message *message = p->head;
        p->head = message->next;
        if (!p->head) p->tail = NULL;
        p->queued--;
        must(pthread_cond_signal(&p->space));
        must(pthread_mutex_unlock(&p->lock));
        lab_sleep_us(3000);
        uint64_t checksum = 0;
        for (size_t i = 0; i < p->bytes; i++) checksum += message->data[i];
        free(message);
        must(pthread_mutex_lock(&p->lock));
        p->checksum += checksum; p->consumed++; p->frees++; p->live -= p->bytes;
        must(pthread_mutex_unlock(&p->lock));
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 4) { fprintf(stderr, "Usage: %s MESSAGES PAYLOAD_KIB CSV\n", argv[0]); return 64; }
    unsigned count = lab_number(argv[1], 1024, 0), kib = lab_number(argv[2], 64, 0);
    if (count < 16) return 64;
    FILE *csv = fopen(argv[3], "w");
    if (!csv) { perror("CSV"); return 71; }
    struct pipeline p = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER,
        .space = PTHREAD_COND_INITIALIZER, .total = count, .bytes = (size_t)kib * 1024, .csv = csv};
#ifdef SADBOARD_FIXED
    p.limit = 8;
#else
    p.limit = count;
#endif
    fprintf(csv, "index,queued,live_payload_bytes,consumed\n");
    uint64_t start = lab_clock(CLOCK_MONOTONIC);
    pthread_t worker; must(pthread_create(&worker, NULL, consume, &p));
    for (unsigned i = 0; i < count; i++) {
        must(pthread_mutex_lock(&p.lock));
        while (p.queued == p.limit) { p.waits++; must(pthread_cond_wait(&p.space, &p.lock)); }
        struct message *message = malloc(sizeof(*message) + p.bytes);
        if (!message) { perror("malloc"); exit(71); }
        message->next = NULL;
        memset(message->data, (int)(i & 255), p.bytes);
        if (p.tail) p.tail->next = message; else p.head = message;
        p.tail = message; p.queued++; p.allocations++; p.live += p.bytes;
        if (p.queued > p.peak) p.peak = p.queued;
        if (p.live > p.peak_live) p.peak_live = p.live;
        fprintf(csv, "%u,%u,%" PRIu64 ",%u\n", i, p.queued, p.live, p.consumed);
        must(pthread_cond_signal(&p.ready));
        must(pthread_mutex_unlock(&p.lock));
        lab_sleep_us(1000);
    }
    must(pthread_join(worker, NULL));
    uint64_t wall = lab_clock(CLOCK_MONOTONIC) - start;
    if (fclose(csv)) return 74;
    struct rusage usage;
    if (getrusage(RUSAGE_SELF, &usage)) return 70;
    printf("{\"messages\":%u,\"payload_kib\":%u,\"completed\":%u,\"checksum\":%" PRIu64
           ",\"allocations\":%u,\"frees\":%u,\"peak_queue\":%u,\"peak_live_bytes\":%" PRIu64
           ",\"final_live_bytes\":%" PRIu64 ",\"producer_waits\":%u,\"maxrss_bytes\":%" PRIu64
           ",\"wall_ns\":%" PRIu64 "}\n", count, kib, p.consumed, p.checksum, p.allocations,
           p.frees, p.peak, p.peak_live, p.live, p.waits, (uint64_t)usage.ru_maxrss * 1024, wall);
    must(pthread_cond_destroy(&p.ready)); must(pthread_cond_destroy(&p.space));
    must(pthread_mutex_destroy(&p.lock));
    return p.queued || p.live ? 70 : 0;
}
