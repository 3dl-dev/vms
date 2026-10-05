/* fake_getifaddrs.c - LD_PRELOAD shim for test_decnetd_startnet_gate.sh (vms-94b).
 *
 * DECNETD.EXE auto-detects its datalink NIC with getifaddrs(). Asserting that
 * against the REAL host is flaky by construction: the host's interface set is
 * whatever the CI runner has at that instant (docker veths appear and vanish
 * under parallel jobs, a sandbox may have only lo). This shim replaces the
 * enumeration with a list given in $FAKE_IFS -- "name:flags,name:flags,..." with
 * flags in {lo, up, down} -- so the selection logic is tested deterministically.
 * Every entry is an AF_PACKET (link-layer) record, as getifaddrs reports. */
#define _GNU_SOURCE
#include <ifaddrs.h>
#include <net/if.h>
#include <netpacket/packet.h>
#include <stdlib.h>
#include <string.h>

int getifaddrs(struct ifaddrs **out)
{
    const char *spec = getenv("FAKE_IFS");
    char *dup = strdup(spec ? spec : "");
    struct ifaddrs *head = NULL, **tail = &head;
    char *save = NULL;
    for (char *tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *colon = strchr(tok, ':');
        if (!colon)
            continue;
        *colon = 0;
        struct ifaddrs *e = calloc(1, sizeof(*e));
        struct sockaddr_ll *ll = calloc(1, sizeof(*ll));
        ll->sll_family = AF_PACKET;
        e->ifa_name = strdup(tok);
        e->ifa_addr = (struct sockaddr *)ll;
        if (!strcmp(colon + 1, "lo"))
            e->ifa_flags = IFF_LOOPBACK | IFF_UP;
        else if (!strcmp(colon + 1, "up"))
            e->ifa_flags = IFF_UP;
        *tail = e;
        tail = &e->ifa_next;
    }
    free(dup);
    *out = head;
    return 0;
}

void freeifaddrs(struct ifaddrs *p)
{
    while (p) {
        struct ifaddrs *n = p->ifa_next;
        free((void *)p->ifa_name);
        free(p->ifa_addr);
        free(p);
        p = n;
    }
}
