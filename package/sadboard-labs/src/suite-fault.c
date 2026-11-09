#include "suite.h"
#include <assert.h>
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

__attribute__((noinline)) static unsigned stack_frame(unsigned depth)
{
    volatile unsigned char frame[4096];
    memset((void *)frame, (int)depth, sizeof(frame));
    unsigned value = depth ? stack_frame(depth - 1) : 0;
    return value + frame[depth & 4095];
}
static void *stack_worker(void *ignored)
{
    return (void *)(uintptr_t)stack_frame(*(unsigned *)ignored);
}
static void fault_child(bool fixed, unsigned kind)
{
    if (kind == 0) {
        volatile unsigned state = 2;
        if (fixed && state != 1)
            _exit(2);
        assert(state == 1);
    } else if (kind == 1) {
        execlp("valgrind", "valgrind", "--leak-check=full", "--error-exitcode=99",
               fixed ? "--log-file=overflow-fixed.valgrind" : "--log-file=overflow-bug.valgrind",
               suite_executable, "--overflow-child", fixed ? "fixed" : "bug", (char *)NULL);
        _exit(70);
    } else if (kind == 2) {
        /* The reference rejects an input exceeding the defined parser depth. */
        unsigned requested_depth = 64, maximum_depth = 8;
        if (fixed && requested_depth > maximum_depth)
            _exit(2);
        pthread_attr_t attr;
        pthread_t thread;
        suite_thread(pthread_attr_init(&attr));
        long minimum = sysconf(_SC_THREAD_STACK_MIN);
        suite_require(minimum > 0 && minimum <= 131072, "bounded platform stack minimum");
        size_t stack_bytes = minimum > 65536 ? (size_t)minimum : 65536;
        suite_event("thread_stack_bytes", stack_bytes);
        suite_thread(pthread_attr_setstacksize(&attr, stack_bytes));
        suite_thread(pthread_attr_setguardsize(&attr, (size_t)sysconf(_SC_PAGESIZE)));
        suite_thread(pthread_create(&thread, &attr, stack_worker, &requested_depth));
        suite_thread(pthread_join(thread, NULL));
        pthread_attr_destroy(&attr);
    } else {
        int fd = open("mapped-source.bin", O_CREAT | O_TRUNC | O_RDWR, 0600);
        suite_require(fd >= 0 && !ftruncate(fd, 8192), "mmap source");
        volatile unsigned char *data = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        suite_require(data != MAP_FAILED, "mmap");
        data[4096] = 42;
        if (fixed) {
            int replacement = open("mapped-next.bin", O_CREAT | O_TRUNC | O_RDWR, 0600);
            suite_require(replacement >= 0 && !ftruncate(replacement, 8192), "replacement");
            suite_require(pwrite(replacement, "*", 1, 4096) == 1, "replacement content");
            close(replacement);
            suite_require(!rename("mapped-next.bin", "mapped-source.bin"), "publish mapping");
        } else
            suite_require(!ftruncate(fd, 0), "truncate mapped inode");
        unsigned value = data[4096];
        munmap((void *)data, 8192);
        close(fd);
        _exit(value == 42 ? 0 : 70);
    }
    _exit(0);
}
void suite_fault(bool fixed, struct sample *r, unsigned kind)
{
    suite_event("fault_kind", kind);
    pid_t child = fork();
    suite_require(child >= 0, "fork fault");
    if (!child)
        fault_child(fixed, kind);
    bool timed;
    int status = suite_wait(child, kind == 1 ? 8000 : 2000, &timed);
    suite_require(!timed, "fault child timeout");
    r->completed = 1;
    r->observed = WIFSIGNALED(status) ? (unsigned)WTERMSIG(status) : 0;
    r->auxiliary = WIFEXITED(status) ? (unsigned)WEXITSTATUS(status) : 0;
    r->checksum = r->auxiliary;
    suite_event("child_pid", (uint64_t)child);
    suite_event("wait_status", (unsigned)status);
}
