#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/ptrace.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t interrupted;
static void stop(int signal) { interrupted = signal; }
int main(int argc, char **argv) {
    if (argc >= 3 && !strcmp(argv[1], "--lock")) {
        /* Inherit the lock across exec; it also releases after SIGKILL. */
        int fd = open("/run/sadboard-kernel.lock", O_CREAT | O_RDWR | O_NOFOLLOW, 0600);
        if (fd < 0 || flock(fd, LOCK_EX | LOCK_NB)) {
            perror("another kernel laboratory owns the module");
            return 75;
        }
        execvp(argv[2], argv + 2);
        perror("execute kernel laboratory");
        return 70;
    }
    if (argc != 1)
        return 64;
    int status, attached = 0;
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGINT, stop);
    signal(SIGTERM, stop);
    signal(SIGHUP, stop);
    if (geteuid() != 0 || ptrace(PTRACE_SEIZE, 1, NULL, 0)) {
        perror("PTRACE_SEIZE PID1");
        return 75;
    }
    attached = 1;
    if (ptrace(PTRACE_INTERRUPT, 1, NULL, NULL) || waitpid(1, &status, __WALL) != 1 ||
        !WIFSTOPPED(status)) {
        perror("stop PID1 watchdog feeder");
        goto detach;
    }
    puts("SadBoard watchdog probe: PID1 feeder is ptrace-stopped; kernel and tracer remain "
         "responsive");
    for (unsigned i = 0; i < 130 && !interrupted; ++i) {
        FILE *f = fopen("/sys/class/watchdog/watchdog0/timeleft", "r");
        unsigned left = 0;
        if (f) {
            if (fscanf(f, "%u", &left) != 1)
                left = 0;
            fclose(f);
        }
        printf("probe_elapsed=%u watchdog_timeleft=%u\n", i, left);
        sleep(1);
    }
detach:
    if (attached && ptrace(PTRACE_DETACH, 1, NULL, NULL))
        perror("resume PID1");
    fputs("Hardware watchdog reset was NOT observed; PID1 resumed; do not run hard lockup\n",
          stderr);
    return 75;
}
