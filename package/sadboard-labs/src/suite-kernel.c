#include "suite-advanced.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <unistd.h>

static void kernel_cow(bool fixed, struct sample *r)
{
    size_t page = (size_t)sysconf(_SC_PAGESIZE);
    unsigned char *memory = mmap(NULL, page, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    suite_require(memory != MAP_FAILED, "private COW mapping");
    memory[0] = 42;
    int result[2];
    suite_require(!pipe2(result, O_CLOEXEC), "COW result pipe");
    pid_t child = fork();
    suite_require(child >= 0, "COW fork");
    if (!child) {
        suite_child_guard();
        close(result[0]);
        memory[0] = 99;
        struct rusage usage;
        suite_require(!getrusage(RUSAGE_SELF, &usage), "COW child rusage");
        uint64_t values[2] = {memory[0], (uint64_t)usage.ru_minflt};
        suite_require(write(result[1], values, sizeof(values)) == sizeof(values), "COW report");
        _exit(0);
    }
    close(result[1]);
    uint64_t values[2];
    suite_require(read(result[0], values, sizeof(values)) == sizeof(values), "COW report read");
    close(result[0]);
    bool timed;
    int status = suite_wait(child, 1500, &timed);
    suite_require(!timed && WIFEXITED(status) && !WEXITSTATUS(status), "COW child exit");
    r->completed = 1;
    r->checksum = 99;
    r->observed = fixed ? values[0] : memory[0];
    r->auxiliary = values[0];
    r->before = 42;
    r->after = memory[0];
    suite_event("child_minor_faults_total_not_delta", values[1]);
    suite_event("parent_private_value", memory[0]);
    suite_event("child_private_value", values[0]);
    suite_require(!munmap(memory, page), "COW unmap");
}

static void kernel_rename(bool fixed, struct sample *r)
{
    int old = open("rename-current.bin", O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    int next = open("rename-next.bin", O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
    suite_require(old >= 0 && next >= 0, "rename private files");
    unsigned char first = 7, second = 9, current;
    suite_require(write(old, &first, 1) == 1 && write(next, &second, 1) == 1, "rename contents");
    struct stat a, b;
    suite_require(!fstat(old, &a), "old inode");
    close(next);
    suite_require(!rename("rename-next.bin", "rename-current.bin"), "atomic rename");
    int published = open("rename-current.bin", O_RDONLY | O_CLOEXEC);
    suite_require(published >= 0 && !fstat(published, &b), "published inode");
    suite_require(pread(fixed ? published : old, &current, 1, 0) == 1, "selected open file");
    r->completed = 1;
    r->checksum = 9;
    r->observed = current;
    r->auxiliary = a.st_ino != b.st_ino;
    r->before = first;
    suite_require(pread(published, &current, 1, 0) == 1, "new pathname content");
    r->after = current;
    suite_event("old_inode", (uint64_t)a.st_ino);
    suite_event("published_inode", (uint64_t)b.st_ino);
    close(old);
    close(published);
}

static void kernel_eventfd(bool fixed, struct sample *r)
{
    int fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    suite_require(fd >= 0, "counter eventfd");
    uint64_t one = 1, two = 2, counter = 0;
    suite_require(write(fd, &one, sizeof(one)) == sizeof(one) &&
                      write(fd, &two, sizeof(two)) == sizeof(two), "eventfd notifications");
    suite_require(read(fd, &counter, sizeof(counter)) == sizeof(counter), "eventfd counter read");
    r->completed = fixed ? counter : 1;
    r->checksum = one + two;
    r->observed = counter;
    r->auxiliary = 1;
    r->before = one + two;
    uint64_t empty;
    suite_require(read(fd, &empty, sizeof(empty)) < 0 && errno == EAGAIN, "eventfd drained");
    r->after = 0;
    suite_event("notification_write_calls", 2);
    suite_event("counter_units_read", counter);
    close(fd);
}

static void kernel_timerfd(bool fixed, struct sample *r)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    suite_require(fd >= 0, "monotonic timerfd");
    struct itimerspec timer = {
        .it_value.tv_nsec = 10000000,
        .it_interval.tv_nsec = 10000000,
    };
    suite_require(!timerfd_settime(fd, 0, &timer, NULL), "periodic timerfd arm");
    lab_sleep_us(55000);
    struct pollfd p = {.fd = fd, .events = POLLIN};
    suite_require(poll(&p, 1, 1000) > 0, "periodic timer readable");
    uint64_t expirations;
    suite_require(read(fd, &expirations, sizeof(expirations)) == sizeof(expirations), "timerfd count");
    suite_require(expirations > 1, "multiple actual expirations");
    r->completed = 1;
    r->checksum = expirations;
    r->observed = fixed ? expirations : 1;
    r->auxiliary = expirations;
    r->before = 10000000;
    r->after = expirations - r->observed;
    suite_event("actual_expirations", expirations);
    suite_event("handled_ticks", r->observed);
    memset(&timer, 0, sizeof(timer));
    suite_require(!timerfd_settime(fd, 0, &timer, NULL), "timerfd disarm");
    close(fd);
}

static bool zombie_exists(pid_t child)
{
    char path[128], line[1024];
    snprintf(path, sizeof(path), "/proc/%ld/stat", (long)child);
    FILE *file = fopen(path, "r");
    if (!file) {
        suite_require(errno == ENOENT, "child proc stat");
        return false;
    }
    suite_require(fgets(line, sizeof(line), file) != NULL, "read child stat");
    fclose(file);
    char *end = strrchr(line, ')');
    suite_require(end && end[1] == ' ', "parse child state");
    return end[2] == 'Z';
}

static void kernel_zombie(bool fixed, struct sample *r)
{
    pid_t child = fork();
    suite_require(child >= 0, "zombie fork");
    if (!child)
        _exit(7);
    siginfo_t information;
    memset(&information, 0, sizeof(information));
    suite_require(!waitid(P_PID, (id_t)child, &information, WEXITED | WNOWAIT), "observe unreaped child");
    suite_require(information.si_code == CLD_EXITED && information.si_status == 7, "child exit reason");
    r->before = zombie_exists(child);
    suite_require(r->before == 1, "exited child is a zombie before wait");
    int status = 0;
    if (fixed)
        suite_require(waitpid(child, &status, 0) == child, "reap child");
    r->observed = zombie_exists(child);
    if (!fixed)
        suite_require(waitpid(child, &status, 0) == child, "cleanup unreaped child");
    suite_require(WIFEXITED(status) && WEXITSTATUS(status) == 7, "reaped exit status");
    r->completed = 1;
    r->checksum = 7;
    r->auxiliary = WEXITSTATUS(status);
    r->after = zombie_exists(child);
    suite_event("owned_child_pid", (uint64_t)child);
    suite_event("observed_zombie_before_cleanup", r->observed);
}

static void kernel_datagram(bool fixed, struct sample *r)
{
    int receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    int sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    suite_require(receiver >= 0 && sender >= 0, "owned UDP sockets");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    suite_require(!bind(receiver, (struct sockaddr *)&address, size) &&
                      !getsockname(receiver, (struct sockaddr *)&address, &size), "ephemeral UDP port");
    unsigned char payload[128], received[128];
    for (unsigned i = 0; i < sizeof(payload); i++)
        payload[i] = (unsigned char)i;
    suite_require(sendto(sender, payload, sizeof(payload), 0, (struct sockaddr *)&address, size) ==
                      sizeof(payload), "one whole UDP datagram");
    struct pollfd ready = {.fd = receiver, .events = POLLIN};
    suite_require(poll(&ready, 1, 1000) > 0, "UDP receive readiness");
    struct iovec iov = {.iov_base = received, .iov_len = fixed ? sizeof(received) : 16};
    struct msghdr message = {.msg_iov = &iov, .msg_iovlen = 1};
    ssize_t n = recvmsg(receiver, &message, 0);
    suite_require(n > 0 && n <= 128, "UDP read size");
    for (ssize_t i = 0; i < n; i++)
        r->checksum += received[i];
    r->completed = (uint64_t)n;
    r->observed = !!(message.msg_flags & MSG_TRUNC);
    r->auxiliary = sizeof(payload);
    suite_event("datagram_sent_bytes", sizeof(payload));
    suite_event("application_buffer_bytes", iov.iov_len);
    suite_event("recvmsg_flags", (unsigned)message.msg_flags);
    close(sender);
    close(receiver);
}

static void kernel_pipe(bool fixed, struct sample *r)
{
    int data[2], release[2];
    suite_require(!pipe2(data, O_CLOEXEC) && !pipe2(release, O_CLOEXEC), "bounded frame pipes");
    pid_t child = fork();
    suite_require(child >= 0, "pipe writer fork");
    if (!child) {
        suite_child_guard();
        close(data[0]);
        close(release[1]);
        unsigned char frame[12], go;
        for (unsigned i = 0; i < sizeof(frame); i++)
            frame[i] = (unsigned char)i;
        suite_require(write(data[1], frame, 3) == 3, "first frame fragment");
        suite_require(read(release[0], &go, 1) == 1, "release frame tail");
        suite_require(write(data[1], frame + 3, 9) == 9, "remaining frame fragment");
        _exit(0);
    }
    close(data[1]);
    close(release[0]);
    unsigned char frame[12], go = 1;
    ssize_t first = read(data[0], frame, sizeof(frame));
    suite_require(first == 3, "deterministic successful short read");
    suite_require(write(release[1], &go, 1) == 1, "allow tail writer");
    close(release[1]);
    size_t used = (size_t)first;
    if (fixed) {
        while (used < sizeof(frame)) {
            ssize_t n = read(data[0], frame + used, sizeof(frame) - used);
            suite_require(n > 0, "accumulate full frame");
            used += (size_t)n;
        }
    }
    r->completed = used;
    for (size_t i = 0; i < used; i++)
        r->checksum += frame[i];
    r->observed = (uint64_t)first;
    r->auxiliary = sizeof(frame);
    bool timed;
    int status = suite_wait(child, 1500, &timed);
    /* Keep the reader alive until the writer completes, including bug mode. */
    suite_require(!timed && WIFEXITED(status) && !WEXITSTATUS(status), "frame writer exit");
    close(data[0]);
    suite_event("first_read_bytes", (uint64_t)first);
    suite_event("processed_frame_bytes", used);
}

static void kernel_affinity(bool fixed, struct sample *r)
{
    cpu_set_t original, selected, restored;
    suite_require(!sched_getaffinity(0, sizeof(original), &original), "allowed CPU mask");
    int first = -1;
    for (int cpu = 0; cpu < CPU_SETSIZE; cpu++)
        if (CPU_ISSET(cpu, &original)) {
            first = cpu;
            break;
        }
    suite_require(first >= 0, "at least one allowed CPU");
    CPU_ZERO(&selected);
    if (fixed)
        CPU_SET(first, &selected);
    /* A failed logical-to-CPU lookup produced an empty mask in the defective path. */
    errno = 0;
    int result = sched_setaffinity(0, sizeof(selected), &selected);
    r->observed = result ? (unsigned)errno : 0;
    if (fixed)
        suite_require(!result && sched_getcpu() == first, "allowed CPU selected");
    suite_require(!sched_setaffinity(0, sizeof(original), &original), "restore original affinity");
    suite_require(!sched_getaffinity(0, sizeof(restored), &restored) && CPU_EQUAL(&original, &restored),
                  "affinity restored exactly");
    r->completed = 1;
    r->checksum = 1;
    r->auxiliary = (unsigned)first;
    r->before = (unsigned)CPU_COUNT(&original);
    r->after = (unsigned)CPU_COUNT(&restored);
    suite_event("allowed_first_cpu", (unsigned)first);
    suite_event("selected_mask_cpu_count", fixed ? 1 : 0);
}

void suite_kernel(bool fixed, struct sample *r, unsigned kind)
{
    suite_event("kernel_subsystem_variant", kind);
    switch (kind) {
    case 0: kernel_cow(fixed, r); break;
    case 1: kernel_rename(fixed, r); break;
    case 2: kernel_eventfd(fixed, r); break;
    case 3: kernel_timerfd(fixed, r); break;
    case 4: kernel_zombie(fixed, r); break;
    case 5: kernel_datagram(fixed, r); break;
    case 6: kernel_pipe(fixed, r); break;
    case 7: kernel_affinity(fixed, r); break;
    default: suite_require(false, "unknown kernel teaching variant");
    }
}
