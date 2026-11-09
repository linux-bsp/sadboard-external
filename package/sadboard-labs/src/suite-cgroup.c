#include "suite-advanced.h"
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>
void suite_cgroup_set(struct suite_cgroup *c, const char *file, const char *value)
{
    char path[4200];
    snprintf(path, sizeof(path), "%s/%s", c->path, file);
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0) {
        fprintf(stderr, "owned cgroup control: %s: %s\n", path, strerror(errno));
        exit(70);
    }
    suite_require(write(fd, value, strlen(value)) == (ssize_t)strlen(value),
                  "configure owned cgroup");
    close(fd);
}
void suite_cgroup_create(struct suite_cgroup *c, const char *kind)
{
    char cwd[4096];
    suite_require(getcwd(cwd, sizeof(cwd)) != NULL, "experiment directory");
    const char *name = strrchr(cwd, '/');
    suite_require(name != NULL, "experiment basename");
    snprintf(c->path, sizeof(c->path), "/sys/fs/cgroup/sadboard-suite-%.120s-%ld", name + 1,
             (long)getpid());
    if (mkdir(c->path, 0700)) {
        perror("owned cgroup requires delegation/root");
        exit(75);
    }
    char file[128];
    snprintf(file, sizeof(file), "%s-%ld.cgroup", kind, (long)getpid());
    FILE *f = fopen(file, "w");
    suite_require(f != NULL, "cgroup evidence");
    fprintf(f, "%s\n", c->path);
    fclose(f);
    suite_cgroup_set(c, "memory.max", "134217728");
    suite_cgroup_set(c, "memory.swap.max", "0");
    suite_cgroup_set(c, "pids.max", "4");
}
void suite_cgroup_join(struct suite_cgroup *c, pid_t child)
{
    char value[32];
    snprintf(value, sizeof(value), "%ld", (long)child);
    suite_cgroup_set(c, "cgroup.procs", value);
}
uint64_t suite_cgroup_value(struct suite_cgroup *c, const char *file, const char *key)
{
    char path[4200], line[256];
    snprintf(path, sizeof(path), "%s/%s", c->path, file);
    FILE *f = fopen(path, "r");
    suite_require(f != NULL, "cgroup counter");
    uint64_t value = 0;
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (!key) {
            value = strtoull(line, NULL, 10);
            found = true;
            break;
        }
        if (!strncmp(line, key, strlen(key)) && line[strlen(key)] == ' ') {
            value = strtoull(line + strlen(key) + 1, NULL, 10);
            found = true;
            break;
        }
    }
    fclose(f);
    suite_require(found, "cgroup counter missing");
    return value;
}
void suite_cgroup_remove(struct suite_cgroup *c)
{
    suite_require(!rmdir(c->path), "remove empty owned cgroup");
}
void suite_child_guard(void)
{
    pid_t parent = getppid();
    suite_require(!prctl(PR_SET_PDEATHSIG, SIGKILL), "parent death guard");
    if (parent == 1 || getppid() != parent)
        _exit(70);
}
void suite_pin(unsigned cpu)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    CPU_SET(cpu, &mask);
    suite_require(!sched_setaffinity(0, sizeof(mask), &mask), "bounded experiment CPU affinity");
}
