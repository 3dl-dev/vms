/*
 * scs_datalink.c - vms-838: raw-L2 datalink backends (Linux AF_PACKET,
 * NetBSD bpf(4)). See scs_datalink.h for the surface and the rationale.
 *
 * WHAT IS PROVEN vs. WHAT IS A DESIGN CLAIM. The Linux backend is the
 * pre-vms-838 scsd.c behavior, moved here verbatim -- proven the same way
 * it always was (the SCS cluster lab, tests/vmsscs/test_scsd_send_sites.py,
 * tests/vmsscs/test_scsd_wire.c). The NetBSD backend closes SCSD.EXE's
 * `ovmx-images` build gap (it compiles + links a real elf32-vax dynamic
 * executable, README: tools/cross-vax/build-ovmx-images-vax-cmake.sh) but
 * its bpf send/recv semantics are NOT yet proven against a live interface
 * under SIMH -- that needs a running NetBSD/vax instance, which is a
 * follow-up (see the VAX session's SIMH lab, not lab-2/lab-Alpha -- neither
 * carries NetBSD-vax). Treat the NetBSD backend as "builds and is bpf(4)-
 * man-page-faithful," not "wire-proven," until that follow-up lands.
 */
#include "scs_datalink.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__)

#include <arpa/inet.h>
#include <net/if.h>
#include <net/if_arp.h>  /* ARPHRD_ETHER -- scs_datalink_primary_iface() (vms-5ad) */
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>

/* ============================================================================
 * TWO Linux backends, selected at COMPILE time (vms-7eb piece 2):
 *
 *   SCS_DATALINK_VIA_EXECUTIVE defined  -> the EXECUTIVE backend, and ONLY it.
 *       The booted OVMX runtime builds this. The raw 0x6007 wire is carried by
 *       a KERNEL AF_PACKET socket the executive owns (src/kernel-core/vms_l2.c,
 *       exec_l2_*), reached by direct /dev/vms VMS_IOCTL_L2_* ioctls. A booted,
 *       non-root VMS process therefore needs ZERO Linux capabilities -- the
 *       executive does the privileged I/O, gated on the VMS PHY_IO privilege.
 *       The direct-AF_PACKET code is #ifdef'd OUT of this build, so a booted
 *       SCSD.EXE is PHYSICALLY INCAPABLE of opening a raw socket itself (the
 *       anti-fabrication separation the milestone gate depends on).
 *
 *   SCS_DATALINK_VIA_EXECUTIVE undefined -> the AF_PACKET PROBE backend, a
 *       clean-room RE / oracle instrument ONLY (see its banner below).
 *
 * The MAC / interface lookups are SHARED: they use SOCK_DGRAM + getifaddrs,
 * which need no CAP_NET_RAW, so a non-root process runs them under either
 * backend. Only the raw-L2 open/send/recv/close/timeout differ.
 * ==========================================================================*/

/* ---- SHARED non-privileged interface lookups (both backends) ------------- */

int scs_datalink_get_hwaddr(const char *ifname, uint8_t mac_out[6])
{
    int s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s < 0) {
        return -1;
    }
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }
    memcpy(mac_out, ifr.ifr_hwaddr.sa_data, 6);
    close(s);
    return 0;
}

int scs_datalink_primary_iface(char *out, size_t n)
{
    struct if_nameindex *list = if_nameindex();
    if (list == NULL) {
        return -1;
    }

    int found = -1;
    for (struct if_nameindex *p = list; p->if_name != NULL; p++) {
        int s = socket(AF_INET, SOCK_DGRAM, 0);
        if (s < 0) {
            continue;
        }

        struct ifreq ifr;
        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, p->if_name, IFNAMSIZ - 1);
        if (ioctl(s, SIOCGIFFLAGS, &ifr) < 0 || (ifr.ifr_flags & IFF_LOOPBACK)) {
            close(s);
            continue; /* vms-9d2 twin: skip loopback exactly as
                       * for_each_netdev's IFF_LOOPBACK check does */
        }

        memset(&ifr, 0, sizeof(ifr));
        strncpy(ifr.ifr_name, p->if_name, IFNAMSIZ - 1);
        if (ioctl(s, SIOCGIFHWADDR, &ifr) < 0) {
            close(s);
            continue;
        }
        close(s);
        if (ifr.ifr_hwaddr.sa_family != ARPHRD_ETHER) {
            continue; /* require Ethernet, exactly as the executive does */
        }

        if (out != NULL && n > 0) {
            strncpy(out, p->if_name, n - 1);
            out[n - 1] = '\0';
        }
        found = 0;
        break;
    }

    if_freenameindex(list);
    if (found != 0) {
        errno = ENODEV;
    }
    return found;
}

#if defined(SCS_DATALINK_VIA_EXECUTIVE)

/* ---- EXECUTIVE backend: the raw L2 wire routes through /dev/vms ---------- */

#include "vms_ioctl.h"   /* VMS_IOCTL_L2_* + struct vms_l2_*_args + VMS_IOCTL_REGISTER */
#include <fcntl.h>

/* SCSD opens exactly one datalink for its whole run; a small fd->handle table
 * (sized like the NetBSD bpf table) keeps the executive handle + recv timeout
 * the /dev/vms fd alone cannot carry. */
#define SCS_DATALINK_MAX_OPEN 4
struct l2_slot { int in_use; int fd; uint32_t handle; uint32_t timeout_ms; };
static struct l2_slot g_l2[SCS_DATALINK_MAX_OPEN];

static struct l2_slot *l2_find(int fd)
{
    for (int i = 0; i < SCS_DATALINK_MAX_OPEN; i++)
        if (g_l2[i].in_use && g_l2[i].fd == fd) return &g_l2[i];
    return NULL;
}
static struct l2_slot *l2_alloc(int fd, uint32_t handle)
{
    for (int i = 0; i < SCS_DATALINK_MAX_OPEN; i++)
        if (!g_l2[i].in_use) {
            g_l2[i].in_use = 1; g_l2[i].fd = fd;
            g_l2[i].handle = handle; g_l2[i].timeout_ms = 0;
            return &g_l2[i];
        }
    return NULL;
}

int scs_datalink_open(const char *ifname, uint16_t ethertype)
{
    int fd = open("/dev/vms", O_RDWR);
    if (fd < 0)
        return -1;                     /* no executive -> honest failure, never
                                        * a silent AF_PACKET fallback: that code
                                        * is not compiled into this binary. */
    /* The executive resolves the caller's process (vms_proc_find_or_err) from
     * its registration, then gates L2_OPEN on PHY_IO -- register first, exactly
     * as scsd's DLM path does (scsd.c). */
    struct vms_register_args reg;
    memset(&reg, 0, sizeof(reg));
    if (ioctl(fd, VMS_IOCTL_REGISTER, &reg) < 0) { close(fd); return -1; }

    struct vms_l2_open_args a;
    memset(&a, 0, sizeof(a));
    strncpy(a.ifname, ifname, sizeof(a.ifname) - 1);
    a.ethertype = ethertype;
    if (ioctl(fd, VMS_IOCTL_L2_OPEN, &a) < 0) { close(fd); return -1; }
    if (a.status != 1u) {          /* SS$_NORMAL == 1; anything else is honest
                                    * refusal (SS$_NOPRIV without PHY_IO,
                                    * SS$_NOSUCHDEV 2680 for an absent iface). */
        close(fd);
        errno = (a.status == 2680u) ? ENODEV : EACCES;
        return -1;
    }
    if (l2_alloc(fd, a.handle) == NULL) { close(fd); errno = ENOMEM; return -1; }
    return fd;
}

void scs_datalink_close(int fd)
{
    struct l2_slot *s = l2_find(fd);
    if (s != NULL) {
        struct vms_l2_close_args a;
        memset(&a, 0, sizeof(a));
        a.handle = s->handle;
        (void)ioctl(fd, VMS_IOCTL_L2_CLOSE, &a);
        memset(s, 0, sizeof(*s));
    }
    close(fd);
}

ssize_t scs_datalink_send(int fd, int ifindex, uint16_t ethertype,
                          const uint8_t dst_mac[6],
                          const uint8_t *frame, size_t len)
{
    struct l2_slot *s = l2_find(fd);
    if (s == NULL) { errno = EBADF; return -1; }
    if (len > VMS_L2_MAXLEN) { errno = EMSGSIZE; return -1; }
    struct vms_l2_send_args a;
    memset(&a, 0, sizeof(a));
    a.handle = s->handle;
    a.ifindex = (uint32_t)ifindex;
    a.ethertype = ethertype;
    memcpy(a.dst_mac, dst_mac, 6);
    a.len = (uint32_t)len;
    memcpy(a.data, frame, len);
    if (ioctl(fd, VMS_IOCTL_L2_SEND, &a) < 0) return -1;
    if (a.status != 1u) { errno = EIO; return -1; }
    return (ssize_t)a.len;
}

ssize_t scs_datalink_recv(int fd, uint8_t *buf, size_t buf_len)
{
    struct l2_slot *s = l2_find(fd);
    if (s == NULL) { errno = EBADF; return -1; }
    struct vms_l2_recv_args a;
    memset(&a, 0, sizeof(a));
    a.handle = s->handle;
    a.timeout_ms = s->timeout_ms;
    if (ioctl(fd, VMS_IOCTL_L2_RECV, &a) < 0) return -1;
    if (a.status != 1u) { errno = EAGAIN; return -1; }  /* nothing before the
                                        * timeout -> the same "come back later"
                                        * the AF_PACKET recv() path signals. */
    size_t n = a.len;
    if (n > buf_len) n = buf_len;
    memcpy(buf, a.data, n);
    return (ssize_t)n;
}

int scs_datalink_set_recv_timeout(int fd, int seconds)
{
    struct l2_slot *s = l2_find(fd);
    if (s == NULL) { errno = EBADF; return -1; }
    s->timeout_ms = (uint32_t)seconds * 1000u;
    return 0;
}

#else  /* !SCS_DATALINK_VIA_EXECUTIVE -- the AF_PACKET PROBE backend */

/* ---- AF_PACKET PROBE backend: a clean-room RE / ORACLE INSTRUMENT ONLY ----
 * This opens a userspace AF_PACKET raw socket directly, which needs CAP_NET_RAW
 * (or the ambient capabilities a lab pod grants an exec'd process). It is the
 * kubectl-exec'd wire-mining tool that OBSERVES a real VAX's cluster wire -- it
 * is NOT "OVMX running", and a run of it NEVER counts as a booted OVMX joining
 * a cluster. The booted runtime compiles SCS_DATALINK_VIA_EXECUTIVE above
 * instead, and this raw-socket code is then absent from that binary entirely. */

#include <netpacket/packet.h>

int scs_datalink_open(const char *ifname, uint16_t ethertype)
{
    int s = socket(AF_PACKET, SOCK_RAW, htons(ethertype));
    if (s < 0) {
        return -1;
    }

    unsigned ifindex = if_nametoindex(ifname);
    if (ifindex == 0) {
        int e = errno ? errno : ENODEV;
        close(s);
        errno = e;
        return -1;
    }

    struct sockaddr_ll sll;
    memset(&sll, 0, sizeof(sll));
    sll.sll_family = AF_PACKET;
    sll.sll_protocol = htons(ethertype);
    sll.sll_ifindex = (int)ifindex;

    if (bind(s, (struct sockaddr *)&sll, sizeof(sll)) < 0) {
        int e = errno;
        close(s);
        errno = e;
        return -1;
    }

    return s;
}

void scs_datalink_close(int fd)
{
    close(fd);
}

/*
 * datalink_send_linux - THE Linux transport primitive. Kept as its own
 * named function (rather than inlined into scs_datalink_send() below) so a
 * text-based census over this file -- the same discipline
 * tests/vmsscs/test_scsd_send_sites.py applies to scsd.c -- can attribute
 * the sendto() to exactly one place per backend. See that script's vms-838
 * update for how it now covers this file too.
 */
static ssize_t datalink_send_linux(int sock, int ifindex, uint16_t ethertype,
                                   const uint8_t dst_mac[6],
                                   const uint8_t *frame, size_t len)
{
    struct sockaddr_ll da;
    memset(&da, 0, sizeof(da));
    da.sll_family = AF_PACKET;
    da.sll_protocol = htons(ethertype);
    da.sll_ifindex = ifindex;
    da.sll_halen = 6;
    memcpy(da.sll_addr, dst_mac, 6);
    return sendto(sock, frame, len, 0, (struct sockaddr *)&da, sizeof(da));
}

ssize_t scs_datalink_send(int fd, int ifindex, uint16_t ethertype,
                          const uint8_t dst_mac[6],
                          const uint8_t *frame, size_t len)
{
    return datalink_send_linux(fd, ifindex, ethertype, dst_mac, frame, len);
}

ssize_t scs_datalink_recv(int fd, uint8_t *buf, size_t buf_len)
{
    return recv(fd, buf, buf_len, 0);
}

int scs_datalink_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    return setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}

#endif  /* SCS_DATALINK_VIA_EXECUTIVE (executive backend) vs AF_PACKET probe */

#elif defined(__NetBSD__)

#include <fcntl.h>
#include <ifaddrs.h>
#include <net/bpf.h>
#include <net/if.h>
#include <net/if_dl.h>
#include <net/if_types.h>  /* IFT_ETHER -- scs_datalink_primary_iface() (vms-5ad) */
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/time.h>

/*
 * The bpf read-side buffer table. bpf(4): "the packet ... is preceded by a
 * ... struct bpf_hdr" and "more than one packet may be transferred to the
 * user process at a time" -- a single read(2) can return several captures
 * packed WORDALIGN'd back to back, so a receiver that wants "one frame per
 * call" (scs_datalink_recv()'s contract, matching Linux recv()) has to keep
 * whatever a read() did not yet hand out. This table is that per-fd queue.
 *
 * Fixed-size and small: SCSD opens exactly one datalink for its whole run.
 * Sized to 4 so a future second consumer (rd vms-30e, DECnet Phase IV) or a
 * test harness opening more than one in a process does not immediately need
 * to grow it.
 */
#define SCS_DATALINK_MAX_OPEN 4

struct scs_datalink_bpfbuf {
    int in_use;           /* 0 if this slot is free -- relies on the array's
                           * static zero-initialization, unlike an fd==-1
                           * sentinel which zero-init would NOT give us */
    int fd;
    uint8_t *buf;
    size_t buflen;       /* allocated size (from BIOCGBLEN) */
    size_t off;          /* offset of the next unconsumed capture */
    size_t used;          /* valid bytes in buf, from the last read() */
};

static struct scs_datalink_bpfbuf g_bpfbuf[SCS_DATALINK_MAX_OPEN];

static struct scs_datalink_bpfbuf *bpfbuf_find(int fd)
{
    for (int i = 0; i < SCS_DATALINK_MAX_OPEN; i++) {
        if (g_bpfbuf[i].in_use && g_bpfbuf[i].fd == fd) {
            return &g_bpfbuf[i];
        }
    }
    return NULL;
}

static struct scs_datalink_bpfbuf *bpfbuf_alloc(int fd, size_t buflen)
{
    for (int i = 0; i < SCS_DATALINK_MAX_OPEN; i++) {
        if (!g_bpfbuf[i].in_use) {
            g_bpfbuf[i].buf = malloc(buflen);
            if (g_bpfbuf[i].buf == NULL) {
                return NULL;
            }
            g_bpfbuf[i].in_use = 1;
            g_bpfbuf[i].fd = fd;
            g_bpfbuf[i].buflen = buflen;
            g_bpfbuf[i].off = 0;
            g_bpfbuf[i].used = 0;
            return &g_bpfbuf[i];
        }
    }
    return NULL;
}

static void bpfbuf_free(int fd)
{
    struct scs_datalink_bpfbuf *e = bpfbuf_find(fd);
    if (e == NULL) {
        return;
    }
    free(e->buf);
    memset(e, 0, sizeof(*e));
}

int scs_datalink_open(const char *ifname, uint16_t ethertype)
{
    int fd = open("/dev/bpf", O_RDWR);
    if (fd < 0) {
        /* Pre-cloning-device NetBSD: probe /dev/bpf0.. for a free unit. */
        char path[32];
        for (int unit = 0; unit < 16; unit++) {
            snprintf(path, sizeof(path), "/dev/bpf%d", unit);
            fd = open(path, O_RDWR);
            if (fd >= 0) {
                break;
            }
            if (errno != EBUSY) {
                break;
            }
        }
    }
    if (fd < 0) {
        return -1;
    }

    /* BIOCSBLEN must be set before BIOCSETIF to take effect; a failure here
     * is non-fatal (the kernel default buffer size is used instead). */
    unsigned int blen = 32768;
    (void)ioctl(fd, BIOCSBLEN, &blen);

    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, sizeof(ifr.ifr_name) - 1);
    if (ioctl(fd, BIOCSETIF, &ifr) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    unsigned int one = 1;
    /* Deliver frames as they arrive rather than batching until the buffer
     * fills -- SCS/HELLO timing (VC_RETRANSMIT_TIMEOUT_MS etc.) is real-time
     * sensitive. */
    if (ioctl(fd, BIOCIMMEDIATE, &one) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }
    /* scsd.c's frame builders already set the source MAC (and every other
     * header field) themselves; without this, bpf overwrites the source
     * address on write(2) with the interface's own, which is usually the
     * same value but is not guaranteed to be (and defeats the "hand it a
     * fully-built frame, apply no policy" contract send_frame_raw() and
     * this API both promise). */
    if (ioctl(fd, BIOCSHDRCMPLT, &one) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    /* Read-side filter restricting captured frames to `ethertype` -- the
     * bpf-classic equivalent of the Linux backend's
     * socket(AF_PACKET, SOCK_RAW, htons(ethertype)) protocol filter, so the
     * two backends see the same traffic. Standard bpf(4) two-instruction
     * "load ethertype at offset 12, compare, accept-all-or-reject" program;
     * BPF_STMT/BPF_JUMP are the public <net/bpf.h> macros. */
    struct bpf_insn insns[4] = {
        BPF_STMT(BPF_LD + BPF_H + BPF_ABS, 12),
        BPF_JUMP(BPF_JMP + BPF_JEQ + BPF_K, ethertype, 0, 1),
        BPF_STMT(BPF_RET + BPF_K, (u_int)-1),
        BPF_STMT(BPF_RET + BPF_K, 0),
    };
    struct bpf_program prog;
    prog.bf_len = 4;
    prog.bf_insns = insns;
    if (ioctl(fd, BIOCSETF, &prog) < 0) {
        int e = errno;
        close(fd);
        errno = e;
        return -1;
    }

    unsigned int actual_blen = blen;
    (void)ioctl(fd, BIOCGBLEN, &actual_blen);
    if (bpfbuf_alloc(fd, actual_blen) == NULL) {
        close(fd);
        errno = ENOMEM;
        return -1;
    }

    return fd;
}

void scs_datalink_close(int fd)
{
    bpfbuf_free(fd);
    close(fd);
}

int scs_datalink_get_hwaddr(const char *ifname, uint8_t mac_out[6])
{
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) < 0) {
        return -1;
    }
    int found = 0;
    for (struct ifaddrs *p = ifap; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_LINK) {
            continue;
        }
        if (strcmp(p->ifa_name, ifname) != 0) {
            continue;
        }
        struct sockaddr_dl *sdl = (struct sockaddr_dl *)(void *)p->ifa_addr;
        if (sdl->sdl_alen != 6) {
            continue;
        }
        memcpy(mac_out, LLADDR(sdl), 6);
        found = 1;
        break;
    }
    freeifaddrs(ifap);
    if (!found) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

int scs_datalink_primary_iface(char *out, size_t n)
{
    struct ifaddrs *ifap = NULL;
    if (getifaddrs(&ifap) < 0) {
        return -1;
    }
    int found = 0;
    for (struct ifaddrs *p = ifap; p != NULL; p = p->ifa_next) {
        if (p->ifa_addr == NULL || p->ifa_addr->sa_family != AF_LINK) {
            continue;
        }
        if (p->ifa_flags & IFF_LOOPBACK) {
            continue; /* vms-9d2 twin: skip loopback exactly as
                       * IFNET_READER_FOREACH's IFT_LOOP check does */
        }
        struct sockaddr_dl *sdl = (struct sockaddr_dl *)(void *)p->ifa_addr;
        if (sdl->sdl_type != IFT_ETHER) {
            continue; /* require Ethernet, exactly as the executive does */
        }
        if (out != NULL && n > 0) {
            strncpy(out, p->ifa_name, n - 1);
            out[n - 1] = '\0';
        }
        found = 1;
        break;
    }
    freeifaddrs(ifap);
    if (!found) {
        errno = ENODEV;
        return -1;
    }
    return 0;
}

/*
 * datalink_send_netbsd - THE NetBSD transport primitive, named for the same
 * census-attribution reason datalink_send_linux() above is: exactly one
 * function per backend contains the raw write(2)/sendto(2).
 */
static ssize_t datalink_send_netbsd(int fd, const uint8_t *frame, size_t len)
{
    /* bpf has no per-packet destination address (unlike AF_PACKET's
     * sockaddr_ll) -- the frame's own Ethernet header already carries the
     * destination MAC, set by scsd.c's builders exactly as before. */
    return write(fd, frame, len);
}

ssize_t scs_datalink_send(int fd, int ifindex, uint16_t ethertype,
                          const uint8_t dst_mac[6],
                          const uint8_t *frame, size_t len)
{
    (void)ifindex;
    (void)ethertype;
    (void)dst_mac;
    return datalink_send_netbsd(fd, frame, len);
}

ssize_t scs_datalink_recv(int fd, uint8_t *buf, size_t buf_len)
{
    struct scs_datalink_bpfbuf *e = bpfbuf_find(fd);
    if (e == NULL) {
        errno = EBADF;
        return -1;
    }

    if (e->off >= e->used) {
        ssize_t r = read(fd, e->buf, e->buflen);
        if (r < 0) {
            return -1; /* EAGAIN/EWOULDBLOCK on a BIOCSRTIMEOUT expiry, or a
                        * real error -- both propagate to the caller exactly
                        * as the Linux recv() path already does. */
        }
        if (r == 0) {
            /* No data before the read timeout on this NetBSD version's bpf
             * (some report timeout as a zero-length read rather than
             * EWOULDBLOCK) -- normalize to the same "nothing pending, come
             * back later" signal scsd.c already handles. */
            errno = EAGAIN;
            return -1;
        }
        e->off = 0;
        e->used = (size_t)r;
    }

    /* struct bpf_hdr precedes each captured frame in the buffer (bpf(4)):
     * bh_hdrlen is this header's own length (word-aligned, platform/version
     * dependent -- never assume sizeof(struct bpf_hdr)), bh_caplen is the
     * captured frame length that follows it. Captures are packed
     * BPF_WORDALIGN'd back to back. */
    if (e->off + sizeof(struct bpf_hdr) > e->used) {
        e->off = e->used; /* corrupt/truncated trailing header -- drop the
                           * rest of this buffer rather than read past it. */
        errno = EAGAIN;
        return -1;
    }
    struct bpf_hdr hdr;
    memcpy(&hdr, e->buf + e->off, sizeof(hdr));

    size_t frame_off = e->off + hdr.bh_hdrlen;
    if (frame_off + hdr.bh_caplen > e->used) {
        e->off = e->used;
        errno = EAGAIN;
        return -1;
    }

    size_t copy_len = hdr.bh_caplen;
    if (copy_len > buf_len) {
        copy_len = buf_len;
    }
    memcpy(buf, e->buf + frame_off, copy_len);

    e->off += BPF_WORDALIGN(hdr.bh_hdrlen + hdr.bh_caplen);

    return (ssize_t)copy_len;
}

int scs_datalink_set_recv_timeout(int fd, int seconds)
{
    struct timeval tv;
    tv.tv_sec = seconds;
    tv.tv_usec = 0;
    return ioctl(fd, BIOCSRTIMEOUT, &tv);
}

#else
#error "scs_datalink: no backend for this platform (only __linux__ and __NetBSD__ are implemented)"
#endif
