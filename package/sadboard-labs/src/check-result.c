/* Read-only evidence checks for the documented reference workload contracts. */
#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static const char *directory;

static void reject(const char *reason)
{
    fprintf(stderr, "[sadboard] [ERROR] Evidence rejected: %s\n", reason);
    exit(1);
}

static FILE *open_file(const char *name, off_t maximum)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s/%s", directory, name) >= (int)sizeof(path))
        reject("path too long");
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size > maximum)
        reject(name);
    FILE *stream = fdopen(fd, "rb");
    if (!stream) reject(name);
    return stream;
}

static void text_file(const char *name, char *text, size_t capacity)
{
    FILE *stream = open_file(name, (off_t)capacity - 1);
    size_t n = fread(text, 1, capacity - 1, stream);
    if (ferror(stream) || memchr(text, 0, n)) reject(name);
    text[n] = 0;
    fclose(stream);
}

static void skip_space(char **p)
{
    while (isspace((unsigned char)**p)) ++*p;
}

static uint64_t number(char **p)
{
    if (!isdigit((unsigned char)**p) || (**p == '0' && isdigit((unsigned char)(*p)[1])))
        reject("expected unsigned integer");
    errno = 0;
    char *end;
    unsigned long long value = strtoull(*p, &end, 10);
    if (errno || end == *p) reject("integer overflow");
    *p = end;
    return value;
}

/* Order independent, exact fields, no duplicates/trailing data or truncated JSON. */
static void numeric_json(const char *file, const char *const *names, size_t count, uint64_t *values)
{
    char text[2048], key[64], *p = text;
    unsigned found = 0;
    text_file(file, text, sizeof(text));
    skip_space(&p);
    if (*p++ != '{') reject(file);
    for (size_t i = 0; i < count; i++) {
        skip_space(&p);
        if (*p++ != '"') reject(file);
        size_t n = 0;
        while (*p && *p != '"' && n + 1 < sizeof(key)) key[n++] = *p++;
        key[n] = 0;
        if (*p++ != '"') reject(file);
        size_t index;
        for (index = 0; index < count && strcmp(key, names[index]); index++) {}
        if (index == count || (found & (1U << index))) reject("unknown or duplicate JSON field");
        found |= 1U << index;
        skip_space(&p);
        if (*p++ != ':') reject(file);
        skip_space(&p);
        values[index] = number(&p);
        skip_space(&p);
        if (*p++ != (i + 1 == count ? '}' : ',')) reject(file);
    }
    skip_space(&p);
    if (*p || found != (1U << count) - 1) reject(file);
}

static uint64_t scalar(const char *file)
{
    char text[128], *p = text;
    text_file(file, text, sizeof(text));
    uint64_t value = number(&p);
    skip_space(&p);
    if (*p) reject(file);
    return value;
}

static void exact(const char *file, const char *expected)
{
    char text[2048];
    text_file(file, text, sizeof(text));
    if (strcmp(text, expected)) reject(file);
}

static uint64_t field(const char *file, const char *key)
{
    char text[2048];
    text_file(file, text, sizeof(text));
    char *line = text;
    int found = 0;
    uint64_t value = 0;
    while (line && *line) {
        char *next = strchr(line, '\n');
        if (next) *next++ = 0;
        if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == ' ') {
            if (found++) reject("duplicate event field");
            char *p = line + strlen(key) + 1;
            value = number(&p);
            skip_space(&p);
            if (*p) reject(file);
        }
        line = next;
    }
    if (!found) reject(file);
    return value;
}

static void cpu(const char *mode, int count, char **args)
{
    if (count != 3) reject("CPU parameters missing");
    unsigned items = lab_number(args[0], 16384, 0), queries = lab_number(args[1], 2000000, 0);
    uint32_t seed = lab_number(args[2], UINT32_MAX, 1), state = seed;
    if ((uint64_t)items * queries > UINT64_C(4000000000)) reject("CPU work budget");
    uint64_t checksum = 0, times[2] = {0};
    for (unsigned i = 0; i < queries; i++) {
        state = state * UINT32_C(1664525) + UINT32_C(1013904223);
        checksum += (state % items) * 3 + 7;
    }
    const char *const keys[] = {"items", "queries", "seed", "checksum", "cpu_ns", "wall_ns"};
    for (unsigned i = 0; i < (!strcmp(mode, "verify") ? 2U : 1U); i++) {
        uint64_t v[6];
        numeric_json(i ? "cpu-fixed.out" : "cpu-bug.out", keys, 6, v);
        if (v[0] != items || v[1] != queries || v[2] != seed || v[3] != checksum || !v[5])
            reject("CPU inputs/checksum/timing do not match the recorded workload");
        times[i] = v[4];
    }
    printf("[sadboard] [INFO] CPU contract matched: queries=%u checksum=%" PRIu64 "\n", queries, checksum);
    if (!strcmp(mode, "verify") && times[1])
        printf("[sadboard] [INFO] CPU time ratio=%.3f samples=1 performance_verdict=manual\n", (double)times[0] / times[1]);
}

static void lock_check(const char *mode, int count, char **args)
{
    if (count != 2) reject("lock parameters missing");
    unsigned n = lab_number(args[0], 200, 0), delay = lab_number(args[1], 100000, 0);
    if ((uint64_t)n * delay * 2 > 10000000) reject("lock duration budget");
    const char *const keys[] = {"iterations", "completed", "cpu_ns", "wall_ns"};
    uint64_t times[2] = {0};
    for (unsigned i = 0; i < (!strcmp(mode, "verify") ? 2U : 1U); i++) {
        uint64_t v[4], minimum = (uint64_t)n * delay * (i ? 1 : 2) * 1000;
        numeric_json(i ? "lock-fixed.out" : "lock-bug.out", keys, 4, v);
        if (v[0] != n || v[1] != n * 2 || v[3] < minimum)
            reject("lock workload incomplete or sleep budget skipped");
        times[i] = v[3];
    }
    printf("[sadboard] [INFO] Lock contract matched: completed=%u\n", n * 2);
    if (!strcmp(mode, "verify") && times[1])
        printf("[sadboard] [INFO] Wall time ratio=%.3f samples=1 performance_verdict=manual\n", (double)times[0] / times[1]);
}

static uint64_t bytes(const unsigned char *p, unsigned n, int little)
{
    uint64_t result = 0;
    for (unsigned i = 0; i < n; i++) result = result * 256 + p[little ? n - i - 1 : i];
    return result;
}

static void core_check(void)
{
    DIR *dir = opendir(directory);
    if (!dir) reject("core directory");
    struct dirent *entry;
    int valid = 0;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "core.crash-bug.", 15)) continue;
        FILE *f = open_file(entry->d_name, 33554432);
        unsigned char h[64], ph[56];
        struct stat st;
        if (fstat(fileno(f), &st) || fread(h, 1, sizeof(h), f) != sizeof(h) ||
            memcmp(h, ELFMAG, SELFMAG) || h[EI_CLASS] != ELFCLASS64 ||
            (h[EI_DATA] != ELFDATA2LSB && h[EI_DATA] != ELFDATA2MSB)) reject("invalid/truncated ELF core");
        int le = h[EI_DATA] == ELFDATA2LSB;
        if (bytes(h + 16, 2, le) != ET_CORE || bytes(h + 20, 4, le) != EV_CURRENT)
            reject("file is not an ELF core");
#if defined(__aarch64__)
        if (bytes(h + 18, 2, le) != EM_AARCH64) reject("wrong core architecture");
#elif defined(__x86_64__)
        if (bytes(h + 18, 2, le) != EM_X86_64) reject("wrong core architecture");
#endif
        uint64_t offset = bytes(h + 32, 8, le), entries = bytes(h + 56, 2, le);
        if (!entries || bytes(h + 54, 2, le) != sizeof(ph) || offset > (uint64_t)st.st_size ||
            entries * sizeof(ph) > (uint64_t)st.st_size - offset) reject("invalid core segment table");
        for (uint64_t i = 0; i < entries; i++) {
            if (fseeko(f, (off_t)(offset + i * sizeof(ph)), SEEK_SET) || fread(ph, 1, sizeof(ph), f) != sizeof(ph)) reject("core segment");
            uint64_t start = bytes(ph + 8, 8, le), size = bytes(ph + 32, 8, le);
            if (start > (uint64_t)st.st_size || size > (uint64_t)st.st_size - start) reject("truncated core segment");
            if (bytes(ph, 4, le) == PT_NOTE) {
                if (size > 1048576) reject("oversized core notes");
                uint64_t used = 0;
                while (used < size) {
                    unsigned char note[12], name[5];
                    if (size - used < sizeof(note) || fseeko(f, (off_t)(start + used), SEEK_SET) ||
                        fread(note, 1, sizeof(note), f) != sizeof(note)) reject("truncated core note");
                    uint64_t names = bytes(note, 4, le), desc = bytes(note + 4, 4, le);
                    uint64_t total = 12 + ((names + 3) & ~UINT64_C(3)) + ((desc + 3) & ~UINT64_C(3));
                    if (total > size - used) reject("invalid core note lengths");
                    if (names == 5 && desc >= 16 && bytes(note + 8, 4, le) == NT_PRSTATUS &&
                        fread(name, 1, 5, f) == 5 && !memcmp(name, "CORE", 5)) valid = 1;
                    used += total;
                }
            }
        }
        fclose(f);
    }
    closedir(dir);
    if (!valid) reject("no complete ELF core with thread status notes");
}

static void oom(const char *mode)
{
    if (scalar("memory-bug.exit") != 137 || !field("memory-bug.events", "oom") ||
        !field("memory-bug.events", "oom_kill") || !scalar("memory-bug.peak"))
        reject("SIGKILL lacks cgroup OOM evidence");
    exact("memory-bug.limits", "memory.max=33554432\nmemory.swap.max=0\nmemory.oom.group=1\n");
    if (!strcmp(mode, "verify")) {
        if (scalar("memory-fixed.exit") || field("memory-fixed.events", "oom_kill") || !scalar("memory-fixed.peak"))
            reject("fixed workload failed or was OOM killed");
        exact("memory-fixed.limits", "memory.max=33554432\nmemory.swap.max=0\nmemory.oom.group=1\n");
        const char *const keys[] = {"chunks", "checksum"};
        uint64_t v[2];
        numeric_json("memory-fixed.out", keys, 2, v);
        if (v[0] != 64 || v[1] != 532480) reject("fixed memory workload incomplete");
    }
}

static double ping_result(const char *file, unsigned wanted_tx, unsigned wanted_rx)
{
    char text[8192], *line;
    unsigned tx = 0, rx = 0;
    double low = 0, avg = 0, high = 0;
    int counts = 0, timing = 0;
    text_file(file, text, sizeof(text));
    for (line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
        if (sscanf(line, "%u packets transmitted, %u", &tx, &rx) == 2) counts++;
        if (sscanf(line, "round-trip min/avg/max = %lf/%lf/%lf", &low, &avg, &high) == 3 ||
            sscanf(line, "rtt min/avg/max/mdev = %lf/%lf/%lf", &low, &avg, &high) == 3) timing++;
    }
    if (counts != 1 || tx != wanted_tx || rx != wanted_rx ||
        (wanted_rx && (timing != 1 || !isfinite(low) || !isfinite(avg) || !isfinite(high) || low < 0 || low > avg || avg > high)))
        reject(file);
    return avg;
}

static void pcap_check(void)
{
    FILE *f = open_file("network.pcap", 33554432);
    unsigned char h[24], packet[16], frame[34];
    if (fread(h, 1, sizeof(h), f) != sizeof(h)) reject("empty pcap");
    int le = !memcmp(h, "\xd4\xc3\xb2\xa1", 4) || !memcmp(h, "\x4d\x3c\xb2\xa1", 4);
    if (!le && memcmp(h, "\xa1\xb2\xc3\xd4", 4) && memcmp(h, "\xa1\xb2\x3c\x4d", 4)) reject("pcap format");
    if (bytes(h + 4, 2, le) != 2 || bytes(h + 6, 2, le) != 4 || bytes(h + 20, 4, le) != 1)
        reject("pcap version/link type");
    unsigned protocols = 0, packets = 0;
    size_t n;
    struct stat st;
    if (fstat(fileno(f), &st)) reject("pcap stat");
    while ((n = fread(packet, 1, sizeof(packet), f))) {
        if (n != sizeof(packet)) reject("truncated pcap packet header");
        uint64_t size = bytes(packet + 8, 4, le), original = bytes(packet + 12, 4, le);
        off_t position = ftello(f);
        if (position < 0 || size > original || size > 262144 || size > (uint64_t)(st.st_size - position)) reject("truncated pcap packet");
        size_t prefix = size < sizeof(frame) ? (size_t)size : sizeof(frame);
        if (fread(frame, 1, prefix, f) != prefix) reject("pcap data");
        if (prefix == sizeof(frame) && frame[12] == 8 && frame[13] == 0 && (frame[14] >> 4) == 4) {
            const unsigned char left[4] = {198, 18, 0, 1}, right[4] = {198, 18, 0, 2};
            int pair = (!memcmp(frame + 26, left, 4) && !memcmp(frame + 30, right, 4)) ||
                       (!memcmp(frame + 26, right, 4) && !memcmp(frame + 30, left, 4));
            if (pair && frame[23] == 1) protocols |= 1;
            if (pair && frame[23] == 6) protocols |= 2;
        }
        if (fseeko(f, (off_t)(size - prefix), SEEK_CUR)) reject("pcap seek");
        packets++;
    }
    if (ferror(f) || !packets || protocols != 3) reject("pcap lacks experiment ICMP/TCP traffic");
    fclose(f);
}

static void network(const char *mode)
{
    char text[4096];
    pcap_check();
    double delayed = ping_result("delayed.ping", 5, 5);
    text_file("delayed.qdisc", text, sizeof(text));
    if (!strstr(text, "netem") || !strstr(text, "delay 100ms") || delayed < 50)
        reject("delay injection not evidenced");
    if (!strcmp(mode, "verify")) {
        double baseline = ping_result("baseline.ping", 5, 5), restored = ping_result("restored.ping", 5, 5);
        (void)ping_result("lost.ping", 3, 0);
        text_file("lost.qdisc", text, sizeof(text));
        if (!strstr(text, "netem") || !strstr(text, "loss 100%") ||
            delayed < baseline + 50 || restored >= delayed / 2)
            reject("network baseline/loss/restoration inconsistent");
        printf("[sadboard] [INFO] RTT baseline=%.3f delayed=%.3f restored=%.3f ms\n", baseline, delayed, restored);
    }
}

int main(int argc, char **argv)
{
    if (argc < 5) return 64;
    const char *scenario = argv[1], *mode = argv[2];
    directory = argv[3];
    unsigned code = lab_number(argv[4], 255, 1);
    if (strcmp(mode, "run") && strcmp(mode, "verify")) return 64;
    int verify = !strcmp(mode, "verify");
    if (!strcmp(scenario, "cpu") || !strcmp(scenario, "lock")) {
        if (code) reject("workload did not exit successfully");
        if (!strcmp(scenario, "cpu")) cpu(mode, argc - 5, argv + 5);
        else lock_check(mode, argc - 5, argv + 5);
    } else if (!strcmp(scenario, "oom")) {
        if (argc != 5 || code != (verify ? 0U : 137U)) reject("memory exit/parameters");
        oom(mode);
    } else if (!strcmp(scenario, "crash")) {
        if (verify) {
            if (argc != 5 || code) reject("crash reference exit/parameters");
            exact("crash-valid.out", "record=1 value=64\n");
            if (scalar("crash-missing.exit") != 2) reject("missing record not rejected");
            exact("crash-missing.err", "record 42 not found\n");
        } else {
            if (argc != 6) reject("crash input missing");
            unsigned key = lab_number(argv[5], INT_MAX, 1);
            if (code != (key == 1 ? 0U : 139U)) reject("unexpected crash signal/exit");
            if (key == 1) exact("crash-bug.out", "record=1 value=64\n");
            else core_check();
        }
    } else if (!strcmp(scenario, "network")) {
        if (argc != 5 || code) reject("network exit/parameters");
        network(mode);
    } else return 64;
    printf("[sadboard] [INFO] Evidence contract matched: case=%s operation=%s\n", scenario, mode);
    return 0;
}
