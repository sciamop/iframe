#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "net.h"

uint64_t now_nanos(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

/// TCP tuned like MessageChannel.parameters(): no Nagle, fast dead-peer detection, and the
/// video traffic class so Wi-Fi puts frames in the WMM video queue.
static void tune(int fd, int family) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    int idle = 5, interval = 2, count = 3;
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof idle);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &interval, sizeof interval);
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &count, sizeof count);
    int tos = 0x80;  // DSCP CS4 (real-time interactive) -> WMM AC_VI
    if (family == AF_INET6) setsockopt(fd, IPPROTO_IPV6, IPV6_TCLASS, &tos, sizeof tos);
    else setsockopt(fd, IPPROTO_IP, IP_TOS, &tos, sizeof tos);
    int priority = 5;
    setsockopt(fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof priority);
    int rcvbuf = 4 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof rcvbuf);
}

int net_connect(const char *host, int port, int timeout_ms, char *err, size_t err_cap) {
    char service[16];
    snprintf(service, sizeof service, "%d", port);
    struct addrinfo hints = { .ai_socktype = SOCK_STREAM, .ai_family = AF_UNSPEC }, *res = NULL;
    int rc = getaddrinfo(host, service, &hints, &res);
    if (rc != 0) {
        snprintf(err, err_cap, "can't resolve %s: %s", host, gai_strerror(rc));
        return -1;
    }
    int fd = -1;
    snprintf(err, err_cap, "can't reach %s:%d", host, port);
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0) continue;
        tune(fd, ai->ai_family);
        int flags = fcntl(fd, F_GETFL);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int ok = connect(fd, ai->ai_addr, ai->ai_addrlen) == 0;
        if (!ok && errno == EINPROGRESS) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };
            if (poll(&pfd, 1, timeout_ms) == 1) {
                int so_error = 0;
                socklen_t len = sizeof so_error;
                getsockopt(fd, SOL_SOCKET, SO_ERROR, &so_error, &len);
                ok = so_error == 0;
                if (!ok) snprintf(err, err_cap, "can't reach %s:%d: %s", host, port, strerror(so_error));
            } else {
                snprintf(err, err_cap, "can't reach %s:%d: timed out", host, port);
            }
        }
        if (ok) {
            fcntl(fd, F_SETFL, flags);
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

bool net_read_full(int fd, void *buf, size_t len) {
    uint8_t *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n == 0) errno = 0;  // the peer closed cleanly; callers tell that apart from errors
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool net_write_full(int fd, const void *buf, size_t len) {
    const uint8_t *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}
