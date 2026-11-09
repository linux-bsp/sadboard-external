#include "suite-advanced.h"
#include <fcntl.h>
#include <poll.h>
#include <stdalign.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static void cpu_work(unsigned microseconds)
{
    uint64_t end = lab_clock(CLOCK_THREAD_CPUTIME_ID) + (uint64_t)microseconds * 1000;
    while (lab_clock(CLOCK_THREAD_CPUTIME_ID) < end)
        atomic_signal_fence(memory_order_seq_cst);
}
struct priority {
    pthread_mutex_t lock;
    atomic_uint low_ready, high_ready, medium_ready, medium_done;
    int gate[2];
    uint64_t waited, medium_at_acquire;
    unsigned value;
};
static void *low_priority(void *arg)
{
    struct priority *p = arg;
    suite_thread(pthread_mutex_lock(&p->lock));
    atomic_store(&p->low_ready, 1);
    char byte;
    suite_require(read(p->gate[0], &byte, 1) == 1, "release low priority owner");
    cpu_work(5000);
    p->value += 42;
    suite_thread(pthread_mutex_unlock(&p->lock));
    return NULL;
}
static void *high_priority(void *arg)
{
    struct priority *p = arg;
    uint64_t start = lab_clock(CLOCK_MONOTONIC);
    atomic_store(&p->high_ready, 1);
    suite_thread(pthread_mutex_lock(&p->lock));
    p->waited = lab_clock(CLOCK_MONOTONIC) - start;
    p->medium_at_acquire = atomic_load(&p->medium_done);
    p->value += 42;
    suite_thread(pthread_mutex_unlock(&p->lock));
    return NULL;
}
static void *medium_priority(void *arg)
{
    struct priority *p = arg;
    atomic_store(&p->medium_ready, 1);
    cpu_work(60000);
    atomic_store(&p->medium_done, 1);
    return NULL;
}
static void rt_thread(pthread_t *thread, void *(*function)(void *), void *argument, int priority)
{
    pthread_attr_t attr;
    suite_thread(pthread_attr_init(&attr));
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(3, &mask);
    suite_thread(pthread_attr_setaffinity_np(&attr, sizeof(mask), &mask));
    suite_thread(pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED));
    suite_thread(pthread_attr_setschedpolicy(&attr, SCHED_FIFO));
    struct sched_param policy = {.sched_priority = priority};
    suite_thread(pthread_attr_setschedparam(&attr, &policy));
    int error = pthread_create(thread, &attr, function, argument);
    pthread_attr_destroy(&attr);
    if (error == EPERM) {
        fprintf(stderr, "SCHED_FIFO requires CAP_SYS_NICE; experiment not performed\n");
        exit(75);
    }
    suite_thread(error);
}
struct callbacks {
    int event;
    atomic_uint ready, finished;
    atomic_uint_fast64_t sent[32];
    uint64_t handled[32], data_done;
    unsigned jobs;
};
static void *control_source(void *argument)
{
    struct callbacks *p = argument;
    while (!atomic_load(&p->ready))
        lab_sleep_us(100);
    for (unsigned i = 0; i < 32; i++) {
        atomic_store(&p->sent[i], lab_clock(CLOCK_MONOTONIC));
        uint64_t one = 1;
        suite_require(write(p->event, &one, sizeof(one)) == sizeof(one), "control event");
        lab_sleep_us(1000);
    }
    return NULL;
}
static void *data_callback(void *argument)
{
    struct callbacks *p = argument;
    for (unsigned i = 0; i < 4; i++) {
        lab_sleep_us(50000);
        p->jobs++;
    }
    p->data_done = lab_clock(CLOCK_MONOTONIC);
    atomic_store(&p->finished, 1);
    return NULL;
}
struct counters {
    atomic_uint_fast64_t *value;
    unsigned cpu;
};
static void *count_worker(void *argument)
{
    struct counters *p = argument;
    suite_pin(p->cpu);
    for (unsigned i = 0; i < 500000; i++)
        atomic_fetch_add_explicit(p->value, 1, memory_order_relaxed);
    return NULL;
}
static void logging(bool fixed, struct sample *r)
{
    int fd =
        open(fixed ? "log-fixed.txt" : "log-bug.txt", O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    suite_require(fd >= 0, "durable log");
    char buffer[4096];
    size_t used = 0;
    for (unsigned i = 0; i < 1024; i++) {
        int n = snprintf(buffer + used, sizeof(buffer) - used, "record=%u value=%u\n", i, i * 3);
        suite_require(n > 0 && (size_t)n < sizeof(buffer) - used, "log format");
        used += (size_t)n;
        if (!fixed || (i + 1) % 32 == 0) {
            suite_require(write(fd, buffer, used) == (ssize_t)used, "durable write");
            r->observed++;
            suite_require(!fdatasync(fd), "log commit");
            r->auxiliary++;
            used = 0;
        }
    }
    suite_require(!used && lseek(fd, 0, SEEK_SET) == 0, "read durable records");
    FILE *f = fdopen(fd, "r");
    suite_require(f != NULL, "durable stream");
    unsigned i, value;
    while (fscanf(f, "record=%u value=%u\n", &i, &value) == 2) {
        suite_require(i == r->completed && value == i * 3, "durable contents");
        r->completed++;
        r->checksum += i;
    }
    fclose(f);
}
void suite_adv_perf(bool fixed, struct sample *r, unsigned kind)
{
    if (kind == 0) {
        suite_require(sysconf(_SC_NPROCESSORS_ONLN) >= 4, "AM625 four cores required");
        suite_pin(0);
        struct priority p = {0};
        suite_require(!pipe(p.gate), "priority gate");
        pthread_mutexattr_t attr;
        suite_thread(pthread_mutexattr_init(&attr));
        if (fixed)
            suite_thread(pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT));
        suite_thread(pthread_mutex_init(&p.lock, &attr));
        pthread_t low, high, medium;
        rt_thread(&low, low_priority, &p, 10);
        while (!atomic_load(&p.low_ready))
            lab_sleep_us(100);
        rt_thread(&high, high_priority, &p, 50);
        while (!atomic_load(&p.high_ready))
            lab_sleep_us(100);
        rt_thread(&medium, medium_priority, &p, 30);
        while (!atomic_load(&p.medium_ready))
            lab_sleep_us(100);
        char byte = 42;
        suite_require(write(p.gate[1], &byte, 1) == 1, "owner wake");
        suite_thread(pthread_join(low, NULL));
        suite_thread(pthread_join(high, NULL));
        suite_thread(pthread_join(medium, NULL));
        r->completed = 2;
        r->checksum = p.value;
        r->observed = p.waited;
        r->auxiliary = p.medium_at_acquire;
        suite_event("medium_finished_when_high_acquired", p.medium_at_acquire);
        pthread_mutex_destroy(&p.lock);
        pthread_mutexattr_destroy(&attr);
        close(p.gate[0]);
        close(p.gate[1]);
    } else if (kind == 1) {
        struct callbacks p = {.event = eventfd(0, EFD_CLOEXEC)};
        suite_require(p.event >= 0, "control eventfd");
        pthread_t source, worker;
        suite_thread(pthread_create(&source, NULL, control_source, &p));
        atomic_store(&p.ready, 1);
        if (fixed)
            suite_thread(pthread_create(&worker, NULL, data_callback, &p));
        else
            data_callback(&p);
        unsigned processed = 0;
        uint64_t maximum = 0;
        while (processed < 32) {
            uint64_t count;
            suite_require(read(p.event, &count, sizeof(count)) == sizeof(count),
                          "drain control events");
            for (uint64_t i = 0; i < count; i++) {
                uint64_t now = lab_clock(CLOCK_MONOTONIC);
                p.handled[processed] = now;
                uint64_t late = now - atomic_load(&p.sent[processed]);
                if (late > maximum)
                    maximum = late;
                r->checksum += processed;
                processed++;
            }
        }
        suite_thread(pthread_join(source, NULL));
        if (fixed)
            suite_thread(pthread_join(worker, NULL));
        r->completed = processed + p.jobs;
        r->checksum += p.jobs * 100;
        r->observed = maximum;
        r->before = p.data_done;
        r->after = p.handled[0];
        for (unsigned i = 0; i < 32; i++) {
            suite_event("control_release_ns", atomic_load(&p.sent[i]));
            suite_event("control_handled_ns", p.handled[i]);
        }
        close(p.event);
    } else if (kind == 2) {
        unsigned char *memory = aligned_alloc(64, 128);
        suite_require(memory != NULL, "aligned counters");
        memset(memory, 0, 128);
        atomic_uint_fast64_t *a = (void *)memory, *b = (void *)(memory + (fixed ? 64 : sizeof(*a)));
        atomic_init(a, 0);
        atomic_init(b, 0);
        cpu_set_t allowed;
        suite_require(!sched_getaffinity(0, sizeof(allowed), &allowed), "CPU set");
        unsigned cpus[2], n = 0;
        for (unsigned i = 0; i < CPU_SETSIZE && n < 2; i++)
            if (CPU_ISSET(i, &allowed))
                cpus[n++] = i;
        suite_require(n == 2, "two CPUs required");
        struct counters args[2] = {{a, cpus[0]}, {b, cpus[1]}};
        pthread_t threads[2];
        for (unsigned i = 0; i < 2; i++)
            suite_thread(pthread_create(&threads[i], NULL, count_worker, &args[i]));
        for (unsigned i = 0; i < 2; i++)
            suite_thread(pthread_join(threads[i], NULL));
        r->completed = atomic_load(a) + atomic_load(b);
        r->checksum = r->completed;
        r->observed = (uintptr_t)b - (uintptr_t)a;
        r->auxiliary = 2;
        free(memory);
    } else if (kind == 3)
        logging(fixed, r);
    else if (kind == 4) {
        int fd = open(fixed ? "blocks-fixed.bin" : "blocks-bug.bin",
                      O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
        suite_require(fd >= 0, "I/O file");
        unsigned char block[4096];
        for (unsigned i = 0; i < 64; i++) {
            memset(block, (int)i, sizeof(block));
            suite_require(write(fd, block, sizeof(block)) == sizeof(block), "block write");
            if (!fixed || (i + 1) % 8 == 0) {
                suite_require(!fsync(fd), "block commit");
                r->observed++;
            }
            r->auxiliary += sizeof(block);
        }
        suite_require(lseek(fd, 0, SEEK_SET) == 0, "rewind blocks");
        for (unsigned i = 0; i < 64; i++) {
            suite_require(read(fd, block, sizeof(block)) == sizeof(block), "read block");
            for (unsigned j = 0; j < sizeof(block); j++) {
                suite_require(block[j] == i, "block checksum");
                r->checksum += block[j];
            }
            r->completed++;
        }
        close(fd);
    } else if (kind == 5) {
        size_t page = (size_t)sysconf(_SC_PAGESIZE), bytes = 16 * 1024 * 1024;
        unsigned char *p =
            mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        suite_require(p != MAP_FAILED, "working set");
        suite_require(!madvise(p, bytes, MADV_NOHUGEPAGE), "base pages");
        unsigned char *resident = calloc(bytes / page, 1);
        suite_require(resident != NULL, "residency vector");
        if (fixed)
            for (size_t i = 0; i < bytes; i += page)
                p[i] = 0;
        suite_require(!mincore(p, bytes, resident), "initial residency");
        for (size_t i = 0; i < bytes / page; i++)
            r->observed += !(resident[i] & 1);
        struct rusage before, after;
        suite_require(!getrusage(RUSAGE_SELF, &before), "fault baseline");
        for (size_t i = 0; i < bytes; i += page) {
            p[i] = 42;
            r->checksum += p[i];
            r->completed++;
        }
        suite_require(!getrusage(RUSAGE_SELF, &after), "fault result");
        r->auxiliary = (uint64_t)(after.ru_minflt - before.ru_minflt);
        r->before = bytes;
        r->after = bytes / page;
        suite_event("business_minor_faults", r->auxiliary);
        munmap(p, bytes);
        free(resident);
    } else {
        struct suite_cgroup cg;
        suite_cgroup_create(&cg, fixed ? "quota-fixed" : "quota-bug");
        suite_cgroup_set(&cg, "cpu.max", fixed ? "max 100000" : "25000 100000");
        int gate[2], result[2];
        suite_require(!pipe(gate) && !pipe(result), "quota child pipes");
        pid_t child = fork();
        suite_require(child >= 0, "quota child");
        if (!child) {
            suite_child_guard();
            close(gate[1]);
            close(result[0]);
            char byte;
            suite_require(read(gate[0], &byte, 1) == 1, "quota start");
            uint64_t started = lab_clock(CLOCK_PROCESS_CPUTIME_ID);
            volatile uint32_t state = 42;
            for (unsigned i = 0; i < 30000000; i++)
                state = state * 1664525U + 1013904223U;
            uint64_t values[2] = {state, lab_clock(CLOCK_PROCESS_CPUTIME_ID) - started};
            suite_require(write(result[1], values, sizeof(values)) == sizeof(values),
                          "quota result");
            _exit(0);
        }
        close(gate[0]);
        close(result[1]);
        suite_cgroup_join(&cg, child);
        char byte = 42;
        suite_require(write(gate[1], &byte, 1) == 1, "release quota task");
        close(gate[1]);
        uint64_t values[2];
        suite_require(read(result[0], values, sizeof(values)) == sizeof(values), "quota output");
        close(result[0]);
        bool timed;
        int status = suite_wait(child, 5000, &timed);
        suite_require(!timed && WIFEXITED(status) && !WEXITSTATUS(status), "quota completion");
        r->completed = 30000000;
        r->checksum = values[0];
        r->auxiliary = values[1];
        r->observed = suite_cgroup_value(&cg, "cpu.stat", "nr_throttled");
        r->after = suite_cgroup_value(&cg, "cpu.stat", "throttled_usec");
        suite_event("quota_usec", fixed ? 0 : 25000);
        suite_cgroup_remove(&cg);
    }
}
