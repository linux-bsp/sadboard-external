#include "suite.h"
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

struct later {
    int fd;
    bool event;
};
static void *delayed_write(void *argument)
{
    struct later *later = argument;
    lab_sleep_us(50000);
    uint64_t value = 42;
    suite_require(write(later->fd, &value, later->event ? sizeof(value) : 1) ==
                      (later->event ? (ssize_t)sizeof(value) : 1),
                  "delayed write");
    return NULL;
}
void suite_cpu(bool fixed, struct sample *r, unsigned kind)
{
    if (kind == 0) {
        int p[2];
        suite_require(!pipe2(p, O_NONBLOCK | O_CLOEXEC), "nonblocking pipe");
        if (!fixed) {
            unsigned char empty;
            suite_require(read(p[0], &empty, 1) < 0 && errno == EAGAIN, "initial EAGAIN");
            r->observed++;
            r->auxiliary++;
        }
        struct later later = {.fd = p[1]};
        pthread_t worker;
        suite_thread(pthread_create(&worker, NULL, delayed_write, &later));
        unsigned char byte = 0;
        for (;;) {
            if (fixed) {
                struct pollfd item = {.fd = p[0], .events = POLLIN};
                suite_require(poll(&item, 1, 1000) > 0, "wait readable");
            }
            ssize_t n = read(p[0], &byte, 1);
            r->auxiliary++;
            if (n == 1)
                break;
            suite_require(n < 0 && errno == EAGAIN, "expected EAGAIN");
            r->observed++;
        }
        r->completed = 1;
        r->checksum = byte;
        suite_thread(pthread_join(worker, NULL));
        close(p[0]);
        close(p[1]);
    } else if (kind == 1) {
        int fd = memfd_create("sadboard-logging", MFD_CLOEXEC);
        suite_require(fd >= 0, "log memfd");
        char batch[4096];
        size_t used = 0;
        for (unsigned i = 0; i < 2048; i++) {
            char line[64];
            int length = snprintf(line, sizeof(line), "event=%u value=%u\n", i, i * 3);
            suite_require(length > 0 && (size_t)length < sizeof(line), "format log");
            if (fixed) {
                memcpy(batch + used, line, (size_t)length);
                used += (size_t)length;
                if ((i + 1) % 32 == 0) {
                    suite_require(write(fd, batch, used) == (ssize_t)used, "batch write");
                    used = 0;
                    r->observed++;
                }
            } else {
                suite_require(write(fd, line, (size_t)length) == length, "record write");
                r->observed++;
            }
            r->completed++;
            r->checksum += i;
        }
        suite_require(!used && lseek(fd, 0, SEEK_SET) == 0, "rewind logs");
        FILE *input = fdopen(fd, "r");
        suite_require(input != NULL, "log stream");
        unsigned index, value, expected = 0;
        while (fscanf(input, "event=%u value=%u\n", &index, &value) == 2) {
            suite_require(index == expected && value == index * 3, "log contents");
            expected++;
        }
        r->auxiliary = expected;
        fclose(input);
    } else if (kind == 2) {
        int event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK), p[2];
        suite_require(event >= 0 && !pipe2(p, O_CLOEXEC | O_NONBLOCK), "event sources");
        int ep = epoll_create1(EPOLL_CLOEXEC);
        suite_require(ep >= 0, "epoll");
        struct epoll_event registration = {.events = EPOLLIN, .data.fd = event};
        suite_require(!epoll_ctl(ep, EPOLL_CTL_ADD, event, &registration), "register command");
        if (!fixed) {
            registration.events = EPOLLOUT;
            registration.data.fd = p[1];
            suite_require(!epoll_ctl(ep, EPOLL_CTL_ADD, p[1], &registration),
                          "always writable registration");
        }
        struct later later = {.fd = event, .event = true};
        pthread_t worker;
        suite_thread(pthread_create(&worker, NULL, delayed_write, &later));
        bool done = false;
        while (!done) {
            struct epoll_event events[2];
            int n = epoll_wait(ep, events, 2, 1000);
            suite_require(n > 0, "event wait");
            for (int i = 0; i < n; i++) {
                if (events[i].data.fd == event) {
                    uint64_t value;
                    suite_require(read(event, &value, sizeof(value)) == sizeof(value),
                                  "command read");
                    r->checksum = value;
                    r->completed++;
                    done = true;
                } else
                    r->observed++;
            }
        }
        suite_thread(pthread_join(worker, NULL));
        close(ep);
        close(event);
        close(p[0]);
        close(p[1]);
    } else {
        const unsigned side = 1024, passes = 4;
        size_t total = (size_t)side * side;
        uint32_t *matrix = malloc(total * sizeof(*matrix));
        suite_require(matrix != NULL, "matrix");
        for (size_t i = 0; i < total; i++)
            matrix[i] = (uint32_t)(i & 255);
        uintptr_t first = 0;
        unsigned position = 0;
        for (unsigned pass = 0; pass < passes; pass++)
            for (unsigned outer = 0; outer < side; outer++)
                for (unsigned inner = 0; inner < side; inner++) {
                    size_t index =
                        fixed ? (size_t)outer * side + inner : (size_t)inner * side + outer;
                    if (position == 0)
                        first = (uintptr_t)&matrix[index];
                    else if (position == 1)
                        r->observed = (uintptr_t)&matrix[index] - first;
                    if (position < 2)
                        position++;
                    r->checksum += matrix[index];
                    r->completed++;
                }
        r->auxiliary = total * sizeof(*matrix);
        free(matrix);
    }
    suite_event("operation_count", r->observed);
}
