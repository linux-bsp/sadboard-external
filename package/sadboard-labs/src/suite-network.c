#include "suite.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
extern void suite_tcp_pair(int pair[2]);
struct sender {
    int fd;
    pthread_mutex_t lock;
    pthread_cond_t condition;
    bool first_read;
};
static void *fragmented_sender(void *argument)
{
    struct sender *s = argument;
    unsigned char frame[12] = {0, 0, 0, 8, 1, 2, 3, 4, 5, 6, 7, 8};
    suite_require(send(s->fd, frame, 3, MSG_NOSIGNAL) == 3, "fragment header");
    suite_thread(pthread_mutex_lock(&s->lock));
    while (!s->first_read)
        suite_thread(pthread_cond_wait(&s->condition, &s->lock));
    suite_thread(pthread_mutex_unlock(&s->lock));
    suite_require(send(s->fd, frame + 3, 9, MSG_NOSIGNAL) == 9, "fragment remainder");
    return NULL;
}
static uint64_t udp_drops(void)
{
    FILE *f = fopen("/proc/net/snmp", "r");
    suite_require(f != NULL, "UDP statistics");
    char header[4096], values[4096];
    uint64_t drops = 0;
    while (fgets(header, sizeof(header), f)) {
        if (strncmp(header, "Udp:", 4))
            continue;
        suite_require(fgets(values, sizeof(values), f) != NULL, "UDP values");
        char *a, *b, *key = strtok_r(header, " \n", &a), *value = strtok_r(values, " \n", &b);
        while (key && value) {
            if (!strcmp(key, "RcvbufErrors"))
                drops = strtoull(value, NULL, 10);
            key = strtok_r(NULL, " \n", &a);
            value = strtok_r(NULL, " \n", &b);
        }
        break;
    }
    fclose(f);
    return drops;
}
void suite_net(bool fixed, struct sample *r, unsigned kind)
{
    if (kind == 0) {
        int pair[2];
        suite_tcp_pair(pair);
        struct sender sender = {.fd = pair[0],
                                .lock = PTHREAD_MUTEX_INITIALIZER,
                                .condition = PTHREAD_COND_INITIALIZER};
        pthread_t thread;
        suite_thread(pthread_create(&thread, NULL, fragmented_sender, &sender));
        unsigned char frame[12];
        size_t got = 0;
        do {
            ssize_t n = recv(pair[1], frame + got, sizeof(frame) - got, 0);
            suite_require(n > 0, "framed recv");
            got += (size_t)n;
            suite_thread(pthread_mutex_lock(&sender.lock));
            sender.first_read = true;
            suite_thread(pthread_cond_signal(&sender.condition));
            suite_thread(pthread_mutex_unlock(&sender.lock));
        } while (fixed && got < sizeof(frame));
        r->observed = got;
        r->auxiliary = 12;
        if (got == 12) {
            uint32_t length;
            memcpy(&length, frame, 4);
            suite_require(ntohl(length) == 8, "framed length");
            for (unsigned i = 4; i < 12; i++)
                r->checksum += frame[i];
            r->completed = 1;
        }
        suite_thread(pthread_join(thread, NULL));
        pthread_cond_destroy(&sender.condition);
        pthread_mutex_destroy(&sender.lock);
        close(pair[0]);
        close(pair[1]);
    } else if (kind == 1) {
        uint32_t wire = htonl(1024), decoded = fixed ? ntohl(wire) : wire;
        int pair[2];
        suite_tcp_pair(pair);
        suite_require(send(pair[0], &wire, 4, MSG_NOSIGNAL) == 4, "wire length");
        uint32_t received;
        suite_require(recv(pair[1], &received, 4, MSG_WAITALL) == 4, "read length");
        decoded = fixed ? ntohl(received) : received;
        r->observed = decoded;
        r->auxiliary = 4096;
        if (decoded <= 4096) {
            r->completed = 1;
            r->checksum = decoded;
        }
        close(pair[0]);
        close(pair[1]);
    } else if (kind == 2) {
        int receiver = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0),
            sender = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
        suite_require(receiver >= 0 && sender >= 0, "UDP sockets");
        int buffer = 4096;
        suite_require(!setsockopt(receiver, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)),
                      "small receive queue");
        struct timeval timeout = {.tv_usec = 50000};
        suite_require(!setsockopt(receiver, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)),
                      "UDP timeout");
        struct sockaddr_in address = {.sin_family = AF_INET,
                                      .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
        socklen_t size = sizeof(address);
        suite_require(!bind(receiver, (struct sockaddr *)&address, size) &&
                          !getsockname(receiver, (struct sockaddr *)&address, &size),
                      "UDP bind");
        r->before = udp_drops();
        unsigned char payload[512];
        for (unsigned i = 0; i < 128; i++) {
            memset(payload, (int)i, sizeof(payload));
            suite_require(sendto(sender, payload, sizeof(payload), 0, (struct sockaddr *)&address,
                                 size) == sizeof(payload),
                          "UDP send");
            r->auxiliary++;
            if (fixed) {
                ssize_t n = recv(receiver, payload, sizeof(payload), 0);
                suite_require(n == sizeof(payload), "timely receive");
                r->completed++;
                r->checksum += payload[0];
            }
        }
        if (!fixed)
            for (;;) {
                ssize_t n = recv(receiver, payload, sizeof(payload), 0);
                if (n < 0) {
                    suite_require(errno == EAGAIN, "UDP drain");
                    break;
                }
                suite_require(n == sizeof(payload), "UDP size");
                r->completed++;
                r->checksum += payload[0];
            }
        r->after = udp_drops();
        r->observed = r->after - r->before;
        close(receiver);
        close(sender);
    } else {
        int pair[2];
        suite_tcp_pair(pair);
        close(pair[0]);
        char byte;
        ssize_t n = recv(pair[1], &byte, 1, 0);
        r->after = n == 0 ? 0 : 1;
        suite_require(n == 0, "normal FIN EOF");
        close(pair[1]);
        r->completed++;
        r->checksum++;
        suite_tcp_pair(pair);
        struct linger reset = {.l_onoff = 1, .l_linger = 0};
        suite_require(!setsockopt(pair[0], SOL_SOCKET, SO_LINGER, &reset, sizeof(reset)),
                      "reset linger");
        close(pair[0]);
        n = recv(pair[1], &byte, 1, 0);
        r->before = n < 0 ? (unsigned)errno : 0;
        suite_require(n < 0 && errno == ECONNRESET, "RST receive");
        close(pair[1]);
        r->completed++;
        r->checksum++;
        pid_t child = fork();
        suite_require(child >= 0, "SIGPIPE child");
        if (!child) {
            signal(SIGPIPE, SIG_DFL);
            int local[2];
            suite_require(!socketpair(AF_UNIX, SOCK_STREAM, 0, local), "SIGPIPE pair");
            close(local[1]);
            ssize_t sent = send(local[0], "x", 1, fixed ? MSG_NOSIGNAL : 0);
            _exit(sent < 0 && errno == EPIPE ? 32 : 70);
        }
        bool timed;
        int status = suite_wait(child, 1000, &timed);
        suite_require(!timed, "SIGPIPE timeout");
        r->observed = WIFSIGNALED(status) ? (unsigned)WTERMSIG(status) : 0;
        r->auxiliary = WIFEXITED(status) ? (unsigned)WEXITSTATUS(status) : 0;
        if ((!fixed && r->observed == SIGPIPE) || (fixed && r->auxiliary == 32))
            r->checksum++;
        r->completed++;
    }
    suite_event("network_observation", r->observed);
}
