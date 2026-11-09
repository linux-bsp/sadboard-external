#define _GNU_SOURCE
#include "suite-peer.h"
#include "suite-advanced.h"
#include <arpa/inet.h>
#include <poll.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
static int bound_socket(int type, unsigned port)
{
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    suite_require(fd >= 0, "peer socket");
    int yes = 1;
    suite_require(!setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)), "peer reuse");
    struct sockaddr_in address = {.sin_family = AF_INET,
                                  .sin_port = htons((uint16_t)port),
                                  .sin_addr.s_addr = htonl(INADDR_ANY)};
    suite_require(!bind(fd, (struct sockaddr *)&address, sizeof(address)), "peer bind");
    return fd;
}
static void ready(const char *mode, const char *variant)
{
    char name[128];
    snprintf(name, sizeof(name), "%s-%s.ready", mode, variant);
    FILE *f = fopen(name, "w");
    suite_require(f != NULL, "peer ready");
    fprintf(f, "%ld\n", (long)getpid());
    suite_require(!fclose(f), "peer ready close");
}
static void peer_pmtu(const char *variant)
{
    int fd = bound_socket(SOCK_DGRAM, SUITE_DATA_PORT);
    ready("pmtu", variant);
    uint32_t total = 0, sum = 0;
    for (;;) {
        struct pollfd p = {.fd = fd, .events = POLLIN};
        suite_require(poll(&p, 1, 3000) > 0, "PMTU peer receive timeout");
        struct suite_datagram packet;
        struct sockaddr_in source;
        socklen_t size = sizeof(source);
        ssize_t n = recvfrom(fd, &packet, sizeof(packet), 0, (struct sockaddr *)&source, &size);
        suite_require(n >= 12, "PMTU datagram header");
        uint32_t kind = ntohl(packet.kind), offset = ntohl(packet.offset),
                 length = ntohl(packet.length);
        suite_require(length <= 1500 && n == 12 + (ssize_t)length, "PMTU datagram length");
        if (kind == 1) {
            suite_require(length == 64, "small probe");
        } else if (kind == 2) {
            suite_require(offset == total && total + length <= 1400, "PMTU stream order");
            for (unsigned i = 0; i < length; i++) {
                suite_require(packet.data[i] == (unsigned char)((offset + i) & 255),
                              "PMTU content");
                sum += packet.data[i];
            }
            total += length;
        } else
            suite_require(kind == 4 && !length, "PMTU finish");
        struct suite_ack ack = {htonl(kind), htonl(total), htonl(sum),
                                htonl(kind == 1 ? length : 0)};
        suite_require(sendto(fd, &ack, sizeof(ack), 0, (struct sockaddr *)&source, size) ==
                          sizeof(ack),
                      "PMTU acknowledgement");
        if (kind == 4)
            break;
    }
    printf("{\"received_bytes\":%u,\"checksum\":%u}\n", total, sum);
    close(fd);
}
static void peer_rps(const char *variant)
{
    suite_pin(3);
    int listener = bound_socket(SOCK_STREAM, SUITE_DATA_PORT);
    suite_require(!listen(listener, 1), "RPS listener");
    ready("rps", variant);
    struct pollfd p = {.fd = listener, .events = POLLIN};
    suite_require(poll(&p, 1, 3000) > 0, "RPS accept timeout");
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    suite_require(fd >= 0, "RPS accept");
    close(listener);
    struct timeval timeout = {.tv_sec = 3};
    suite_require(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
                  "RPS receive timeout");
    unsigned char data[4096];
    uint32_t total = 0, sum = 0;
    for (;;) {
        ssize_t n = recv(fd, data, sizeof(data), 0);
        suite_require(n >= 0, "RPS receive");
        if (!n)
            break;
        for (ssize_t i = 0; i < n; i++) {
            suite_require(data[i] == 42, "RPS payload");
            sum += data[i];
        }
        total += (uint32_t)n;
    }
    int cpu = -1;
    socklen_t length = sizeof(cpu);
    suite_require(!getsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &cpu, &length) && cpu >= 0,
                  "kernel incoming CPU");
    struct suite_ack ack = {htonl(3), htonl(total), htonl(sum), htonl((uint32_t)cpu)};
    suite_require(send(fd, &ack, sizeof(ack), MSG_NOSIGNAL) == sizeof(ack), "RPS result");
    close(fd);
    printf("{\"received_bytes\":%u,\"checksum\":%u,\"incoming_cpu\":%d}\n", total, sum, cpu);
}
static void peer_priority(const char *variant)
{
    int data = bound_socket(SOCK_DGRAM, SUITE_DATA_PORT),
        control = bound_socket(SOCK_DGRAM, SUITE_CONTROL_PORT);
    ready("priority", variant);
    struct pollfd p[2] = {{.fd = data, .events = POLLIN}, {.fd = control, .events = POLLIN}};
    bool bulk_seen[64] = {0}, control_seen[16] = {0};
    unsigned bulk = 0, commands = 0, sum = 0;
    struct sockaddr_in destination = {0};
    socklen_t destination_size = sizeof(destination);
    while (bulk < 64 || commands < 16) {
        suite_require(poll(p, 2, 3000) > 0, "priority peer timeout");
        for (unsigned slot = 0; slot < 2; slot++)
            if (p[slot].revents & POLLIN) {
                unsigned char packet[512];
                struct sockaddr_in source;
                socklen_t size = sizeof(source);
                ssize_t n = recvfrom(p[slot].fd, packet, sizeof(packet), 0,
                                     (struct sockaddr *)&source, &size);
                suite_require(n == (slot ? 64 : 512) && packet[0] == (slot ? 'C' : 'B'),
                              "priority datagram");
                unsigned sequence = packet[1];
                for (ssize_t i = 2; i < n; i++)
                    suite_require(packet[i] == 42, "priority payload");
                if (!slot) {
                    suite_require(sequence < 64 && !bulk_seen[sequence], "unique bulk packet");
                    bulk_seen[sequence] = true;
                    bulk++;
                    sum += sequence;
                } else {
                    suite_require(sequence < 16 && !control_seen[sequence], "unique control");
                    control_seen[sequence] = true;
                    commands++;
                    sum += sequence;
                    destination = source;
                    destination_size = size;
                    struct suite_ack ack = {htonl(1), htonl(sequence), htonl(sum), 0};
                    suite_require(sendto(control, &ack, sizeof(ack), 0, (struct sockaddr *)&source,
                                         size) == sizeof(ack),
                                  "control acknowledgement");
                }
            }
    }
    struct suite_ack done = {htonl(4), htonl(bulk + commands), htonl(sum), htonl(bulk)};
    suite_require(sendto(control, &done, sizeof(done), 0, (struct sockaddr *)&destination,
                         destination_size) == sizeof(done),
                  "priority completion");
    close(data);
    close(control);
    printf("{\"bulk\":%u,\"control\":%u,\"checksum\":%u}\n", bulk, commands, sum);
}
int suite_peer(int argc, char **argv)
{
    if (argc != 5 || (strcmp(argv[3], "bug") && strcmp(argv[3], "fixed")))
        return 64;
    suite_require(!chdir(argv[4]), "peer result directory");
    if (!strcmp(argv[2], "pmtu"))
        peer_pmtu(argv[3]);
    else if (!strcmp(argv[2], "rps"))
        peer_rps(argv[3]);
    else if (!strcmp(argv[2], "priority"))
        peer_priority(argv[3]);
    else
        return 64;
    return 0;
}
