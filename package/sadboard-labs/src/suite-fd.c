#include "suite.h"
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

void suite_tcp_pair(int pair[2])
{
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    suite_require(listener >= 0, "listener");
    struct sockaddr_in address = {.sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK)};
    socklen_t size = sizeof(address);
    suite_require(!bind(listener, (struct sockaddr *)&address, size) && !listen(listener, 1) &&
                      !getsockname(listener, (struct sockaddr *)&address, &size),
                  "loopback listener");
    pair[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    suite_require(pair[0] >= 0 && !connect(pair[0], (struct sockaddr *)&address, size),
                  "loopback connect");
    pair[1] = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    suite_require(pair[1] >= 0, "accept");
    close(listener);
}
void suite_fd(bool fixed, struct sample *r, unsigned kind)
{
    r->before = suite_fds();
    if (kind == 1) {
        int p[2], ready[2], release[2];
        suite_require(!pipe2(p, fixed ? O_CLOEXEC : 0) && !pipe(ready) && !pipe(release), "pipe2");
        pid_t child = fork();
        suite_require(child >= 0, "fork helper");
        if (!child) {
            close(p[0]);
            close(ready[0]);
            close(release[1]);
            char a[16], b[16];
            snprintf(a, sizeof(a), "%d", ready[1]);
            snprintf(b, sizeof(b), "%d", release[0]);
            execl(suite_executable, suite_executable, "--hold-writer", a, b, (char *)NULL);
            _exit(70);
        }
        close(p[1]);
        close(ready[1]);
        close(release[0]);
        char handshake;
        suite_require(read(ready[0], &handshake, 1) == 1 && handshake == 42, "helper reached exec");
        close(ready[0]);
        struct pollfd poller = {.fd = p[0], .events = POLLIN};
        int poll_ready = poll(&poller, 1, 100);
        suite_require(poll_ready >= 0, "pipe poll");
        r->observed = poll_ready == 0;
        suite_event("inherited_writer_blocks_eof", r->observed);
        handshake = 42;
        suite_require(write(release[1], &handshake, 1) == 1, "release helper");
        close(release[1]);
        bool timed;
        int status = suite_wait(child, 1500, &timed);
        suite_require(!timed && WIFEXITED(status) && !WEXITSTATUS(status), "helper exit");
        char byte;
        r->checksum = read(p[0], &byte, 1) == 0;
        close(p[0]);
        r->completed = 1;
    } else {
        int retained[16], ep = -1;
        if (kind == 3) {
            ep = epoll_create1(EPOLL_CLOEXEC);
            suite_require(ep >= 0, "epoll create");
        }
        unsigned saved = 0;
        for (unsigned i = 0; i < 16; i++) {
            int fd;
            if (kind == 0) {
                int original = open("/dev/null", O_RDONLY | O_CLOEXEC);
                suite_require(original >= 0, "open null");
                fd = dup(original);
                close(original);
                suite_require(fd >= 0, "dup");
            } else if (kind == 2) {
                int pair[2];
                suite_tcp_pair(pair);
                unsigned char byte = (unsigned char)i;
                suite_require(send(pair[0], &byte, 1, MSG_NOSIGNAL) == 1, "peer byte");
                close(pair[0]);
                unsigned char got;
                suite_require(recv(pair[1], &got, 1, MSG_WAITALL) == 1 && got == byte,
                              "peer byte received");
                suite_require(!recv(pair[1], &got, 1, 0), "peer EOF");
                struct tcp_info info;
                socklen_t length = sizeof(info);
                suite_require(!getsockopt(pair[1], IPPROTO_TCP, TCP_INFO, &info, &length),
                              "TCP info");
                suite_event("tcp_state_after_eof", info.tcpi_state);
                if (info.tcpi_state == TCP_CLOSE_WAIT)
                    r->auxiliary++;
                fd = pair[1];
            } else {
                int pair[2];
                suite_require(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, pair),
                              "socketpair");
                struct epoll_event event = {.events = EPOLLIN, .data.fd = pair[0]};
                suite_require(!epoll_ctl(ep, EPOLL_CTL_ADD, pair[0], &event) &&
                                  !epoll_ctl(ep, EPOLL_CTL_DEL, pair[0], NULL),
                              "epoll registration lifecycle");
                close(pair[1]);
                fd = pair[0];
            }
            if (fixed)
                close(fd);
            else
                retained[saved++] = fd;
            r->completed++;
            r->checksum += i;
            suite_event("open_descriptors", suite_fds());
        }
        r->observed = suite_fds() - r->before - (ep >= 0 ? 1 : 0);
        for (unsigned i = 0; i < saved; i++)
            close(retained[i]);
        if (ep >= 0)
            close(ep);
    }
    r->after = suite_fds();
}
