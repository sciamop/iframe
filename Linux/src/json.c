#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iframe.h"

// The host's messages are flat objects written by Swift's JSONEncoder, so finding
// `"key":` and reading the value after it is enough.
static const char *find_value(const char *json, size_t len, const char *key) {
    char pattern[64];
    int n = snprintf(pattern, sizeof pattern, "\"%s\"", key);
    if (n <= 0 || (size_t)n >= sizeof pattern) return NULL;
    const char *end = json + len;
    for (const char *p = json; p + n <= end; p++) {
        if (memcmp(p, pattern, n) != 0) continue;
        const char *q = p + n;
        while (q < end && isspace((unsigned char)*q)) q++;
        if (q >= end || *q != ':') continue;
        q++;
        while (q < end && isspace((unsigned char)*q)) q++;
        return q < end ? q : NULL;
    }
    return NULL;
}

bool json_number(const char *json, size_t len, const char *key, double *out) {
    const char *v = find_value(json, len, key);
    if (!v) return false;
    char buf[64];
    size_t n = 0;
    while (v + n < json + len && n < sizeof buf - 1 && strchr("+-.0123456789eE", v[n])) n++;
    if (n == 0) return false;
    memcpy(buf, v, n);
    buf[n] = 0;
    *out = strtod(buf, NULL);
    return true;
}

bool json_bool(const char *json, size_t len, const char *key, bool *out) {
    const char *v = find_value(json, len, key);
    if (!v) return false;
    size_t left = json + len - v;
    if (left >= 4 && memcmp(v, "true", 4) == 0) { *out = true; return true; }
    if (left >= 5 && memcmp(v, "false", 5) == 0) { *out = false; return true; }
    return false;
}

bool json_string(const char *json, size_t len, const char *key, char *out, size_t cap) {
    const char *v = find_value(json, len, key);
    const char *end = json + len;
    if (!v || *v != '"' || cap == 0) return false;
    size_t n = 0;
    for (const char *p = v + 1; p < end && *p != '"'; p++) {
        char c = *p;
        if (c == '\\' && p + 1 < end) {
            c = *++p;
            if (c == 'n') c = '\n';
            else if (c == 't') c = '\t';
            else if (c == 'u') { p += 4; c = '?'; }  // host names are ASCII in practice
        }
        if (n + 1 < cap) out[n++] = c;
    }
    out[n] = 0;
    return true;
}

void json_escape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    for (; *in && n + 7 < cap; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') {
            out[n++] = '\\';
            out[n++] = c;
        } else if (c < 0x20) {
            n += snprintf(out + n, cap - n, "\\u%04x", c);
        } else {
            out[n++] = c;
        }
    }
    out[n] = 0;
}
