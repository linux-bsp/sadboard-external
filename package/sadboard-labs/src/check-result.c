/* Read-only evidence checks for the documented reference workload contracts. */
#define _GNU_SOURCE
#include "lab.h"
#include <ctype.h>
#include <dirent.h>
#include <elf.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <sched.h>
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

static void descriptor_check(const char *mode, unsigned code, int count, char **args)
{
    if (count != 2) reject("descriptor parameters missing");
    unsigned requests = lab_number(args[0], 4096, 0), delay = lab_number(args[1], 50, 1);
    if ((uint64_t)requests * delay > 10000) reject("descriptor duration budget");
    const char *const keys[] = {"limit", "requested", "delay_ms", "baseline", "completed",
                                "peak_open", "final_open", "error"};
    int verify = !strcmp(mode, "verify");
    for (unsigned i = 0; i < (verify ? 2U : 1U); i++) {
        uint64_t v[8];
        numeric_json(i ? "fd-fixed.out" : "fd-bug.out", keys, 8, v);
        if (v[0] != 64 || v[1] != requests || v[2] != delay || v[3] >= 64)
            reject("descriptor parameters/limit inconsistent");
        uint64_t completed = i || requests <= 64 - v[3] ? requests : 64 - v[3];
        unsigned error = !i && completed < requests ? EMFILE : 0;
        if (v[4] != completed || v[5] != v[3] + (i ? 1 : completed) ||
            v[6] != v[3] + (i ? 0 : completed) || v[7] != error ||
            scalar(i ? "fd-fixed.exit" : "fd-bug.exit") != error)
            reject("descriptor lifecycle/workload inconsistent");
        if (!i && code != (verify ? 0U : error)) reject("descriptor outer exit inconsistent");
    }
    printf("[sadboard] [INFO] Descriptor contract matched: requests=%u limit=64\n", requests);
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

static void csv_row(FILE *stream, uint64_t values[4])
{
    char line[256], *p = line;
    if (!fgets(line, sizeof(line), stream)) reject("CSV row missing");
    for (unsigned i = 0; i < 4; i++) {
        values[i] = number(&p);
        if (i < 3 && *p++ != ',') reject("CSV separator");
    }
    skip_space(&p);
    if (*p) reject("CSV trailing data");
}

static FILE *csv_open(const char *name, const char *header)
{
    FILE *stream = open_file(name, 1048576);
    char line[256];
    if (!fgets(line, sizeof(line), stream) || strcmp(line, header)) reject("CSV header");
    return stream;
}

static void csv_end(FILE *stream)
{
    if (fgetc(stream) != EOF || ferror(stream)) reject("CSV has extra rows or an I/O error");
    fclose(stream);
}

static int compare_u64(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
    return (x > y) - (x < y);
}

static void deadline_check(const char *mode, int count, char **args)
{
    if (count != 3) reject("deadline parameters");
    unsigned n = lab_number(args[0], 500, 0), period = lab_number(args[1], 50000, 0);
    unsigned work = lab_number(args[2], 10000, 0);
    if (n < 2 || work * 2 > period || (uint64_t)n * (period + work) > 5000000) reject("deadline budget");
    const char *const keys[] = {"cycles", "period_us", "work_us", "completed", "checksum", "cpu_ns",
        "wall_ns", "p50_ns", "p99_ns", "max_ns", "drift_ns"};
    uint64_t p99[2] = {0};
    for (unsigned variant = 0; variant < (!strcmp(mode, "verify") ? 2U : 1U); variant++) {
        uint64_t v[11], late[500], row[4], first = 0, last_finish = 0, drift = 0;
        numeric_json(variant ? "deadline-fixed.out" : "deadline-bug.out", keys, 11, v);
        uint64_t checksum = (uint64_t)3 * n * (n - 1) / 2 + (uint64_t)7 * n;
        if (v[0] != n || v[1] != period || v[2] != work || v[3] != n || v[4] != checksum)
            reject("deadline work not preserved");
        FILE *csv = csv_open(variant ? "deadline-fixed.csv" : "deadline-bug.csv",
                             "index,release_ns,wake_ns,finish_ns\n");
        for (unsigned i = 0; i < n; i++) {
            csv_row(csv, row);
            if (!i) first = row[1];
            if (row[0] != i || row[1] != first + (uint64_t)i * period * 1000 ||
                row[2] < row[1] || row[3] < row[2] || row[3] - row[2] < (uint64_t)work * 1000 ||
                (i && row[2] < last_finish)) reject("deadline timestamps do not describe sequential complete jobs");
            late[i] = row[2] - row[1]; last_finish = row[3];
        }
        csv_end(csv); drift = late[n - 1];
        qsort(late, n, sizeof(late[0]), compare_u64);
        if (v[7] != late[(n + 1) / 2 - 1] || v[8] != late[(99 * n + 99) / 100 - 1] ||
            v[9] != late[n - 1] || v[10] != drift ||
            v[6] < last_finish - first + (uint64_t)period * 1000)
            reject("deadline summary does not match raw samples");
        if (!variant && drift < (uint64_t)(n - 1) * work * 1000) reject("relative timer drift not reproduced");
        p99[variant] = v[8];
    }
    printf("[sadboard] [INFO] Deadline work and raw timing matched; p99_ns=%" PRIu64 "/%" PRIu64
           " samples=1 performance_verdict=manual\n", p99[0], p99[1]);
}

static void backlog_check(const char *mode, int count, char **args)
{
    if (count != 2) reject("backlog parameters");
    unsigned n = lab_number(args[0], 1024, 0), kib = lab_number(args[1], 64, 0);
    if (n < 16) reject("backlog minimum workload");
    uint64_t size = (uint64_t)kib * 1024, checksum = 0;
    for (unsigned i = 0; i < n; i++) checksum += (i & 255) * size;
    const char *const keys[] = {"messages", "payload_kib", "completed", "checksum", "allocations", "frees",
        "peak_queue", "peak_live_bytes", "final_live_bytes", "producer_waits", "maxrss_bytes", "wall_ns"};
    uint64_t peaks[2] = {0};
    for (unsigned variant = 0; variant < (!strcmp(mode, "verify") ? 2U : 1U); variant++) {
        uint64_t v[12], row[4], peak_queue = 0, peak_live = 0;
        numeric_json(variant ? "backlog-fixed.out" : "backlog-bug.out", keys, 12, v);
        if (v[0] != n || v[1] != kib || v[2] != n || v[3] != checksum ||
            v[4] != n || v[5] != n || v[8] || v[11] < ((uint64_t)n * 3000 + 100000) * 1000)
            reject("backlog dropped work, leaked payload or skipped service cost");
        FILE *csv = csv_open(variant ? "backlog-fixed.csv" : "backlog-bug.csv",
                             "index,queued,live_payload_bytes,consumed\n");
        for (unsigned i = 0; i < n; i++) {
            csv_row(csv, row);
            if (row[0] != i || !row[1] || row[1] > n || row[3] > i ||
                row[2] != (i + 1 - row[3]) * size || row[2] < row[1] * size ||
                row[2] > (row[1] + 1) * size) reject("backlog raw queue accounting");
            if (row[1] > peak_queue) peak_queue = row[1];
            if (row[2] > peak_live) peak_live = row[2];
        }
        csv_end(csv);
        if (v[6] != peak_queue || v[7] != peak_live) reject("backlog peak not supported by raw samples");
        if (variant ? (v[6] > 8 || v[7] > 9 * size || !v[9]) : v[6] <= 8)
            reject("backlog or bounded backpressure not demonstrated");
        peaks[variant] = v[7];
    }
    printf("[sadboard] [INFO] Backlog preserved %u messages; peak_live_bytes=%" PRIu64 "/%" PRIu64
           "; RSS and allocator retention require separate analysis\n", n, peaks[0], peaks[1]);
}

static void tcp_check(const char *mode, int count, char **args)
{
    if (count != 1) reject("TCP parameters");
    unsigned n = lab_number(args[0], 32768, 0);
    if (n < 64) reject("TCP minimum workload");
    uint64_t checksum = 0, calls[2] = {0};
    for (unsigned i = 0; i < n; i++) {
        for (unsigned shift = 0; shift < 32; shift += 8) checksum += (i >> shift) & 255;
        for (unsigned byte = 4; byte < 64; byte++) checksum += (i * 17 + byte * 3 + 42) & 255;
    }
    const char *const keys[] = {"records", "frame_bytes", "completed", "bytes", "checksum",
        "send_calls", "recv_calls", "cpu_ns", "wall_ns"};
    for (unsigned variant = 0; variant < (!strcmp(mode, "verify") ? 2U : 1U); variant++) {
        uint64_t v[9];
        numeric_json(variant ? "tcp-fixed.out" : "tcp-bug.out", keys, 9, v);
        if (v[0] != n || v[1] != 64 || v[2] != n || v[3] != (uint64_t)n * 64 ||
            v[4] != checksum || !v[5] || !v[6] || !v[8]) reject("TCP stream incomplete or corrupt");
        if (!variant && v[5] < n) reject("per-record send path not reproduced");
        calls[variant] = v[5];
    }
    if (!strcmp(mode, "verify") && calls[1] >= calls[0]) reject("batching did not reduce send calls");
    printf("[sadboard] [INFO] TCP stream preserved; send_calls=%" PRIu64 "/%" PRIu64
           "; send calls are not wire packets\n", calls[0], calls[1]);
}

enum suite_index {
#define CASE(symbol, command, id, description, group, variant) S_##symbol,
#include "advanced-cases.def"
#include "basic-cases.def"
#include "learning-cases.def"
#undef CASE
    S_COUNT
};
static int suite_index(const char *name)
{
    static const char *const names[] = {
#define CASE(symbol, command, id, description, group, variant) command,
#include "advanced-cases.def"
#include "basic-cases.def"
#include "learning-cases.def"
#undef CASE
    };
    for (unsigned i = 0; i < S_COUNT; i++)
        if (!strcmp(names[i], name))
            return (int)i;
    return -1;
}
static void suite_check(const char *name, const char *mode, int index)
{
    const char *const keys[] = {"completed", "checksum", "observed", "auxiliary",
                                "before",    "after",    "cpu_ns",   "wall_ns"};
    for (unsigned fixed = 0; fixed < (!strcmp(mode, "verify") ? 2U : 1U); fixed++) {
        uint64_t v[8];
        char file[128];
        snprintf(file, sizeof(file), "%s-%s.out", name, fixed ? "fixed" : "bug");
        numeric_json(file, keys, 8, v);
        if (!v[6] || !v[7])
            reject("suite missing timing");
        snprintf(file, sizeof(file), "%s-%s.trace", name, fixed ? "fixed" : "bug");
        FILE *trace = open_file(file, 1048576);
        char line[256];
        unsigned seen = 0;
        if (!fgets(line, sizeof(line), trace) || strcmp(line, "event,value\n"))
            reject("suite trace header");
        while (fgets(line, sizeof(line), trace)) {
            char *comma = strchr(line, ',');
            if (!comma)
                reject("suite trace row");
            *comma = 0;
            char *value = comma + 1;
            uint64_t scalar = number(&value);
            skip_space(&value);
            if (*value)
                reject("suite trace number");
            for (unsigned i = 0; i < 6; i++)
                if (!strcmp(line, keys[i])) {
                    if ((seen & (1U << i)) || scalar != v[i])
                        reject("suite summary does not match trace");
                    seen |= 1U << i;
                }
        }
        if (ferror(trace) || seen != 63)
            reject("suite truncated trace");
        fclose(trace);
        uint64_t completed = v[0], sum = v[1], observed = v[2], aux = v[3], before = v[4],
                 after = v[5];
        int valid = 0;
        switch (index) {
        case S_asm_width:
            valid = completed == 1 && sum == UINT32_C(0x89abcdef) && before == UINT32_C(0x12345678) &&
                    observed == (fixed ? UINT64_C(0x89abcdef) : UINT64_C(0x1234567889abcdef)) &&
                    aux == (fixed ? 4U : 8U);
            break;
        case S_asm_sign:
            valid = completed == 1 && sum == (uint64_t)INT64_C(-128) && before == 128 && aux == 8 &&
                    observed == (fixed ? (uint64_t)INT64_C(-128) : 128U);
            break;
        case S_asm_pointer:
            valid = completed == 1 && sum == 7 && before > UINT32_MAX && aux == UINT32_MAX &&
                    observed == (fixed ? before : (uint32_t)before) && after == observed;
            break;
        case S_asm_branch:
            valid = completed == 2 && sum == 1 && before == 16 && after == UINT32_C(0x80000000) &&
                    observed == (fixed ? 1U : 2U) && aux == (fixed ? 0U : 1U);
            break;
        case S_asm_call:
            valid = completed == 1 && sum == 15 && before == 7 && aux == 85 &&
                    observed == (fixed ? 15U : 93U);
            break;
        case S_asm_layout:
            valid = completed == 1 && sum == UINT32_C(0x12345678) && before == 16 && after == 4 &&
                    observed == (fixed ? UINT32_C(0x12345678) : UINT32_C(0x78000000)) &&
                    aux == (fixed ? 4U : 1U);
            break;
        case S_asm_bounds:
            valid = completed == 4 && sum == 10 && before == 4 && after == 20 &&
                    observed == (fixed ? 10U : 65U) && aux == (fixed ? 4U : 5U);
            break;
        case S_asm_preserve:
            valid = completed == 1 && sum == 0x1234 && before == 0x1234 && aux == 19 &&
                    observed == (fixed ? 0x1234U : 85U) && after == observed;
            break;
        case S_k_cow:
            valid = completed == 1 && sum == 99 && before == 42 && after == 42 && aux == 99 &&
                    observed == (fixed ? 99U : 42U);
            break;
        case S_k_rename:
            valid = completed == 1 && sum == 9 && before == 7 && after == 9 && aux == 1 &&
                    observed == (fixed ? 9U : 7U);
            break;
        case S_k_eventfd:
            valid = completed == (fixed ? 3U : 1U) && sum == 3 && observed == 3 && aux == 1 &&
                    before == 3 && after == 0;
            break;
        case S_k_timerfd:
            valid = completed == 1 && aux > 1 && sum == aux && before == 10000000 &&
                    observed == (fixed ? aux : 1U) && after == aux - observed;
            break;
        case S_k_zombie:
            valid = completed == 1 && sum == 7 && before == 1 && after == 0 && aux == 7 &&
                    observed == (fixed ? 0U : 1U);
            break;
        case S_k_datagram:
            valid = completed == (fixed ? 128U : 16U) && sum == (fixed ? 8128U : 120U) &&
                    observed == (fixed ? 0U : 1U) && aux == 128;
            break;
        case S_k_pipe:
            valid = completed == (fixed ? 12U : 3U) && sum == (fixed ? 66U : 3U) &&
                    observed == 3 && aux == 12;
            break;
        case S_k_affinity:
            valid = completed == 1 && sum == 1 && before > 0 && before == after && aux < CPU_SETSIZE &&
                    observed == (fixed ? 0U : 22U);
            break;
        case S_cr_abort:
            valid = completed == 1 && observed == (fixed ? 0U : 6U) && aux == (fixed ? 2U : 0U) &&
                    sum == aux;
            break;
        case S_cr_heap:
            valid = completed == 1 && !observed && aux == (fixed ? 0U : 99U) && sum == aux;
            break;
        case S_cr_stack:
            valid = completed == 1 && observed == (fixed ? 0U : 11U) && aux == (fixed ? 2U : 0U) &&
                    sum == aux;
            break;
        case S_cr_bus:
            valid = completed == 1 && observed == (fixed ? 0U : 7U) && !aux && !sum;
            break;
        case S_fd_dup:
        case S_fd_epoll:
            valid =
                completed == 16 && sum == 120 && observed == (fixed ? 0U : 16U) && before == after;
            break;
        case S_fd_close:
            valid = completed == 16 && sum == 120 && observed == (fixed ? 0U : 16U) && aux == 16 &&
                    before == after;
            break;
        case S_fd_exec:
            valid = completed == 1 && sum == 1 && observed == (fixed ? 0U : 1U) && before == after;
            break;
        case S_mm_limit:
            valid = completed == 1 && sum == 12 && observed == 12 && aux == (fixed ? 0U : 11U);
            break;
        case S_mm_rss:
            valid = completed == 4194304U / (unsigned)sysconf(_SC_PAGESIZE) &&
                    sum == completed * 7 && observed == (fixed ? 4194304U : 33554432U) &&
                    aux == 4194304;
            break;
        case S_mm_shmem:
            valid = completed == 8388608U / (unsigned)sysconf(_SC_PAGESIZE) &&
                    sum == completed * 6 && observed == (fixed ? 0U : 1U) && aux == observed &&
                    before == after;
            break;
        case S_mm_kill:
            valid = completed == 1 && sum == 42 && observed == (fixed ? 0U : 9U) && !aux;
            break;
        case S_cpu_eagain:
            valid = completed == 1 && sum == 42 && (fixed ? !observed : observed > 0) &&
                    aux == observed + 1;
            break;
        case S_cpu_log:
            valid = completed == 2048 && sum == 2096128 && observed == (fixed ? 64U : 2048U) &&
                    aux == 2048;
            break;
        case S_cpu_poll:
            valid = completed == 1 && sum == 42 && (fixed ? !observed : observed > 0);
            break;
        case S_cpu_cache:
            valid = completed == 4194304 && sum == 534773760 && observed == (fixed ? 4U : 4096U) &&
                    aux == 4194304;
            break;
        case S_sy_dead:
            valid = completed == 1 && sum == (fixed ? 84U : 0U) && observed == (fixed ? 0U : 1U) &&
                    aux == (fixed ? 0U : 9U);
            break;
        case S_sy_lost:
            valid = completed == 1 && sum == 42 && observed == (fixed ? 0U : 110U) && aux == 1;
            break;
        case S_sy_race:
            valid =
                completed == 64 && sum == (fixed ? 64U : 32U) && observed == sum && after == sum;
            break;
        case S_sy_join:
            valid = completed == 1 && sum == 1 && observed == (fixed ? 0U : 110U) && aux == 1 &&
                    before == after;
            break;
        case S_net_frame:
            valid = completed == (fixed ? 1U : 0U) && sum == (fixed ? 36U : 0U) &&
                    observed == (fixed ? 12U : 3U) && aux == 12;
            break;
        case S_net_endian:
            valid = completed == (fixed ? 1U : 0U) && sum == (fixed ? 1024U : 0U) &&
                    observed == (fixed ? 1024U : 262144U) && aux == 4096;
            break;
        case S_net_udp:
            valid = aux == 128 && observed == after - before &&
                    sum == completed * (completed - 1) / 2 &&
                    (fixed ? (completed == 128 && !observed)
                           : (completed > 0 && completed < 128 && observed == 128 - completed));
            break;
        case S_net_fin:
            valid = completed == 3 && sum == 3 && observed == (fixed ? 0U : 13U) &&
                    aux == (fixed ? 32U : 0U) && before == 104 && !after;
            break;
        case S_p_pi:
            valid = completed == 2 && sum == 84 && observed > 0 && aux == (fixed ? 0U : 1U);
            break;
        case S_p_loop:
            valid = completed == 36 && sum == 896 && observed > 0 &&
                    (fixed ? after < before : after >= before);
            break;
        case S_p_share:
            valid = completed == 1000000 && sum == 1000000 && observed == (fixed ? 64U : 8U) &&
                    aux == 2;
            break;
        case S_p_log:
            valid = completed == 1024 && sum == 523776 && observed == (fixed ? 32U : 1024U) &&
                    aux == observed;
            break;
        case S_p_io:
            valid = completed == 64 && sum == 8257536 && observed == (fixed ? 8U : 64U) &&
                    aux == 262144;
            break;
        case S_p_fault:
            valid = completed == 4096 && sum == 172032 && observed == (fixed ? 0U : 4096U) &&
                    (fixed ? aux < 32 : aux >= 4096) && before == 16777216 && after == 4096;
            break;
        case S_p_quota:
            valid = completed == 30000000 && sum == 213978538 && aux > 0 &&
                    (fixed ? (!observed && !after) : (observed > 0 && after > 0));
            break;
        case S_m_cma:
            valid = completed == 1 && sum == 4096 && aux >= 4096 &&
                    (fixed ? (observed == 0 && before >= 4096) : (observed == 12 && before < 4096));
            break;
        case S_m_arena:
            valid = completed == 8192 && sum == 1044480 && aux < before + 65536 &&
                    (fixed ? observed < 1048576 : observed > 4194304);
            break;
        case S_m_frame:
            valid = completed == 64 && sum == 528482304 && observed == (fixed ? 0U : 33554432U) &&
                    aux == (fixed ? 4U : 192U) && before == (fixed ? 1048576U : 3145728U) && !after;
            break;
        case S_m_high:
            valid = completed == 4096 && sum == 522240 && !aux && after < 33554432 &&
                    (fixed ? (!observed && before < 8388608) : (observed > 0 && before > 8388608));
            break;
        case S_m_session:
            valid = completed == 8 && sum == 28 && observed == (fixed ? 0U : 8U) &&
                    aux == (fixed ? 1U : 0U) && before == after;
            break;
        case S_m_socket:
            valid = completed == 1048576 && sum == 44040192 && aux == (fixed ? 1U : 16U) &&
                    before == after && (fixed ? !observed : observed > 0);
            break;
        case S_m_thread:
            valid = completed == 12 && sum == 66 && observed == (fixed ? 4U : 12U) && aux > 0 &&
                    after > 0;
            break;
        case S_n_batch:
            valid = completed == 128 && sum == 8128 && observed == (fixed ? 8U : 128U) && !aux;
            break;
        case S_n_slow:
            valid = completed == 64 && sum == 2016 &&
                    (fixed ? (observed > 0 && observed <= 8) : observed > 8) && !after;
            break;
        case S_n_edge:
            valid = completed == (fixed ? 65U : 2U) && sum == (fixed ? 106U : 43U) &&
                    observed == (fixed ? 0U : 63U) && aux == 1;
            break;
        case S_n_mtu:
            valid = completed == (fixed ? 1400U : 0U) && sum == (fixed ? 170340U : 0U) &&
                    observed == (fixed ? 90U : 0U) && aux == (fixed ? 600U : 1500U) &&
                    before == 64 && after == (fixed ? 3U : 0U);
            break;
        case S_n_rps:
            valid = completed == 262144 && sum == 11010048 && observed == (fixed ? 3U : 2U);
            break;
        case S_n_qos:
            valid = completed == 80 && sum == 2136 && observed > 0 && aux == 64 && before == 16;
            break;
        case S_n_retry:
            valid = before == after &&
                    (fixed ? (completed == 1 && sum == 42 && observed > 1 && observed <= 20 && !aux)
                           : (!completed && !sum && observed == 20 && aux == 111));
            break;
        default:
            break;
        }
        if (!valid)
            reject("suite signal/resource/work contract differs");
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
    } else if (!strcmp(scenario, "fd")) {
        descriptor_check(mode, code, argc - 5, argv + 5);
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
    } else if (!strcmp(scenario, "deadline") || !strcmp(scenario, "backlog") || !strcmp(scenario, "tcp")) {
        if (code) reject("advanced workload did not exit successfully");
        if (!strcmp(scenario, "deadline")) deadline_check(mode, argc - 5, argv + 5);
        else if (!strcmp(scenario, "backlog")) backlog_check(mode, argc - 5, argv + 5);
        else tcp_check(mode, argc - 5, argv + 5);
    } else if(suite_index(scenario)>=0) {
        if(argc!=5||code)reject("suite exit/parameters");
        suite_check(scenario,mode,suite_index(scenario));
    } else return 64;
    printf("[sadboard] [INFO] Evidence contract matched: case=%s operation=%s\n", scenario, mode);
    return 0;
}
