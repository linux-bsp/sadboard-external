/* Same framed telemetry stream; one send per record versus bounded-size batches. */
#define _POSIX_C_SOURCE 200809L
#include "lab.h"
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define FRAME_BYTES 64
struct receiver { int fd; unsigned records, completed; uint64_t calls, checksum, bytes; };

static void fail(const char *name) { perror(name); exit(70); }
static void timeout_socket(int fd)
{
    struct timeval timeout = {.tv_sec = 3};
    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout))) fail("socket timeout");
}
static void frame(unsigned char *data, unsigned sequence)
{
    uint32_t number = htonl(sequence);
    memcpy(data, &number, sizeof(number));
    for (unsigned i = 4; i < FRAME_BYTES; i++) data[i] = (unsigned char)(sequence * 17 + i * 3 + 42);
}
static void *receive_stream(void *argument)
{
    struct receiver *r = argument;
    unsigned char buffer[4096], pending[FRAME_BYTES], expected[FRAME_BYTES];
    size_t have = 0;
    while (r->completed < r->records) {
        ssize_t size = recv(r->fd, buffer, sizeof(buffer), 0); r->calls++;
        if (size < 0 && errno == EINTR) continue;
        if (size <= 0) fail("incomplete TCP stream");
        r->bytes += (uint64_t)size;
        for (ssize_t i = 0; i < size; i++) {
            if (r->completed == r->records) { errno = EPROTO; fail("extra TCP data"); }
            pending[have++] = buffer[i];
            if (have == FRAME_BYTES) {
                frame(expected, r->completed);
                if (memcmp(pending, expected, FRAME_BYTES)) { errno = EPROTO; fail("TCP frame"); }
                for (unsigned n = 0; n < FRAME_BYTES; n++) r->checksum += pending[n];
                r->completed++; have = 0;
            }
        }
    }
    return NULL;
}

int main(int argc, char **argv)
{
    if (argc != 2) { fprintf(stderr, "Usage: %s RECORDS\n", argv[0]); return 64; }
    unsigned records = lab_number(argv[1], 32768, 0);
    if (records < 64) return 64;
    int listener = socket(AF_INET, SOCK_STREAM, 0);
    if (listener < 0) fail("listener");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    if (bind(listener, (struct sockaddr *)&address, length) || listen(listener, 1) ||
        getsockname(listener, (struct sockaddr *)&address, &length)) fail("listen loopback");
    int client = socket(AF_INET, SOCK_STREAM, 0);
    if (client < 0 || connect(client, (struct sockaddr *)&address, length)) fail("connect loopback");
    struct receiver r = {.fd = accept(listener, NULL, NULL), .records = records};
    if (r.fd < 0) fail("accept");
    close(listener); timeout_socket(client); timeout_socket(r.fd);
    int enabled = 1;
    if (setsockopt(client, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))) fail("TCP_NODELAY");
    uint64_t cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID), wall = lab_clock(CLOCK_MONOTONIC), calls = 0;
    pthread_t thread;
    int result = pthread_create(&thread, NULL, receive_stream, &r);
    if (result) { errno = result; fail("pthread_create"); }
    for (unsigned first = 0; first < records;) {
#ifdef SADBOARD_FIXED
        unsigned count = records - first < 64 ? records - first : 64;
#else
        unsigned count = 1;
#endif
        unsigned char buffer[64 * FRAME_BYTES];
        for (unsigned n = 0; n < count; n++) frame(buffer + n * FRAME_BYTES, first + n);
        size_t sent = 0, bytes = (size_t)count * FRAME_BYTES;
        while (sent < bytes) {
            ssize_t size = send(client, buffer + sent, bytes - sent, MSG_NOSIGNAL); calls++;
            if (size < 0 && errno == EINTR) continue;
            if (size <= 0) fail("send");
            sent += (size_t)size;
        }
        first += count;
    }
    if (shutdown(client, SHUT_WR)) fail("shutdown");
    result = pthread_join(thread, NULL);
    if (result) { errno = result; fail("pthread_join"); }
    wall = lab_clock(CLOCK_MONOTONIC) - wall; cpu = lab_clock(CLOCK_PROCESS_CPUTIME_ID) - cpu;
    close(client); close(r.fd);
    printf("{\"records\":%u,\"frame_bytes\":%u,\"completed\":%u,\"bytes\":%" PRIu64
           ",\"checksum\":%" PRIu64 ",\"send_calls\":%" PRIu64 ",\"recv_calls\":%" PRIu64
           ",\"cpu_ns\":%" PRIu64 ",\"wall_ns\":%" PRIu64 "}\n", records, FRAME_BYTES,
           r.completed, r.bytes, r.checksum, calls, r.calls, cpu, wall);
    return 0;
}
