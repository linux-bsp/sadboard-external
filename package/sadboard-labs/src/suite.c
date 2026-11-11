#include "suite-advanced.h"
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
#include "advanced-cases.def"
#undef CASE
const struct suite_case suite_cases[] = {
#define CASE(symbol, command, id, description, group, variant) {command, id, description, symbol},
#include "basic-cases.def"
#include "advanced-cases.def"
#undef CASE
};
const size_t suite_case_count = sizeof(suite_cases) / sizeof(suite_cases[0]);

/* These entries are launched by shell helpers or the host UART controller.
 * Include them in the public catalog without changing --has/run dispatch.
 */
static const struct suite_case external_cases[] = {
    {"crash", "B-CR01", "Application exits on an unknown record; GDB/core exercise", NULL},
    {"fd", "B-FD01", "Repeated requests leak descriptors under a process-only limit of 64", NULL},
    {"oom", "B-MM01", "Memory grows until a dedicated 32 MiB cgroup kills the child", NULL},
    {"cpu", "B-CPU01", "Repeated table lookup consumes CPU; perf exercise", NULL},
    {"lock", "B-SYNC01", "Two workers wait while a mutex covers simulated I/O", NULL},
    {"network", "B-NET01", "Isolated veth delay; ping, iperf3, tcpdump and tc exercise", NULL},
    {"panic", "B-BOOT01", "Deliberate kernel panic and reboot; check persistent evidence (run only)", NULL},
    {"boot-initlib", "B-BOOT02", "RAM PID1 loader fault; host sadboard-external/scripts/sadboard-board.py", NULL},
    {"boot-root", "B-BOOT03", "RAM kernel VFS root fault; host sadboard-external/scripts/sadboard-board.py", NULL},
    {"kernel-oops", "B-BOOT04", "Manual kernel BUG and panic-on-oops recovery (run only)", NULL},
    {"kernel-lockup", "B-BOOT05", "Real soft/hard lockup and proven hardware watchdog recovery (run only)", NULL},
    {"deadline", "A-P01", "Periodic telemetry drifts despite low CPU; release-time CSV and absolute timers", NULL},
    {"backlog", "A-M01", "Slow downstream consumer causes memory growth; bounded queue and backpressure", NULL},
    {"tcp", "A-N01", "Many small telemetry writes consume CPU; framed TCP batching in an owned namespace", NULL},
};

static unsigned catalog_group(const char *id)
{
    static const char *const groups[] = {
        "B-CR", "B-FD", "B-MM", "B-CPU", "B-SYNC", "B-NET", "B-BOOT", "A-P", "A-M", "A-N"
    };
    unsigned count = sizeof(groups) / sizeof(groups[0]);
    for (unsigned i = 0; i < count; i++)
        if (!strncmp(id, groups[i], strlen(groups[i])))
            return i;
    return count;
}

static int catalog_compare(const void *left, const void *right)
{
    const struct suite_case *a = *(const struct suite_case *const *)left;
    const struct suite_case *b = *(const struct suite_case *const *)right;
    unsigned ga = catalog_group(a->id), gb = catalog_group(b->id);
    if (ga != gb)
        return ga < gb ? -1 : 1;
    return strcmp(a->id, b->id);
}

static int catalog_list(const char *prefix)
{
    if (prefix && !*prefix) {
        fprintf(stderr, "Case ID prefix must not be empty\n");
        return 64;
    }
    const struct suite_case *entries[sizeof(suite_cases) / sizeof(suite_cases[0]) +
                                     sizeof(external_cases) / sizeof(external_cases[0])];
    size_t count = 0;
    for (size_t i = 0; i < suite_case_count; i++)
        entries[count++] = &suite_cases[i];
    for (size_t i = 0; i < sizeof(external_cases) / sizeof(external_cases[0]); i++)
        entries[count++] = &external_cases[i];
    qsort(entries, count, sizeof(entries[0]), catalog_compare);
    size_t matched = 0, prefix_length = prefix ? strlen(prefix) : 0;
    for (size_t i = 0; i < count; i++) {
        if (prefix && strncmp(entries[i]->id, prefix, prefix_length))
            continue;
        printf("%-9s %-20s %s\n", entries[i]->id, entries[i]->name, entries[i]->description);
        matched++;
    }
    if (prefix && !matched) {
        fprintf(stderr, "No scenarios match case ID prefix: %s\n", prefix);
        return 64;
    }
    if (!prefix) {
        puts("\nAuxiliary checks (not handbook cases):");
        printf("%-9s %-20s %s\n", "", "watchdog-probe",
               "Verify hardware watchdog recovery before kernel-lockup (run only)");
    }
    return 0;
}

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
    if ((argc == 2 || argc == 3) && !strcmp(argv[1], "--catalog"))
        return catalog_list(argc == 3 ? argv[2] : NULL);
    if(argc>=2 && !strcmp(argv[1],"--peer")) return suite_peer(argc,argv);
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
