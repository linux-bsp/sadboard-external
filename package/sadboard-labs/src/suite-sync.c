#include "suite.h"
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

struct locks {
    pthread_mutex_t a, b;
    pthread_barrier_t barrier;
    bool fixed;
    unsigned value;
};
struct actor {
    struct locks *locks;
    unsigned number;
};
static void barrier(pthread_barrier_t *b)
{
    int n = pthread_barrier_wait(b);
    suite_require(n == 0 || n == PTHREAD_BARRIER_SERIAL_THREAD, "barrier");
}
static void *abba_worker(void *argument)
{
    struct actor *actor = argument;
    struct locks *p = actor->locks;
    pthread_mutex_t *first = &p->a, *second = &p->b;
    if (!p->fixed && actor->number) {
        first = &p->b;
        second = &p->a;
    }
    if (p->fixed)
        barrier(&p->barrier);
    suite_thread(pthread_mutex_lock(first));
    if (!p->fixed)
        barrier(&p->barrier);
    suite_thread(pthread_mutex_lock(second));
    p->value += 42;
    suite_thread(pthread_mutex_unlock(second));
    suite_thread(pthread_mutex_unlock(first));
    return NULL;
}
static void *update_worker(void *argument)
{
    struct locks *p = argument;
    for (unsigned i = 0; i < 32; i++) {
        unsigned snapshot = 0;
        if (!p->fixed) {
            suite_thread(pthread_mutex_lock(&p->a));
            snapshot = p->value;
            suite_thread(pthread_mutex_unlock(&p->a));
        }
        barrier(&p->barrier);
        suite_thread(pthread_mutex_lock(&p->a));
        if (p->fixed)
            p->value++;
        else
            p->value = snapshot + 1;
        suite_thread(pthread_mutex_unlock(&p->a));
        barrier(&p->barrier);
    }
    return NULL;
}
struct reader {
    int fd;
    unsigned eof;
};
static void *blocked_reader(void *argument)
{
    struct reader *r = argument;
    char byte;
    ssize_t n = read(r->fd, &byte, 1);
    r->eof = n == 0;
    return NULL;
}
void suite_sync(bool fixed, struct sample *r, unsigned kind)
{
    if (kind == 0) {
        int p[2];
        suite_require(!pipe(p), "ABBA result pipe");
        pid_t child = fork();
        suite_require(child >= 0, "ABBA fork");
        if (!child) {
            close(p[0]);
            struct locks locks = {
                .a = PTHREAD_MUTEX_INITIALIZER, .b = PTHREAD_MUTEX_INITIALIZER, .fixed = fixed};
            suite_thread(pthread_barrier_init(&locks.barrier, NULL, 2));
            struct actor actors[2] = {{&locks, 0}, {&locks, 1}};
            pthread_t threads[2];
            for (unsigned i = 0; i < 2; i++)
                suite_thread(pthread_create(&threads[i], NULL, abba_worker, &actors[i]));
            for (unsigned i = 0; i < 2; i++)
                suite_thread(pthread_join(threads[i], NULL));
            suite_require(write(p[1], &locks.value, sizeof(locks.value)) == sizeof(locks.value),
                          "ABBA result");
            _exit(0);
        }
        close(p[1]);
        bool timed;
        int status = suite_wait(child, 250, &timed);
        unsigned value = 0;
        if (!timed)
            suite_require(read(p[0], &value, sizeof(value)) == sizeof(value), "ABBA value");
        close(p[0]);
        r->observed = timed;
        r->auxiliary = WIFSIGNALED(status) ? (unsigned)WTERMSIG(status) : 0;
        r->completed = 1;
        r->checksum = value;
        suite_event("child_wait_status", (unsigned)status);
    } else if (kind == 1) {
        pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
        pthread_cond_t condition;
        pthread_condattr_t attr;
        suite_thread(pthread_condattr_init(&attr));
        suite_thread(pthread_condattr_setclock(&attr, CLOCK_MONOTONIC));
        suite_thread(pthread_cond_init(&condition, &attr));
        /* The producer completes before the waiter: the signal is intentionally not stored. */
        bool ready = true;
        suite_thread(pthread_cond_signal(&condition));
        struct timespec until;
        clock_gettime(CLOCK_MONOTONIC, &until);
        until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }
        suite_thread(pthread_mutex_lock(&lock));
        int error = 0;
        if (!fixed)
            error = pthread_cond_timedwait(&condition, &lock, &until);
        else
            while (!ready) {
                error = pthread_cond_timedwait(&condition, &lock, &until);
                if (error)
                    break;
            }
        suite_thread(pthread_mutex_unlock(&lock));
        r->observed = (unsigned)error;
        r->auxiliary = ready;
        r->completed = ready;
        r->checksum = ready ? 42 : 0;
        suite_thread(pthread_cond_destroy(&condition));
        suite_thread(pthread_condattr_destroy(&attr));
        suite_thread(pthread_mutex_destroy(&lock));
    } else if (kind == 2) {
        struct locks locks = {.a = PTHREAD_MUTEX_INITIALIZER, .fixed = fixed};
        pthread_t threads[2];
        suite_thread(pthread_barrier_init(&locks.barrier, NULL, 2));
        for (unsigned i = 0; i < 2; i++)
            suite_thread(pthread_create(&threads[i], NULL, update_worker, &locks));
        for (unsigned i = 0; i < 2; i++)
            suite_thread(pthread_join(threads[i], NULL));
        r->completed = 64;
        r->checksum = locks.value;
        r->observed = locks.value;
        r->after = locks.value;
        suite_thread(pthread_barrier_destroy(&locks.barrier));
        suite_thread(pthread_mutex_destroy(&locks.a));
    } else {
        int pair[2];
        r->before = suite_fds();
        suite_require(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair), "shutdown stream");
        struct reader reader = {.fd = pair[0]};
        pthread_t thread;
        suite_thread(pthread_create(&thread, NULL, blocked_reader, &reader));
        if (fixed)
            suite_require(!shutdown(pair[0], SHUT_RDWR), "wake receiver before join");
        struct timespec until;
        clock_gettime(CLOCK_REALTIME, &until);
        until.tv_nsec += 100000000;
        if (until.tv_nsec >= 1000000000) {
            until.tv_sec++;
            until.tv_nsec -= 1000000000;
        }
        int error = pthread_timedjoin_np(thread, NULL, &until);
        r->observed = (unsigned)error;
        if (error == ETIMEDOUT) {
            suite_require(!shutdown(pair[0], SHUT_RDWR), "cleanup blocked receiver");
            suite_thread(pthread_join(thread, NULL));
        } else
            suite_thread(error);
        r->auxiliary = reader.eof;
        r->checksum = reader.eof;
        r->completed = 1;
        close(pair[0]);
        close(pair[1]);
        r->after = suite_fds();
    }
    suite_event("wait_or_state_result", r->observed);
}
