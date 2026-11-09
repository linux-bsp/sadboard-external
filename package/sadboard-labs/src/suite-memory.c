#include "suite.h"
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

void suite_mem(bool fixed, struct sample *r, unsigned kind)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    if (kind == 0) {
        int p[2];
        suite_require(!pipe(p), "result pipe");
        pid_t child = fork();
        suite_require(child >= 0, "fork ENOMEM");
        if (!child) {
            close(p[0]);
            struct rlimit limit = {.rlim_cur = 16 * 1024 * 1024, .rlim_max = 16 * 1024 * 1024};
            suite_require(!setrlimit(RLIMIT_AS, &limit), "address space limit");
            void *memory = mmap(NULL, 64 * 1024 * 1024, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            int error = memory == MAP_FAILED ? errno : 0;
            suite_require(write(p[1], &error, sizeof(error)) == sizeof(error), "allocation errno");
            close(p[1]);
            if (fixed && memory == MAP_FAILED)
                _exit(0);
            volatile unsigned char *unchecked = memory == MAP_FAILED ? NULL : memory;
            *unchecked = 42;
            _exit(70);
        }
        close(p[1]);
        int error;
        suite_require(read(p[0], &error, sizeof(error)) == sizeof(error), "read allocation errno");
        close(p[0]);
        bool timed;
        int status = suite_wait(child, 1000, &timed);
        suite_require(!timed, "ENOMEM child");
        r->observed = (unsigned)error;
        r->auxiliary = WIFSIGNALED(status) ? (unsigned)WTERMSIG(status) : 0;
        r->completed = 1;
        r->checksum = (unsigned)error;
        suite_event("rlimit_as", 16 * 1024 * 1024);
        suite_event("wait_status", (unsigned)status);
    } else if (kind == 1) {
        size_t total = 32 * 1024 * 1024, used = 4 * 1024 * 1024;
        r->before = suite_status("VmRSS:");
        unsigned char *memory =
            mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        suite_require(memory != MAP_FAILED, "reserve mapping");
        unsigned char *resident = calloc(total / page, 1);
        suite_require(resident != NULL, "mincore vector");
        suite_require(!mincore(memory, total, resident), "untouched mincore");
        unsigned count = 0;
        for (size_t i = 0; i < total / page; i++)
            count += !!(resident[i] & 1);
        suite_event("reserved_untouched_pages", count);
        for (size_t i = 0; i < used; i += page) {
            memory[i] = 7;
            r->completed++;
            r->checksum += memory[i];
        }
        suite_require(!mincore(memory, total, resident), "touched mincore");
        for (size_t i = 0; i < total / page; i++)
            r->auxiliary += !!(resident[i] & 1) * page;
        if (fixed)
            suite_require(!munmap(memory + used, total - used), "release unused reservation");
        r->observed = fixed ? used : total;
        r->after = suite_status("VmRSS:");
        suite_event("resident_payload_bytes", r->auxiliary);
        suite_require(!munmap(memory, fixed ? used : total), "release mapping");
        free(resident);
    } else if (kind == 2) {
        r->before = suite_fds();
        char path[128];
        snprintf(path, sizeof(path), "/dev/shm/sadboard-%ld", (long)getpid());
        int fd = open(path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
        suite_require(fd >= 0 && !ftruncate(fd, 8 * 1024 * 1024), "shared tmpfs file");
        suite_require(!unlink(path), "unlink shared name");
        suite_event("name_removed", access(path, F_OK) < 0 && errno == ENOENT);
        unsigned char *p = mmap(NULL, 8 * 1024 * 1024, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        suite_require(p != MAP_FAILED, "shmem mapping");
        for (size_t i = 0; i < 8 * 1024 * 1024; i += page) {
            p[i] = 6;
            r->checksum += p[i];
            r->completed++;
        }
        if (fixed) {
            suite_require(!munmap(p, 8 * 1024 * 1024), "unmap shared");
            close(fd);
        }
        r->observed = fcntl(fd, F_GETFD) >= 0;
        r->auxiliary = suite_fds() - r->before;
        suite_event("retained_memfd", r->observed);
        if (!fixed) {
            munmap(p, 8 * 1024 * 1024);
            close(fd);
        }
        r->after = suite_fds();
    } else {
        int p[2];
        suite_require(!pipe(p), "signal pipe");
        pid_t child = fork();
        suite_require(child >= 0, "fork SIGKILL");
        if (!child) {
            close(p[1]);
            char byte;
            ssize_t n = read(p[0], &byte, 1);
            _exit(n == 1 && byte == 42 ? 0 : 70);
        }
        close(p[0]);
        if (fixed) {
            char byte = 42;
            suite_require(write(p[1], &byte, 1) == 1, "graceful shutdown");
        } else
            suite_require(!kill(child, SIGKILL), "external SIGKILL");
        close(p[1]);
        bool timed;
        int status = suite_wait(child, 1000, &timed);
        suite_require(!timed, "shutdown child");
        r->observed = WIFSIGNALED(status) ? (unsigned)WTERMSIG(status) : 0;
        r->auxiliary = WIFEXITED(status) ? (unsigned)WEXITSTATUS(status) : 0;
        r->completed = 1;
        r->checksum = 42;
        suite_event("operator_requested_signal", fixed ? 0 : SIGKILL);
        suite_event("wait_status", (unsigned)status);
    }
}
