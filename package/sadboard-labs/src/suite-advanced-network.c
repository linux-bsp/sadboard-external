#include "suite-advanced.h"
#include "suite-peer.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <linux/errqueue.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
extern void suite_tcp_pair(int pair[2]);
static void timeout_socket(int fd)
{
    struct timeval timeout = {.tv_sec = 3};
    suite_require(!setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) &&
                      !setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)),
                  "bounded socket wait");
}
static int peer_connection(int type, const char *address, unsigned port)
{
    int fd = socket(AF_INET, type | SOCK_CLOEXEC, 0);
    suite_require(fd >= 0, "test socket");
    timeout_socket(fd);
    struct sockaddr_in peer = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port)};
    suite_require(inet_pton(AF_INET, address, &peer.sin_addr) == 1 &&
                      !connect(fd, (struct sockaddr *)&peer, sizeof(peer)),
                  "private peer connect");
    return fd;
}
struct queued_message {
    struct queued_message *next;
    unsigned sequence;
    unsigned char bytes[4096];
};
struct slow_peer {
    pthread_mutex_t lock;
    pthread_cond_t ready, space;
    struct queued_message *head, *tail;
    unsigned count, peak, limit, finished;
    int tx, rx;
    uint64_t sum, send_queue;
};
static void *slow_receive(void *arg)
{
    struct slow_peer *p = arg;
    lab_sleep_us(100000);
    unsigned char data[4096];
    for (unsigned i = 0; i < 64; i++) {
        suite_require(recv(p->rx, data, sizeof(data), MSG_WAITALL) == sizeof(data),
                      "slow peer frame");
        for (unsigned j = 0; j < sizeof(data); j++)
            suite_require(data[j] == i, "slow peer integrity");
        p->sum += data[0];
        p->finished++;
    }
    return NULL;
}
static void *queued_send(void *arg)
{
    struct slow_peer *p = arg;
    for (unsigned i = 0; i < 64; i++) {
        suite_thread(pthread_mutex_lock(&p->lock));
        while (!p->head)
            suite_thread(pthread_cond_wait(&p->ready, &p->lock));
        struct queued_message *message = p->head;
        p->head = message->next;
        if (!p->head)
            p->tail = NULL;
        p->count--;
        suite_thread(pthread_cond_signal(&p->space));
        suite_thread(pthread_mutex_unlock(&p->lock));
        size_t sent = 0;
        while (sent < sizeof(message->bytes)) {
            ssize_t n =
                send(p->tx, message->bytes + sent, sizeof(message->bytes) - sent, MSG_NOSIGNAL);
            suite_require(n > 0, "slow socket send");
            sent += (size_t)n;
        }
        int queued = 0;
        suite_require(!ioctl(p->tx, TIOCOUTQ, &queued), "kernel send queue");
        if ((uint64_t)queued > p->send_queue)
            p->send_queue = (unsigned)queued;
        free(message);
    }
    return NULL;
}
static void batch_udp(bool fixed, struct sample *r)
{
    int rx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0),
        tx = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    suite_require(rx >= 0 && tx >= 0, "batch UDP sockets");
    timeout_socket(rx);
    int size = 262144;
    suite_require(!setsockopt(rx, SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)), "bounded UDP queue");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t length = sizeof(address);
    suite_require(!bind(rx, (struct sockaddr *)&address, length) &&
                      !getsockname(rx, (struct sockaddr *)&address, &length),
                  "batch bind");
    unsigned char data[16][512];
    for (unsigned i = 0; i < 128; i++) {
        memset(data[0], (int)i, 512);
        suite_require(sendto(tx, data[0], 512, 0, (struct sockaddr *)&address, length) == 512,
                      "burst data");
    }
    while (r->completed < 128) {
        struct mmsghdr messages[16] = {0};
        struct iovec buffers[16];
        unsigned wanted = fixed ? 16 : 1;
        for (unsigned i = 0; i < wanted; i++) {
            buffers[i] = (struct iovec){data[i], 512};
            messages[i].msg_hdr.msg_iov = &buffers[i];
            messages[i].msg_hdr.msg_iovlen = 1;
        }
        int got;
        if (fixed)
            got = recvmmsg(rx, messages, wanted, MSG_WAITFORONE, NULL);
        else {
            ssize_t n = recv(rx, data[0], 512, 0);
            got = n >= 0 ? 1 : -1;
            messages[0].msg_len = (unsigned)n;
        }
        suite_require(got > 0, "batch receive");
        r->observed++;
        for (int i = 0; i < got; i++) {
            suite_require(messages[i].msg_len == 512 && data[i][0] == r->completed,
                          "ordered burst");
            for (unsigned j = 1; j < 512; j++)
                suite_require(data[i][j] == data[i][0], "burst bytes");
            r->checksum += data[i][0];
            r->completed++;
        }
    }
    close(rx);
    close(tx);
}
static void edge_budget(bool fixed, struct sample *r)
{
    int data[2], control[2];
    suite_require(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, data) &&
                      !socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, control),
                  "edge streams");
    int ep = epoll_create1(EPOLL_CLOEXEC);
    suite_require(ep >= 0, "edge epoll");
    struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.fd = data[1]};
    suite_require(!epoll_ctl(ep, EPOLL_CTL_ADD, data[1], &event), "data edge");
    event.data.fd = control[1];
    suite_require(!epoll_ctl(ep, EPOLL_CTL_ADD, control[1], &event), "control edge");
    unsigned char payload[64];
    memset(payload, 1, sizeof(payload));
    suite_require(send(data[0], payload, sizeof(payload), MSG_NOSIGNAL) == sizeof(payload) &&
                      send(control[0], "*", 1, MSG_NOSIGNAL) == 1,
                  "edge input");
    bool runnable = false;
    for (unsigned round = 0; round < 20; round++) {
        struct epoll_event events[2];
        int n = epoll_wait(ep, events, 2, runnable ? 0 : 50);
        suite_require(n >= 0, "edge wait");
        for (int i = 0; i < n; i++)
            if (events[i].data.fd == control[1]) {
                unsigned char byte;
                suite_require(read(control[1], &byte, 1) == 1, "control progress");
                r->checksum += byte;
                r->completed++;
                r->auxiliary++;
            } else
                runnable = true;
        if (runnable) {
            unsigned limit = fixed ? 8 : 1;
            for (unsigned i = 0; i < limit; i++) {
                unsigned char byte;
                ssize_t got = read(data[1], &byte, 1);
                if (got < 0) {
                    suite_require(errno == EAGAIN, "edge drained");
                    runnable = false;
                    break;
                }
                suite_require(got == 1 && byte == 1, "edge payload");
                r->checksum += byte;
                r->completed++;
            }
            if (!fixed)
                runnable = false;
        }
        if (!n && !runnable)
            break;
    }
    int remaining;
    suite_require(!ioctl(data[1], FIONREAD, &remaining), "edge remaining bytes");
    suite_event("edge_fionread", (unsigned)remaining);
    unsigned char pending[128];
    ssize_t readable = recv(data[1], pending, sizeof(pending), MSG_PEEK | MSG_DONTWAIT);
    suite_require(readable >= 0 || errno == EAGAIN, "edge actual unread bytes");
    r->observed = readable < 0 ? 0U : (uint64_t)readable;
    close(ep);
    for (unsigned i = 0; i < 2; i++) {
        close(data[i]);
        close(control[i]);
    }
}
static void pmtu(bool fixed, struct sample *r)
{
    int fd = peer_connection(SOCK_DGRAM, SUITE_PEER_ADDRESS, SUITE_DATA_PORT);
    int yes = 1, discover = IP_PMTUDISC_DO;
    suite_require(!setsockopt(fd, IPPROTO_IP, IP_RECVERR, &yes, sizeof(yes)) &&
                      !setsockopt(fd, IPPROTO_IP, IP_MTU_DISCOVER, &discover, sizeof(discover)),
                  "DF and error queue");
    struct suite_datagram packet = {.kind = htonl(2), .offset = 0, .length = htonl(1400)};
    for (unsigned i = 0; i < 1400; i++)
        packet.data[i] = (unsigned char)(i & 255);
    suite_require(send(fd, &packet, 1412, 0) == 1412, "initial large DF probe");
    lab_sleep_us(20000);
    unsigned char payload[16], control[512];
    struct iovec iov = {payload, sizeof(payload)};
    struct msghdr message = {.msg_iov = &iov,
                             .msg_iovlen = 1,
                             .msg_control = control,
                             .msg_controllen = sizeof(control)};
    ssize_t error = recvmsg(fd, &message, MSG_ERRQUEUE | MSG_DONTWAIT);
    if (error >= 0) {
        for (struct cmsghdr *c = CMSG_FIRSTHDR(&message); c; c = CMSG_NXTHDR(&message, c))
            if (c->cmsg_level == IPPROTO_IP && c->cmsg_type == IP_RECVERR) {
                struct sock_extended_err info;
                memcpy(&info, CMSG_DATA(c), sizeof(info));
                r->observed = info.ee_errno;
                suite_event("icmp_reported_mtu", info.ee_info);
            }
    } else
        suite_require(errno == EAGAIN, "no ICMP feedback");
    int mtu;
    socklen_t size = sizeof(mtu);
    suite_require(!getsockopt(fd, IPPROTO_IP, IP_MTU, &mtu, &size), "cached path MTU");
    r->auxiliary = (unsigned)mtu;
    packet.kind = htonl(1);
    packet.offset = 0;
    packet.length = htonl(64);
    memset(packet.data, 42, 64);
    suite_require(send(fd, &packet, 76, 0) == 76, "small path probe");
    struct suite_ack ack;
    suite_require(recv(fd, &ack, sizeof(ack), 0) == sizeof(ack) && ntohl(ack.kind) == 1,
                  "small probe response");
    r->before = ntohl(ack.cpu);
    if (fixed)
        for (unsigned offset = 0; offset < 1400;) {
            unsigned n = 1400 - offset < 512 ? 1400 - offset : 512;
            packet.kind = htonl(2);
            packet.offset = htonl(offset);
            packet.length = htonl(n);
            for (unsigned i = 0; i < n; i++)
                packet.data[i] = (unsigned char)((offset + i) & 255);
            suite_require(send(fd, &packet, 12 + n, 0) == (ssize_t)(12 + n), "PMTU-sized retry");
            suite_require(recv(fd, &ack, sizeof(ack), 0) == sizeof(ack) && ntohl(ack.kind) == 2 &&
                              ntohl(ack.total) == offset + n,
                          "PMTU retry acknowledged");
            r->after++;
            offset += n;
        }
    packet.kind = htonl(4);
    packet.length = 0;
    suite_require(send(fd, &packet, 12, 0) == 12 && recv(fd, &ack, sizeof(ack), 0) == sizeof(ack) &&
                      ntohl(ack.kind) == 4,
                  "PMTU peer completion");
    r->completed = ntohl(ack.total);
    r->checksum = ntohl(ack.checksum);
    close(fd);
}
static void rps(struct sample *r)
{
    suite_pin(2);
    int fd = peer_connection(SOCK_STREAM, SUITE_RPS_ADDRESS, SUITE_DATA_PORT);
    int yes = 1;
    suite_require(!setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)), "RPS small writes");
    unsigned char data[64];
    memset(data, 42, sizeof(data));
    for (unsigned i = 0; i < 4096; i++) {
        size_t sent = 0;
        while (sent < sizeof(data)) {
            ssize_t n = send(fd, data + sent, sizeof(data) - sent, MSG_NOSIGNAL);
            suite_require(n > 0, "RPS transmission");
            sent += (size_t)n;
        }
    }
    suite_require(!shutdown(fd, SHUT_WR), "RPS end stream");
    struct suite_ack ack;
    suite_require(recv(fd, &ack, sizeof(ack), MSG_WAITALL) == sizeof(ack) && ntohl(ack.kind) == 3,
                  "RPS receiver evidence");
    r->completed = ntohl(ack.total);
    r->checksum = ntohl(ack.checksum);
    r->observed = ntohl(ack.cpu);
    int local = -1;
    socklen_t size = sizeof(local);
    suite_require(!getsockopt(fd, SOL_SOCKET, SO_INCOMING_CPU, &local, &size),
                  "local incoming CPU");
    r->auxiliary = local >= 0 ? (unsigned)local : UINT32_MAX;
    close(fd);
}
static void priority_network(struct sample *r)
{
    int data = peer_connection(SOCK_DGRAM, SUITE_RPS_ADDRESS, SUITE_DATA_PORT),
        control = peer_connection(SOCK_DGRAM, SUITE_RPS_ADDRESS, SUITE_CONTROL_PORT);
    unsigned char packet[512];
    memset(packet, 42, sizeof(packet));
    packet[0] = 'B';
    for (unsigned i = 0; i < 64; i++) {
        packet[1] = (unsigned char)i;
        suite_require(send(data, packet, 512, 0) == 512, "bulk queue");
    }
    uint64_t maximum = 0;
    for (unsigned i = 0; i < 16; i++) {
        packet[0] = 'C';
        packet[1] = (unsigned char)i;
        uint64_t start = lab_clock(CLOCK_MONOTONIC);
        suite_require(send(control, packet, 64, 0) == 64, "control request");
        struct suite_ack ack;
        suite_require(recv(control, &ack, sizeof(ack), 0) == sizeof(ack) && ntohl(ack.kind) == 1 &&
                          ntohl(ack.total) == i,
                      "control response order");
        uint64_t late = lab_clock(CLOCK_MONOTONIC) - start;
        if (late > maximum)
            maximum = late;
        r->before++;
        suite_event("control_roundtrip_ns", late);
    }
    struct suite_ack done;
    suite_require(recv(control, &done, sizeof(done), 0) == sizeof(done) && ntohl(done.kind) == 4,
                  "all bulk and control completed");
    r->completed = ntohl(done.total);
    r->checksum = ntohl(done.checksum);
    r->auxiliary = ntohl(done.cpu);
    r->observed = maximum;
    close(data);
    close(control);
}
struct restore_server {
    int listener;
};
static void *server_restore(void *argument)
{
    struct restore_server *p = argument;
    lab_sleep_us(100000);
    suite_require(!listen(p->listener, 1), "restored listener");
    struct pollfd item = {.fd = p->listener, .events = POLLIN};
    int n = poll(&item, 1, 300);
    suite_require(n >= 0, "restoration poll");
    if (n) {
        int fd = accept(p->listener, NULL, NULL);
        suite_require(fd >= 0, "restored client");
        suite_require(send(fd, "*", 1, MSG_NOSIGNAL) == 1, "restored service reply");
        close(fd);
    }
    return NULL;
}
static void reconnect(bool fixed, struct sample *r)
{
    r->before = suite_fds();
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    suite_require(listener >= 0, "restoration socket");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    suite_require(!bind(listener, (struct sockaddr *)&address, size) &&
                      !getsockname(listener, (struct sockaddr *)&address, &size),
                  "reserved recovery endpoint");
    struct restore_server server = {listener};
    pthread_t thread;
    suite_thread(pthread_create(&thread, NULL, server_restore, &server));
    uint32_t seed = 7;
    unsigned delay = 5000;
    for (unsigned attempt = 0; attempt < 20; attempt++) {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        suite_require(fd >= 0, "retry socket");
        r->observed++;
        if (!connect(fd, (struct sockaddr *)&address, size)) {
            unsigned char byte;
            suite_require(recv(fd, &byte, 1, 0) == 1, "restored reply");
            r->checksum = byte;
            r->completed = 1;
            close(fd);
            r->auxiliary = 0;
            break;
        }
        r->auxiliary = (unsigned)errno;
        suite_event("retry_errno", r->auxiliary);
        close(fd);
        if (fixed) {
            seed = seed * 1664525U + 1013904223U;
            lab_sleep_us(delay + seed % 5000);
            if (delay < 80000)
                delay *= 2;
        }
    }
    suite_thread(pthread_join(thread, NULL));
    close(listener);
    r->after = suite_fds();
}
void suite_adv_net(bool fixed, struct sample *r, unsigned kind)
{
    if (kind == 0)
        batch_udp(fixed, r);
    else if (kind == 1) {
        int pair[2];
        suite_tcp_pair(pair);
        timeout_socket(pair[0]);
        timeout_socket(pair[1]);
        int size = 4096;
        suite_require(!setsockopt(pair[0], SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) &&
                          !setsockopt(pair[1], SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)),
                      "small transport windows");
        struct slow_peer p = {.lock = PTHREAD_MUTEX_INITIALIZER,
                              .ready = PTHREAD_COND_INITIALIZER,
                              .space = PTHREAD_COND_INITIALIZER,
                              .limit = fixed ? 8 : 64,
                              .tx = pair[0],
                              .rx = pair[1]};
        pthread_t sender, receiver;
        suite_thread(pthread_create(&receiver, NULL, slow_receive, &p));
        suite_thread(pthread_create(&sender, NULL, queued_send, &p));
        for (unsigned i = 0; i < 64; i++) {
            suite_thread(pthread_mutex_lock(&p.lock));
            while (p.count == p.limit)
                suite_thread(pthread_cond_wait(&p.space, &p.lock));
            struct queued_message *message = malloc(sizeof(*message));
            suite_require(message != NULL, "bounded outgoing message");
            message->next = NULL;
            message->sequence = i;
            memset(message->bytes, (int)i, sizeof(message->bytes));
            if (p.tail)
                p.tail->next = message;
            else
                p.head = message;
            p.tail = message;
            p.count++;
            if (p.count > p.peak)
                p.peak = p.count;
            suite_thread(pthread_cond_signal(&p.ready));
            suite_thread(pthread_mutex_unlock(&p.lock));
        }
        suite_thread(pthread_join(sender, NULL));
        suite_thread(pthread_join(receiver, NULL));
        r->completed = p.finished;
        r->checksum = p.sum;
        r->observed = p.peak;
        r->auxiliary = p.send_queue;
        r->after = p.count;
        close(pair[0]);
        close(pair[1]);
        pthread_mutex_destroy(&p.lock);
        pthread_cond_destroy(&p.ready);
        pthread_cond_destroy(&p.space);
    } else if (kind == 2)
        edge_budget(fixed, r);
    else if (kind == 3)
        pmtu(fixed, r);
    else if (kind == 4)
        rps(r);
    else if (kind == 5)
        priority_network(r);
    else
        reconnect(fixed, r);
}
