#include "suite-advanced.h"
#include <dirent.h>
#include <fcntl.h>
#include <linux/sock_diag.h>
#include <malloc.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
extern void suite_tcp_pair(int pair[2]);
static unsigned tasks(void)
{
    DIR *d = opendir("/proc/self/task");
    suite_require(d != NULL, "thread directory");
    unsigned n = 0;
    struct dirent *e;
    while ((e = readdir(d)))
        if (e->d_name[0] != '.')
            n++;
    closedir(d);
    return n;
}
static void fd_send(int socket, int fd)
{
    char byte = 42, control[CMSG_SPACE(sizeof(int))];
    memset(control, 0, sizeof(control));
    struct iovec iov = {&byte, 1};
    struct msghdr message = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    c->cmsg_level = SOL_SOCKET;
    c->cmsg_type = SCM_RIGHTS;
    c->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(c), &fd, sizeof(fd));
    suite_require(sendmsg(socket, &message, 0) == 1, "pass shared buffer descriptor");
}
static int fd_receive(int socket)
{
    char byte, control[CMSG_SPACE(sizeof(int))];
    struct iovec iov = {&byte, 1};
    struct msghdr message = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    suite_require(recvmsg(socket, &message, MSG_CMSG_CLOEXEC) == 1 &&
                      !(message.msg_flags & MSG_CTRUNC),
                  "receive buffer descriptor");
    struct cmsghdr *c = CMSG_FIRSTHDR(&message);
    suite_require(c && c->cmsg_level == SOL_SOCKET && c->cmsg_type == SCM_RIGHTS &&
                      c->cmsg_len == CMSG_LEN(sizeof(int)),
                  "SCM_RIGHTS envelope");
    int fd;
    memcpy(&fd, CMSG_DATA(c), sizeof(fd));
    return fd;
}
static uint64_t socket_memory(int fd)
{
    uint32_t values[SK_MEMINFO_VARS] = {0};
    socklen_t length = sizeof(values);
    suite_require(!getsockopt(fd, SOL_SOCKET, SO_MEMINFO, values, &length),
                  "actual kernel socket memory");
    return values[SK_MEMINFO_RMEM_ALLOC];
}
static _Thread_local unsigned char thread_state[16384];
struct thread_pool {
    pthread_barrier_t ready, release;
    pthread_mutex_t lock;
    unsigned count, completed;
    uint64_t sum;
};
struct thread_job {
    struct thread_pool *pool;
    unsigned number;
};
static void *memory_worker(void *arg)
{
    struct thread_job *job = arg;
    struct thread_pool *p = job->pool;
    volatile unsigned char stack[131072];
    memset((void *)stack, 7, sizeof(stack));
    memset(thread_state, 3, sizeof(thread_state));
    pthread_barrier_wait(&p->ready);
    pthread_barrier_wait(&p->release);
    unsigned done = 0;
    uint64_t sum = 0;
    for (unsigned i = job->number; i < 12; i += p->count) {
        thread_state[0] = (unsigned char)i;
        sum += thread_state[0] + stack[0] - 7;
        done++;
    }
    suite_thread(pthread_mutex_lock(&p->lock));
    p->completed += done;
    p->sum += sum;
    suite_thread(pthread_mutex_unlock(&p->lock));
    return NULL;
}
void suite_adv_mem(bool fixed, struct sample *r, unsigned kind)
{
    if (kind == 0) {
        suite_require(mallopt(M_TRIM_THRESHOLD, -1) != 0,
                      "disable automatic trim only in this process");
        unsigned char *blocks[8192];
        uintptr_t pages[8192];
        size_t page_count = 0;
        long page_size = sysconf(_SC_PAGESIZE);
        suite_require(page_size > 0, "allocator page size");
        struct mallinfo2 initial = mallinfo2();
        r->before = initial.uordblks;
        for (unsigned i = 0; i < 8192; i++) {
            blocks[i] = malloc(1024);
            suite_require(blocks[i] != NULL, "allocator objects");
            memset(blocks[i], (int)(i & 255), 1024);
            uintptr_t page = (uintptr_t)blocks[i] & ~((uintptr_t)page_size - 1);
            bool known = false;
            for (size_t j = 0; j < page_count; ++j)
                if (pages[j] == page) { known = true; break; }
            if (!known) pages[page_count++] = page;
        }
        for (unsigned i = 0; i < 8192; i++) {
            r->checksum += blocks[i][0];
            r->completed++;
            free(blocks[i]);
        }
        if (fixed)
            malloc_trim(0);
        struct mallinfo2 after = mallinfo2();
        /* malloc_trim may MADV_DONTNEED arena interiors without shrinking
         * fordblks (notably AArch64 glibc). Measure the actual formerly-owned
         * pages instead of assuming virtual free arena bytes become zero. */
        for (size_t i = 0; i < page_count; ++i) {
            unsigned char resident = 0;
            int result = mincore((void *)pages[i], (size_t)page_size, &resident);
            suite_require(result == 0 || errno == ENOMEM, "freed arena mincore");
            if (result == 0 && (resident & 1)) r->observed += (uint64_t)page_size;
        }
        r->auxiliary = after.uordblks;
        r->after = suite_status("VmRSS:");
        suite_event("allocator_free_bytes", after.fordblks);
        suite_event("allocator_live_bytes", after.uordblks);
        suite_event("freed_arena_resident_bytes", r->observed);
    } else if (kind == 1) {
        const size_t bytes = 262144;
        unsigned char *pool[4] = {0};
        uint64_t live = 0, peak = 0;
        if (fixed)
            for (unsigned i = 0; i < 4; i++) {
                pool[i] = malloc(bytes);
                suite_require(pool[i] != NULL, "frame pool");
                r->auxiliary++;
                live += bytes;
            }
        peak = live;
        for (unsigned batch = 0; batch < 16; batch++) {
            unsigned char *stages[4][3];
            for (unsigned i = 0; i < 4; i++) {
                if (fixed)
                    stages[i][0] = stages[i][1] = stages[i][2] = pool[i];
                else
                    for (unsigned j = 0; j < 3; j++) {
                        stages[i][j] = malloc(bytes);
                        suite_require(stages[i][j] != NULL, "pipeline frame");
                        r->auxiliary++;
                        live += bytes;
                        if (live > peak)
                            peak = live;
                    }
                memset(stages[i][0], (int)(batch * 4 + i), bytes);
                if (!fixed)
                    for (unsigned j = 1; j < 3; j++) {
                        memcpy(stages[i][j], stages[i][j - 1], bytes);
                        r->observed += bytes;
                    }
            }
            for (unsigned i = 0; i < 4; i++) {
                for (size_t j = 0; j < bytes; j++)
                    r->checksum += stages[i][2][j];
                r->completed++;
                if (!fixed)
                    for (unsigned j = 0; j < 3; j++) {
                        free(stages[i][j]);
                        live -= bytes;
                    }
            }
            suite_event("live_frame_bytes", live);
        }
        if (fixed)
            for (unsigned i = 0; i < 4; i++) {
                free(pool[i]);
                live -= bytes;
            }
        r->before = peak;
        r->after = live;
    } else if (kind == 2) {
        struct suite_cgroup cg;
        suite_cgroup_create(&cg, fixed ? "memory-high-fixed" : "memory-high-bug");
        suite_cgroup_set(&cg, "memory.max", "33554432");
        int gate[2], ready[2], result[2];
        suite_require(!pipe(gate) && !pipe(ready) && !pipe(result), "memory pressure pipes");
        pid_t child = fork();
        suite_require(child >= 0, "memory pressure child");
        if (!child) {
            suite_child_guard();
            close(gate[1]);
            close(ready[0]);
            close(result[0]);
            char byte;
            suite_require(read(gate[0], &byte, 1) == 1, "memory pressure start");
            size_t page = (size_t)sysconf(_SC_PAGESIZE), bytes = fixed ? 4194304 : 16777216;
            unsigned char *memory =
                mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            suite_require(memory != MAP_FAILED, "pressure mapping");
            madvise(memory, bytes, MADV_NOHUGEPAGE);
            uint64_t sum = 0;
            for (unsigned i = 0; i < 4096; i++) {
                size_t offset = ((size_t)i * page) % bytes;
                memory[offset] = (unsigned char)(i & 255);
                sum += memory[offset];
            }
            suite_require(write(ready[1], &byte, 1) == 1 && read(gate[0], &byte, 1) == 1,
                          "pressure prepared handshake");
            volatile unsigned char *extra =
                mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
            suite_require(extra != MAP_FAILED, "pressure final page");
            *extra = 42;
            suite_require(write(result[1], &sum, sizeof(sum)) == sizeof(sum), "pressure result");
            suite_require(read(gate[0], &byte, 1) == 1, "hold pressure for measurement");
            _exit(0);
        }
        close(gate[0]);
        close(ready[1]);
        close(result[1]);
        suite_cgroup_join(&cg, child);
        char byte = 42;
        suite_require(write(gate[1], &byte, 1) == 1 && read(ready[0], &byte, 1) == 1,
                      "prepared working set");
        close(ready[0]);
        r->before = suite_cgroup_value(&cg, "memory.current", NULL);
        suite_cgroup_set(&cg, "memory.high", "8388608");
        suite_require(write(gate[1], &byte, 1) == 1, "cause post-high allocation");
        uint64_t sum;
        suite_require(read(result[0], &sum, sizeof(sum)) == sizeof(sum), "reclaim completion");
        close(result[0]);
        r->observed = suite_cgroup_value(&cg, "memory.events", "high");
        r->auxiliary = suite_cgroup_value(&cg, "memory.events", "oom_kill");
        r->after = suite_cgroup_value(&cg, "memory.peak", NULL);
        r->completed = 4096;
        r->checksum = sum;
        suite_require(write(gate[1], &byte, 1) == 1, "release measured process");
        close(gate[1]);
        bool timed;
        int status = suite_wait(child, 1000, &timed);
        suite_require(!timed && WIFEXITED(status) && !WEXITSTATUS(status), "memory child exit");
        suite_cgroup_remove(&cg);
    } else if (kind == 3) {
        r->before = suite_fds();
        int sockets[2], result[2];
        suite_require(!socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sockets) &&
                          !pipe(result),
                      "pool session channels");
        pid_t child = fork();
        suite_require(child >= 0, "shared session fork");
        if (!child) {
            close(sockets[0]);
            close(result[0]);
            uint64_t sum = 0;
            for (unsigned i = 0; i < 8; i++) {
                int fd = fd_receive(sockets[1]);
                unsigned char value;
                suite_require(pread(fd, &value, 1, 0) == 1 && value == i, "shared frame bytes");
                sum += value;
                close(fd);
            }
            suite_require(write(result[1], &sum, sizeof(sum)) == sizeof(sum), "consumer proof");
            _exit(0);
        }
        close(sockets[1]);
        close(result[1]);
        unsigned occupied = 0, generation = 1;
        for (unsigned i = 0; i < 8; i++) {
            int fd = memfd_create("shared-frame", MFD_CLOEXEC);
            unsigned char byte = (unsigned char)i;
            suite_require(fd >= 0 && write(fd, &byte, 1) == 1, "shared frame");
            fd_send(sockets[0], fd);
            close(fd);
            occupied |= 1U << i;
            r->completed++;
        }
        suite_require(read(result[0], &r->checksum, sizeof(r->checksum)) == sizeof(r->checksum),
                      "consumer checksum");
        close(result[0]);
        close(sockets[0]);
        bool timed;
        int status = suite_wait(child, 1000, &timed);
        suite_require(!timed && WIFEXITED(status) && !WEXITSTATUS(status), "consumer proven dead");
        if (fixed) {
            generation++;
            occupied = 0;
            unsigned stale_generation = 1;
            if (stale_generation != generation)
                r->auxiliary++;
        }
        for (unsigned i = 0; i < 8; i++)
            r->observed += !!(occupied & (1U << i));
        r->after = suite_fds();
        suite_event("session_generation", generation);
    } else if (kind == 4) {
        r->before = suite_fds();
        int pairs[16][2];
        unsigned char buffer[4096];
        memset(buffer, 42, sizeof(buffer));
        if (!fixed)
            for (unsigned i = 0; i < 16; i++) {
                suite_tcp_pair(pairs[i]);
                int size = 131072;
                suite_require(
                    !setsockopt(pairs[i][0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) &&
                        !setsockopt(pairs[i][1], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)),
                    "socket budgets");
            }
        for (unsigned i = 0; i < 16; i++) {
            int *pair = pairs[i];
            if (fixed)
                suite_tcp_pair(pair);
            for (unsigned block = 0; block < 16; block++) {
                suite_require(send(pair[0], buffer, sizeof(buffer), MSG_NOSIGNAL) == sizeof(buffer),
                              "socket payload");
                if (fixed) {
                    unsigned char received[4096];
                    suite_require(recv(pair[1], received, sizeof(received), MSG_WAITALL) ==
                                      sizeof(received),
                                  "bounded socket drain");
                    for (unsigned j = 0; j < sizeof(received); j++) {
                        suite_require(received[j] == 42, "socket data");
                        r->checksum += received[j];
                    }
                    r->completed += sizeof(received);
                }
            }
            if (fixed) {
                r->observed += socket_memory(pair[1]);
                close(pair[0]);
                close(pair[1]);
            }
        }
        if (!fixed) {
            lab_sleep_us(10000);
            for (unsigned i = 0; i < 16; i++)
                r->observed += socket_memory(pairs[i][1]);
            for (unsigned i = 0; i < 16; i++) {
                for (unsigned block = 0; block < 16; block++) {
                    unsigned char received[4096];
                    suite_require(recv(pairs[i][1], received, sizeof(received), MSG_WAITALL) ==
                                      sizeof(received),
                                  "delayed socket drain");
                    for (unsigned j = 0; j < sizeof(received); j++) {
                        suite_require(received[j] == 42, "delayed socket data");
                        r->checksum += received[j];
                    }
                    r->completed += sizeof(received);
                }
                close(pairs[i][0]);
                close(pairs[i][1]);
            }
        }
        r->auxiliary = fixed ? 1 : 16;
        r->after = suite_fds();
    } else {
        struct thread_pool p = {.count = fixed ? 4 : 12, .lock = PTHREAD_MUTEX_INITIALIZER};
        suite_thread(pthread_barrier_init(&p.ready, NULL, p.count + 1));
        suite_thread(pthread_barrier_init(&p.release, NULL, p.count + 1));
        struct thread_job jobs[12];
        pthread_t threads[12];
        pthread_attr_t attr;
        suite_thread(pthread_attr_init(&attr));
        suite_thread(pthread_attr_setstacksize(&attr, 524288));
        r->before = suite_status("VmRSS:");
        for (unsigned i = 0; i < p.count; i++) {
            jobs[i] = (struct thread_job){&p, i};
            suite_thread(pthread_create(&threads[i], &attr, memory_worker, &jobs[i]));
        }
        pthread_barrier_wait(&p.ready);
        r->observed = tasks() - 1;
        r->auxiliary = suite_status("VmSize:");
        r->after = suite_status("VmRSS:");
        pthread_barrier_wait(&p.release);
        for (unsigned i = 0; i < p.count; i++)
            suite_thread(pthread_join(threads[i], NULL));
        r->completed = p.completed;
        r->checksum = p.sum;
        pthread_barrier_destroy(&p.ready);
        pthread_barrier_destroy(&p.release);
        pthread_mutex_destroy(&p.lock);
        pthread_attr_destroy(&attr);
    }
}
