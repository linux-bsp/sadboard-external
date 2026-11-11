#ifndef SADBOARD_SUITE_H
#define SADBOARD_SUITE_H
#define _GNU_SOURCE
#include "lab.h"
#include <pthread.h>
#include <stdbool.h>
#include <sys/types.h>

struct sample {
    uint64_t completed, checksum, observed, auxiliary, before, after;
    uint64_t cpu_ns, wall_ns;
};
struct suite_case {
    const char *name, *id, *description;
    void (*run)(bool fixed, struct sample *result);
};
extern const struct suite_case suite_cases[];
extern const size_t suite_case_count;
extern char suite_executable[4096];
extern FILE *suite_trace;
void suite_event(const char *name, uint64_t value);
void suite_require(bool condition, const char *reason);
void suite_thread(int error);
unsigned suite_fds(void);
uint64_t suite_status(const char *key);
int suite_wait(pid_t child, unsigned milliseconds, bool *timed_out);
void suite_fault(bool fixed, struct sample *r, unsigned kind);
void suite_fd(bool fixed, struct sample *r, unsigned kind);
void suite_mem(bool fixed, struct sample *r, unsigned kind);
void suite_cpu(bool fixed, struct sample *r, unsigned kind);
void suite_sync(bool fixed, struct sample *r, unsigned kind);
void suite_net(bool fixed, struct sample *r, unsigned kind);
void suite_assembly(bool fixed, struct sample *r, unsigned kind);
void suite_kernel(bool fixed, struct sample *r, unsigned kind);
#endif

void suite_cma(bool fixed, struct sample *r, unsigned variant);
