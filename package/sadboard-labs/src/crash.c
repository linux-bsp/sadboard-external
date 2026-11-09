/* Guided exercise: a failed lookup reaches a pointer dereference unchecked. */
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>

struct record {
    int key;
    int value;
};

static const struct record *lookup_record(int key)
{
    static const struct record record = {1, 64};
    return key == record.key ? &record : NULL;
}

static int process_request(int key)
{
    const struct record *record = lookup_record(key);
#ifdef SADBOARD_FIXED
    if (!record) {
        fprintf(stderr, "record %d not found\n", key);
        return 2;
    }
#endif
    printf("record=%d value=%d\n", key, record->value);
    return 0;
}

int main(int argc, char **argv)
{
    char *end;
    if (argc != 2) {
        fprintf(stderr, "Usage: %s KEY\n", argv[0]);
        return 64;
    }
    errno = 0;
    long key = strtol(argv[1], &end, 10);
    if (errno || !*argv[1] || *end || key < 0 || key > INT_MAX) {
        fprintf(stderr, "KEY must be a nonnegative integer\n");
        return 64;
    }
    return process_request((int)key);
}
