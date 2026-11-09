/* Same queries and checksum; the reference uses the table's direct index. */
#define _POSIX_C_SOURCE 200809L
#include "lab.h"

struct entry { uint32_t key, value; };

static uint32_t lookup_linear(const struct entry *table, unsigned items, unsigned key)
{
    for (unsigned i = 0; i < items; i++)
        if (table[i].key == key)
            return table[i].value;
    return 0;
}

static uint32_t lookup_indexed(const struct entry *table, unsigned items, unsigned key)
{
    return key < items ? table[key].value : 0;
}

int main(int argc, char **argv)
{
    if (argc != 4) {
        fprintf(stderr, "Usage: %s ITEMS QUERIES SEED\n", argv[0]);
        return 64;
    }
    unsigned items = lab_number(argv[1], 16384, 0);
    unsigned queries = lab_number(argv[2], 2000000, 0);
    uint32_t seed = lab_number(argv[3], UINT32_MAX, 1), random = seed;
    if ((uint64_t)items * queries > UINT64_C(4000000000)) {
        fprintf(stderr, "work budget exceeded\n");
        return 64;
    }
    struct entry *table = malloc((size_t)items * sizeof(*table));
    if (!table) {
        perror("malloc");
        return 71;
    }
    for (unsigned i = 0; i < items; i++)
        table[i] = (struct entry){i, i * 3 + 7};
    uint64_t checksum = 0, wall = lab_clock(CLOCK_MONOTONIC);
    uint64_t cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID);
    for (unsigned q = 0; q < queries; q++) {
        random = random * UINT32_C(1664525) + UINT32_C(1013904223);
        unsigned key = random % items;
#ifdef SADBOARD_FIXED
        checksum += lookup_indexed(table, items, key);
#else
        checksum += lookup_linear(table, items, key);
#endif
    }
    cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    wall = lab_clock(CLOCK_MONOTONIC) - wall;
    printf("{\"items\":%u,\"queries\":%u,\"seed\":%u,\"checksum\":%" PRIu64
           ",\"cpu_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64 "}\n",
           items, queries, seed, checksum, cpu, wall);
    free(table);
    return 0;
}
