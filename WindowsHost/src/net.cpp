#include "host.h"

#include <limits.h>

extern "C" uint64_t now_nanos(void) {
    static LARGE_INTEGER freq;
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    // Split to avoid overflowing 64 bits at high counter frequencies.
    uint64_t sec = t.QuadPart / freq.QuadPart, rem = t.QuadPart % freq.QuadPart;
    return sec * 1000000000ull + rem * 1000000000ull / freq.QuadPart;
}

bool net_read_full(SOCKET fd, void *buf, size_t len) {
    char *p = (char *)buf;
    while (len) {
        int n = recv(fd, p, len > INT_MAX ? INT_MAX : (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}

bool net_write_full(SOCKET fd, const void *buf, size_t len) {
    const char *p = (const char *)buf;
    while (len) {
        int n = send(fd, p, len > INT_MAX ? INT_MAX : (int)len, 0);
        if (n <= 0) return false;
        p += n;
        len -= n;
    }
    return true;
}
