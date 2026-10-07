/* fake_getifaddrs.c - LD_PRELOAD shim for test_decnetd_startnet_gate.sh (vms-94b).
 *
 * DECNETD.EXE auto-detects its datalink NIC with scs_datalink_primary_iface()
 * (if_nameindex() + SIOCGIFFLAGS/SIOCGIFHWADDR, src/libdatalink). Asserting
 * that against the REAL host is flaky by construction: the interface set is
 * whatever the CI runner has at that instant (docker veths appear and vanish
 * under parallel jobs, a sandbox may have only lo). This shim replaces the
 * enumeration with the list in $FAKE_IFS -- "name:kind,..." with kind in
 * {lo, eth, other} -- answering if_nameindex() and the two ioctls for those
 * names, so the SELECTION logic is tested deterministically. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>

static const char *kind_of(const char *name)
{
    static char buf[512];
    const char *spec = getenv("FAKE_IFS");
    if (!spec)
        return NULL;
    snprintf(buf, sizeof(buf), "%s", spec);
    char *save = NULL;
    for (char *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *c = strchr(tok, ':');
        if (c && (size_t)(c - tok) == strlen(name) && !strncmp(tok, name, c - tok))
            return c + 1;
    }
    return NULL;
}

struct if_nameindex *if_nameindex(void)
{
    const char *spec = getenv("FAKE_IFS");
    char *dup = strdup(spec ? spec : "");
    size_t cap = 1;
    for (char *p = dup; *p; p++)
        if (*p == ',')
            cap++;
    struct if_nameindex *l = calloc(cap + 1, sizeof(*l));
    size_t n = 0;
    char *save = NULL;
    for (char *tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char *c = strchr(tok, ':');
        if (!c)
            continue;
        *c = 0;
        l[n].if_index = (unsigned)(n + 1);
        l[n].if_name = strdup(tok);
        n++;
    }
    free(dup);
    return l;
}

void if_freenameindex(struct if_nameindex *l)
{
    for (struct if_nameindex *p = l; p && p->if_name; p++)
        free(p->if_name);
    free(l);
}

int ioctl(int fd, unsigned long req, ...)
{
    va_list ap;
    va_start(ap, req);
    void *arg = va_arg(ap, void *);
    va_end(ap);
    if (req == SIOCGIFFLAGS || req == SIOCGIFHWADDR) {
        struct ifreq *r = arg;
        const char *k = kind_of(r->ifr_name);
        if (!k)
            return -1;
        if (req == SIOCGIFFLAGS)
            r->ifr_flags = !strcmp(k, "lo") ? (IFF_LOOPBACK | IFF_UP) : 0;
        else
            r->ifr_hwaddr.sa_family = !strcmp(k, "other") ? ARPHRD_NONE : ARPHRD_ETHER;
        return 0;
    }
    int (*real)(int, unsigned long, ...) = dlsym(RTLD_NEXT, "ioctl");
    return real(fd, req, arg);
}
