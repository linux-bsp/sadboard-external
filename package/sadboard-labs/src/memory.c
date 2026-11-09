/* An intentional missing free; invoke with a dedicated memory cgroup. */
#define _POSIX_C_SOURCE 200809L
#include "lab.h"

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "Usage: %s CHUNKS DELAY_MS\n", argv[0]);
        return 64;
    }
    unsigned chunks = lab_number(argv[1], 128, 0);
    unsigned delay = lab_number(argv[2], 200, 1);
    if (chunks * delay > 10000) {
        fprintf(stderr, "duration budget exceeded\n");
        return 64;
    }
    uint64_t checksum = 0;
    for (unsigned i = 0; i < chunks; i++) {
        volatile unsigned char *block = malloc(1024 * 1024);
        if (!block) {
            perror("malloc");
            return 71;
        }
        /* Volatile page writes commit physical memory, even with optimization. */
        for (unsigned offset = 0; offset < 1024 * 1024; offset += 4096) {
            block[offset] = (unsigned char)(i + 1);
            checksum += block[offset];
        }
#ifdef SADBOARD_FIXED
        free((void *)block);
#endif
        lab_sleep_us(delay * 1000);
    }
    printf("{\"chunks\":%u,\"checksum\":%" PRIu64 "}\n", chunks, checksum);
    return 0;
}
