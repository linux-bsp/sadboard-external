#include "suite.h"
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

char suite_executable[4096];
FILE *suite_trace;
#define CASE(symbol, command, id, description, group, variant)                                     \
    static void symbol(bool fixed, struct sample *r)                                               \
    {                                                                                              \
        suite_##group(fixed, r, variant);                                                          \
    }
#include "basic-cases.def"
#undef CASE
const struct suite_case suite_cases[] = {
#define CASE(symbol, command, id, description, group, variant) {command, id, description, symbol},
#include "basic-cases.def"
#undef CASE
};
const size_t suite_case_count = sizeof(suite_cases) / sizeof(suite_cases[0]);

void suite_require(bool condition, const char *reason)
{
    if (!condition) {
        perror(reason);
        exit(70);
    }
}
void suite_thread(int error)
{
    if (error) {
        errno = error;
        perror("pthread");
        exit(70);
    }
}
void suite_event(const char *name, uint64_t value)
{
    fprintf(suite_trace, "%s,%" PRIu64 "\n", name, value);
    suite_require(!fflush(suite_trace), "trace flush");
}
unsigned suite_fds(void)
{
    DIR *dir = opendir("/proc/self/fd");
    suite_require(dir != NULL, "fd directory");
    unsigned count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)))
        if (entry->d_name[0] != '.')
            count++;
    closedir(dir);
    return count - 1;
}
uint64_t suite_status(const char *key)
{
    FILE *f = fopen("/proc/self/status", "r");
    suite_require(f != NULL, "status");
    char line[256];
    uint64_t value = 0;
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, key, strlen(key))) {
            value = strtoull(line + strlen(key), NULL, 10) * 1024;
            break;
        }
    fclose(f);
    return value;
}
int suite_wait(pid_t child, unsigned milliseconds, bool *timed_out)
{
    uint64_t end = lab_clock(CLOCK_MONOTONIC) + (uint64_t)milliseconds * 1000000;
    int status;
    *timed_out = false;
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child)
            return status;
        suite_require(done == 0 || errno == EINTR, "waitpid");
        if (lab_clock(CLOCK_MONOTONIC) >= end)
            break;
        lab_sleep_us(1000);
    }
    *timed_out = true;
    suite_require(!kill(child, SIGKILL), "kill experiment child");
    while (waitpid(child, &status, 0) < 0)
        suite_require(errno == EINTR, "wait killed child");
    return status;
}

static int overflow_child(bool fixed)
{
    volatile unsigned char *p = malloc(32);
    if (!p)
        return 70;
    for (unsigned i = 0; i < (fixed ? 32U : 33U); i++)
        p[i] = (unsigned char)i;
    unsigned sum = 0;
    for (unsigned i = 0; i < 32; i++)
        sum += p[i];
    free((void *)p);
    return sum == 496 ? 0 : 70;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "--overflow-child"))
        return overflow_child(!strcmp(argv[2], "fixed"));
    if (argc == 4 && !strcmp(argv[1], "--hold-writer")) {
        int ready = (int)lab_number(argv[2], 4096, 0), release = (int)lab_number(argv[3], 4096, 0);
        char byte = 42;
        if (write(ready, &byte, 1) != 1 || read(release, &byte, 1) != 1 || byte != 42)
            return 70;
        close(ready);
        close(release);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "--has")) {
        for (size_t i = 0; i < suite_case_count; i++)
            if (!strcmp(argv[2], suite_cases[i].name))
                return 0;
        return 64;
    }
    if (argc == 2 && !strcmp(argv[1], "--list")) {
        for (size_t i = 0; i < suite_case_count; i++)
            printf("%-20s %s %s\n", suite_cases[i].name, suite_cases[i].id,
                   suite_cases[i].description);
        return 0;
    }
    if (argc != 4 || (strcmp(argv[2], "bug") && strcmp(argv[2], "fixed")))
        return 64;
    const struct suite_case *selected = NULL;
    for (size_t i = 0; i < suite_case_count; i++)
        if (!strcmp(argv[1], suite_cases[i].name))
            selected = &suite_cases[i];
    if (!selected)
        return 64;
    ssize_t length = readlink("/proc/self/exe", suite_executable, sizeof(suite_executable) - 1);
    suite_require(length > 0, "executable path");
    suite_executable[length] = 0;
    suite_require(!chdir(argv[3]), "evidence directory");
    char trace[128];
    snprintf(trace, sizeof(trace), "%s-%s.trace", argv[1], argv[2]);
    suite_trace = fopen(trace, "w");
    suite_require(suite_trace != NULL, "trace");
    fprintf(suite_trace, "event,value\n");
    suite_event("started", lab_clock(CLOCK_MONOTONIC));
    struct sample r = {0};
    uint64_t wall = lab_clock(CLOCK_MONOTONIC), cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID);
    selected->run(!strcmp(argv[2], "fixed"), &r);
    r.cpu_ns = lab_clock(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    r.wall_ns = lab_clock(CLOCK_MONOTONIC) - wall;
    suite_event("completed", r.completed);
    suite_event("checksum", r.checksum);
    suite_event("observed", r.observed);
    suite_event("auxiliary", r.auxiliary);
    suite_event("before", r.before);
    suite_event("after", r.after);
    suite_require(!fclose(suite_trace), "close trace");
    printf("{\"completed\":%" PRIu64 ",\"checksum\":%" PRIu64 ",\"observed\":%" PRIu64
           ",\"auxiliary\":%" PRIu64 ",\"before\":%" PRIu64 ",\"after\":%" PRIu64
           ",\"cpu_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64 "}\n",
           r.completed, r.checksum, r.observed, r.auxiliary, r.before, r.after, r.cpu_ns,
           r.wall_ns);
    return 0;
}
