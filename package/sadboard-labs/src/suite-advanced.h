#ifndef SADBOARD_ADVANCED_H
#define SADBOARD_ADVANCED_H
#include "suite.h"
#include <sched.h>
#include <stdatomic.h>
struct suite_cgroup {
    char path[4096];
};
void suite_adv_perf(bool fixed, struct sample *r, unsigned kind);
void suite_adv_mem(bool fixed, struct sample *r, unsigned kind);
void suite_adv_net(bool fixed, struct sample *r, unsigned kind);
void suite_cgroup_create(struct suite_cgroup *c, const char *kind);
void suite_cgroup_set(struct suite_cgroup *c, const char *file, const char *value);
uint64_t suite_cgroup_value(struct suite_cgroup *c, const char *file, const char *key);
void suite_cgroup_join(struct suite_cgroup *c, pid_t child);
void suite_cgroup_remove(struct suite_cgroup *c);
void suite_child_guard(void);
void suite_pin(unsigned cpu);
int suite_peer(int argc, char **argv);
#endif
