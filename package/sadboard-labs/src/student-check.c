/* Evaluate a learner's B-CR01 executable against a bounded public contract. */
#define _GNU_SOURCE
#include "lab.h"
#include <elf.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

struct test {
    const char *label, *argument;
    unsigned expected;
    bool extra;
};
static const struct test tests[] = {
    {"valid", "1", 0, false}, {"missing", "42", 2, false},
    {"zero", "0", 2, false}, {"maximum", "2147483647", 2, false},
    {"text", "bad", 64, false}, {"negative", "-1", 64, false},
    {"empty", "", 64, false}, {"overflow", "999999999999999999999", 64, false},
    {"no-argument", NULL, 64, false}, {"extra-argument", "1", 64, true},
};

static void fail(const char *message)
{
    fprintf(stderr, "[sadboard] [ERROR] Candidate evaluation: %s\n", message);
    exit(75);
}

static size_t snapshot(const char *source)
{
    int input = open(source, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    struct stat before, after;
    Elf64_Ehdr header;
    if (input < 0 || fstat(input, &before) || !S_ISREG(before.st_mode) ||
        before.st_size < (off_t)sizeof(header) || before.st_size > 16 * 1024 * 1024 ||
        read(input, &header, sizeof(header)) != sizeof(header))
        fail("use a regular ELF executable no larger than 16 MiB");
#if defined(__aarch64__)
    const unsigned machine = EM_AARCH64;
#elif defined(__x86_64__)
    const unsigned machine = EM_X86_64;
#else
    fail("unsupported evaluation architecture");
    const unsigned machine = 0;
#endif
    if (memcmp(header.e_ident, ELFMAG, SELFMAG) || header.e_ident[EI_CLASS] != ELFCLASS64 ||
        header.e_ident[EI_DATA] != ELFDATA2LSB || header.e_machine != machine ||
        (header.e_type != ET_EXEC && header.e_type != ET_DYN))
        fail("candidate ISA/class/endianness/type does not match this machine");
    int output = open("candidate.elf", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0700);
    if (output < 0 || lseek(input, 0, SEEK_SET))
        fail("cannot create the candidate snapshot");
    char buffer[16384];
    off_t total = 0;
    ssize_t count;
    while ((count = read(input, buffer, sizeof(buffer))) > 0) {
        if (total + count > before.st_size)
            fail("candidate changed during snapshot");
        ssize_t position = 0;
        while (position < count) {
            ssize_t written = write(output, buffer + position, count - position);
            if (written <= 0) fail("candidate snapshot write failed");
            position += written;
        }
        total += count;
    }
    if (count < 0 || total != before.st_size || fstat(input, &after) ||
        after.st_size != before.st_size ||
        after.st_mtim.tv_sec != before.st_mtim.tv_sec || after.st_mtim.tv_nsec != before.st_mtim.tv_nsec ||
        fsync(output) || close(output))
        fail("candidate snapshot is incomplete or input changed");
    close(input);
    return total;
}

static size_t contents(const char *path, char *buffer, size_t capacity)
{
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) fail("missing per-input output");
    ssize_t count = read(fd, buffer, capacity - 1);
    if (count < 0) fail("cannot read per-input output");
    buffer[count] = 0;
    close(fd);
    return count;
}

static bool evaluate(unsigned index, FILE *results)
{
    const struct test *test = &tests[index];
    char out[64], err[64];
    snprintf(out, sizeof(out), "student-%02u.stdout", index);
    snprintf(err, sizeof(err), "student-%02u.stderr", index);
    int output = open(out, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    int errors = open(err, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (output < 0 || errors < 0) fail("per-input output already exists or cannot be created");
    pid_t child = fork();
    if (child < 0) fail("fork");
    if (!child) {
        const struct rlimit core = {0, 0}, cpu = {1, 1}, files = {16384, 16384};
        if (setpgid(0, 0) || setrlimit(RLIMIT_CORE, &core) ||
            setrlimit(RLIMIT_CPU, &cpu) || setrlimit(RLIMIT_FSIZE, &files) ||
            dup2(output, STDOUT_FILENO) < 0 || dup2(errors, STDERR_FILENO) < 0)
            _exit(126);
        close(output);
        close(errors);
        if (test->extra)
            execl("./candidate.elf", "candidate.elf", test->argument, "extra", (char *)NULL);
        else if (test->argument)
            execl("./candidate.elf", "candidate.elf", test->argument, (char *)NULL);
        else
            execl("./candidate.elf", "candidate.elf", (char *)NULL);
        _exit(126);
    }
    close(output);
    close(errors);
    /* The child does the same operation before exec; this closes the parent-side race. */
    if (setpgid(child, child) && errno != EACCES && errno != ESRCH)
        fail("candidate process group");
    int status = 0;
    bool timed_out = false;
    uint64_t deadline = lab_clock(CLOCK_MONOTONIC) + UINT64_C(1500000000);
    for (;;) {
        pid_t done = waitpid(child, &status, WNOHANG);
        if (done == child) break;
        if (done < 0 && errno != EINTR) fail("candidate wait");
        if (lab_clock(CLOCK_MONOTONIC) >= deadline) {
            timed_out = true;
            kill(-child, SIGKILL);
            while (waitpid(child, &status, 0) < 0)
                if (errno != EINTR) fail("candidate timeout wait");
            break;
        }
        lab_sleep_us(1000);
    }
    kill(-child, SIGKILL);
    unsigned code = WIFEXITED(status) ? WEXITSTATUS(status) : 128U + WTERMSIG(status);
    char stdout_text[16385], stderr_text[16385];
    size_t length = contents(out, stdout_text, sizeof(stdout_text));
    size_t errors_length = contents(err, stderr_text, sizeof(stderr_text));
    bool passed = !timed_out && WIFEXITED(status) && code == test->expected;
    if (index == 0)
        passed &= length == strlen("record=1 value=64\n") && !strcmp(stdout_text, "record=1 value=64\n");
    else
        passed &= !length && errors_length > 0;
    fprintf(results, "%u,%s,%u,%u,%u,%u\n", index, test->label, test->expected, code,
            timed_out, passed);
    printf("[sadboard] [INFO] candidate input=%s expected=%u observed=%u result=%s\n",
           test->label, test->expected, code, passed ? "pass" : "fail");
    return passed;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 64;
    /* Resolve the source before chdir; the public launcher passes an absolute path. */
    char *source = realpath(argv[1], NULL);
    if (!source || chdir(argv[2])) fail("candidate or evidence directory unavailable");
    size_t size = snapshot(source);
    free(source);
    FILE *results = fopen("student-results.csv", "wx");
    if (!results) fail("cannot create input/result table");
    fputs("index,input,expected,observed,timeout,passed\n", results);
    unsigned passed = 0, count = sizeof(tests) / sizeof(tests[0]);
    for (unsigned i = 0; i < count; i++) passed += evaluate(i, results);
    if (fclose(results)) fail("cannot save result table");
    FILE *summary = fopen("student-summary.out", "wx");
    if (!summary) fail("cannot create summary");
    fprintf(summary, "{\"tests\":%u,\"passed\":%u,\"failed\":%u,\"candidate_size\":%zu}\n",
            count, passed, count - passed, size);
    if (fclose(summary)) fail("cannot save summary");
    return passed == count ? 0 : 1;
}
