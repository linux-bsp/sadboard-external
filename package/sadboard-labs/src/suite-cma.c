#define _GNU_SOURCE
#include "suite.h"
#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

static char area[512];
static uint64_t number(const char *name) {
    char path[640];
    uint64_t value;
    snprintf(path, sizeof(path), "%s/%s", area, name);
    FILE *f = fopen(path, "r");
    suite_require(f != NULL, path);
    suite_require(fscanf(f, "%" SCNu64, &value) == 1, "CMA number");
    fclose(f);
    return value;
}
static int request(const char *path, const char *text) {
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    suite_require(fd >= 0, path);
    ssize_t n = write(fd, text, strlen(text));
    int error = n < 0 ? errno : n == (ssize_t)strlen(text) ? 0 : EIO;
    close(fd);
    return error;
}
void suite_cma(bool fixed, struct sample *r, unsigned variant) {
    (void)variant;
    DIR *dir = opendir("/sys/kernel/debug/cma");
    suite_require(dir != NULL, "CMA debugfs (CONFIG_CMA_DEBUGFS)");
    struct dirent *entry;
    unsigned found = 0;
    while ((entry = readdir(dir))) {
        if (entry->d_name[0] == '.')
            continue;
        snprintf(area, sizeof(area), "/sys/kernel/debug/cma/%s", entry->d_name);
        found++;
    }
    closedir(dir);
    suite_require(found == 1, "One real CMA area is required for this AM625 exercise");
    const char *control = "/sys/kernel/debug/sadboard-faults/control";
    uint64_t count = number("count"), used = number("used");
    uint64_t base = number("ranges/0/base_pfn");
    long page = sysconf(_SC_PAGESIZE);
    suite_require(page == 4096 && count >= 8192 && count <= 32768, "CMA pool bounds");
    FILE *mem = fopen("/proc/meminfo", "r");
    suite_require(mem != NULL, "meminfo");
    char line[256];
    unsigned long available = 0;
    while (fgets(line, sizeof(line), mem))
        if (sscanf(line, "MemAvailable: %lu kB", &available) == 1)
            break;
    fclose(mem);
    suite_require(available > 256 * 1024, "256MiB free before CMA pinning");
    char text[192], path[640];
    if (!fixed) {
        snprintf(text, sizeof(text), "fragment %" PRIu64 " %" PRIu64 "\n", base * page, count);
        suite_require(request(control, text) == 0, "pin real coherent CMA buffers");
    }
    r->auxiliary = count - number("used");
    r->before = number("maxchunk");
    suite_event("cma_free_pages", r->auxiliary);
    suite_event("cma_maxchunk_pages", r->before);
    snprintf(path, sizeof(path), "%s/alloc", area);
    r->observed = request(path, "4096\n");
    if (!r->observed) {
        snprintf(path, sizeof(path), "%s/free", area);
        suite_require(request(path, "4096\n") == 0, "release own debugfs allocation");
    }
    suite_require(request(control, "release\n") == 0, "release owned DMA buffers");
    r->after = number("used");
    suite_require(r->after == used, "CMA owned allocation cleanup");
    if (fixed)
        suite_require(r->observed == 0, "contiguous allocation after repair");
    else
        suite_require(r->observed == ENOMEM && r->auxiliary >= 4096 && r->before < 4096,
                      "real fragmented CMA ENOMEM with enough aggregate free pages");
    r->completed = 1;
    r->checksum = 4096;
}
