#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "iframe.h"

// avahi-browse escapes service names: "\032" is a space, "\." a dot.
static void unescape(const char *in, char *out, size_t cap) {
    size_t n = 0;
    while (*in && n + 1 < cap) {
        if (in[0] == '\\' && in[1] >= '0' && in[1] <= '9' && in[2] && in[3]) {
            out[n++] = (char)((in[1] - '0') * 100 + (in[2] - '0') * 10 + (in[3] - '0'));
            in += 4;
        } else if (in[0] == '\\' && in[1]) {
            out[n++] = in[1];
            in += 2;
        } else {
            out[n++] = *in++;
        }
    }
    out[n] = 0;
}

/// Finds iframe-host instances (_iframe._tcp) on the LAN. Prefers IPv4 addresses.
int discover_hosts(HostEntry *out, int max, int timeout_seconds) {
    char cmd[128];
    snprintf(cmd, sizeof cmd, "timeout %d avahi-browse -rpt _iframe._tcp 2>/dev/null", timeout_seconds);
    FILE *f = popen(cmd, "r");
    if (!f) return 0;
    int count = 0;
    char *line = NULL;
    size_t cap = 0;
    // Resolved lines: =;iface;proto;name;type;domain;hostname;address;port;txt
    while (getline(&line, &cap, f) > 0) {
        if (line[0] != '=') continue;
        char *fields[10] = {0};
        int nf = 0;
        for (char *p = line, *tok; nf < 10 && (tok = strsep(&p, ";")); ) fields[nf++] = tok;
        if (nf < 9) continue;
        bool ipv4 = strcmp(fields[2], "IPv4") == 0;
        // Containers and VMs bridges show up too when the host is this machine; skip them.
        const char *iface = fields[1];
        if (!strncmp(iface, "docker", 6) || !strncmp(iface, "br-", 3) || !strncmp(iface, "virbr", 5) ||
            !strncmp(iface, "veth", 4))
            continue;
        char name[128];
        unescape(fields[3], name, sizeof name);
        int i;
        for (i = 0; i < count; i++)
            if (strcmp(out[i].name, name) == 0) break;
        if (i < count) {
            // Already have it; upgrade an IPv6 entry to IPv4 (link-local v6 needs a scope id).
            if (ipv4 && strchr(out[i].address, ':')) snprintf(out[i].address, sizeof out[i].address, "%s", fields[7]);
            continue;
        }
        if (count >= max) continue;
        snprintf(out[count].name, sizeof out[count].name, "%s", name);
        snprintf(out[count].address, sizeof out[count].address, "%s", fields[7]);
        out[count].port = atoi(fields[8]);
        count++;
    }
    free(line);
    pclose(f);
    return count;
}
