#pragma once

#include "iframe.h"

// Returns a connected, tuned TCP socket or -1 (with a message in err).
int net_connect(const char *host, int port, int timeout_ms, char *err, size_t err_cap);
bool net_read_full(int fd, void *buf, size_t len);
bool net_write_full(int fd, const void *buf, size_t len);
