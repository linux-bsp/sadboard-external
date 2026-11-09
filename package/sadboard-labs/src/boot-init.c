#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/reboot.h>
#include <sys/vfs.h>
#include <unistd.h>
extern int sadboard_init_dependency(void);
/* RAM laboratory only: a stalled recovery PID1 can keep feeding WDT.
 * Hold a proc mount fd across switch_root to check PID1's actual new root. */
static void rescue(unsigned seconds) {
    int proc = -1;
    for (unsigned i = 0; i < seconds; ++i) {
        if (proc < 0) {
            int fd = open("/proc", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
            struct statfs fs;
            if (fd >= 0 && !fstatfs(fd, &fs) && fs.f_type == 0x9fa0)
                proc = fd;
            else if (fd >= 0)
                close(fd);
        }
        if (proc >= 0) {
            int ready = openat(proc, "1/root/run/fwctl-boot-confirmed", O_RDONLY | O_CLOEXEC);
            if (ready >= 0) {
                close(ready);
                close(proc);
                _exit(0);
            }
        }
        sleep(1);
    }
    puts("SadBoard RAM rescue guard: timeout; rebooting to saved normal profile");
    fflush(stdout);
    reboot(RB_AUTOBOOT);
    _exit(71);
}
int main(void) {
    if (getpid() != 1)
        return 64;
    if (sadboard_init_dependency() != 42)
        return 70;
    puts("SadBoard PID1 shared-library dependency restored; entering verified fwctl init");
    fflush(stdout);
    int test = getenv("SADBOARD_RESCUE_TEST") != NULL;
    pid_t guard = fork();
    if (guard < 0)
        return 72;
    if (!guard)
        rescue(test ? 10 : 120);
    if (test)
        for (;;)
            pause();
    execl("/usr/sbin/fwctl", "fwctl", "init", (char *)NULL);
    perror("fwctl init");
    return 71;
}
