/* sca_l2probe.c - raw-L2 (AF_PACKET) probe for the DLM cross-node harness, H1
 * (rd vms-534).
 *
 * A DELIBERATELY TINY, dependency-free tool (libc + Linux UAPI only, builds
 * -static with musl-gcc) that does exactly two jobs on ethertype 0x6007 (DEC
 * LAVC/SCA), the SAME ethertype and group multicast MAC (AB-00-04-01-01-01)
 * SCSD's HELLO beacon uses:
 *
 *   send <iface> <count> <interval_ms> <marker>
 *       multicast <count> minimal 0x6007 frames to AB-00-04-01-01-01 from
 *       <iface>'s HW MAC, payload carrying <marker>.
 *
 *   recv <iface> <seconds> <pcap_out>
 *       bind AF_PACKET/SOCK_RAW to 0x6007 on <iface>, join the group multicast
 *       (PACKET_MR_PROMISC so a flooded group frame is delivered even though it
 *       is not addressed to our unicast MAC), and for <seconds> log every
 *       received 0x6007 frame AND append it to <pcap_out> in libpcap format.
 *       A frame whose ETH source differs from our own MAC is a PEER frame --
 *       proof the wire flooded another node's multicast to us.
 *
 * WHY THIS EXISTS: H1's make-or-break unknown is whether a QEMU `socket`
 * (mcast) / listen-connect netdev faithfully FLOODS the 0x6007 group multicast
 * between two guests with NO host bridge and NO privilege (CI-runnable). This
 * tool answers that in isolation -- no vms.ko, no SCSD identity, no SYSGEN
 * store -- so a red result points at the netdev, not the daemon. Once the
 * netdev is proven, the same `recv` runs PASSIVELY alongside SCSD in the full
 * harness to capture the pcap artifact (multiple AF_PACKET SOCK_RAW sockets on
 * one iface each get their own copy of every matching frame).
 *
 * It fabricates nothing: it prints verbatim what recvfrom() delivered and
 * writes those exact bytes to the pcap. Clean-room Rule 8: the frame it sends
 * is a minimal marker frame, NOT a spec HELLO -- HELLO fidelity is scs_hello.c;
 * this only exercises the transport.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <net/if.h>
#include <arpa/inet.h>
#include <linux/if_packet.h>
#include <linux/if_ether.h>

#define SCA_ETHERTYPE 0x6007
static const uint8_t GROUP_MAC[6] = { 0xAB, 0x00, 0x04, 0x01, 0x01, 0x01 };

static int get_ifindex_mac(int fd, const char *ifname, int *ifindex, uint8_t mac[6])
{
    struct ifreq ifr;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFINDEX, &ifr) < 0) {
        fprintf(stderr, "SCA-L2PROBE-E-IFINDEX, ioctl(SIOCGIFINDEX,%s): %s\n",
                ifname, strerror(errno));
        return -1;
    }
    *ifindex = ifr.ifr_ifindex;
    memset(&ifr, 0, sizeof(ifr));
    strncpy(ifr.ifr_name, ifname, IFNAMSIZ - 1);
    if (ioctl(fd, SIOCGIFHWADDR, &ifr) < 0) {
        fprintf(stderr, "SCA-L2PROBE-E-HWADDR, ioctl(SIOCGIFHWADDR,%s): %s\n",
                ifname, strerror(errno));
        return -1;
    }
    memcpy(mac, ifr.ifr_hwaddr.sa_data, 6);
    return 0;
}

static const char *macs(const uint8_t m[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x",
             m[0], m[1], m[2], m[3], m[4], m[5]);
    return out;
}

/* --- libpcap file format (DLT_EN10MB), just enough to be tcpdump-readable --- */
static int pcap_open(const char *path, FILE **outf)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "SCA-L2PROBE-E-PCAP, fopen(%s): %s\n", path, strerror(errno));
        return -1;
    }
    uint32_t magic = 0xa1b2c3d4u; /* us-resolution, host byte order */
    uint16_t vmaj = 2, vmin = 4;
    int32_t  thiszone = 0;
    uint32_t sigfigs = 0, snaplen = 65535, network = 1 /* LINKTYPE_ETHERNET */;
    fwrite(&magic, 4, 1, f);
    fwrite(&vmaj, 2, 1, f);
    fwrite(&vmin, 2, 1, f);
    fwrite(&thiszone, 4, 1, f);
    fwrite(&sigfigs, 4, 1, f);
    fwrite(&snaplen, 4, 1, f);
    fwrite(&network, 4, 1, f);
    fflush(f);
    *outf = f;
    return 0;
}

static void pcap_write(FILE *f, const uint8_t *buf, size_t n)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    uint32_t sec = (uint32_t)ts.tv_sec;
    uint32_t usec = (uint32_t)(ts.tv_nsec / 1000);
    uint32_t incl = (uint32_t)n, orig = (uint32_t)n;
    fwrite(&sec, 4, 1, f);
    fwrite(&usec, 4, 1, f);
    fwrite(&incl, 4, 1, f);
    fwrite(&orig, 4, 1, f);
    fwrite(buf, 1, n, f);
    fflush(f);
}

static int do_send(const char *ifname, int count, int interval_ms, const char *marker)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(SCA_ETHERTYPE));
    if (fd < 0) {
        fprintf(stderr, "SCA-L2PROBE-E-SOCKET, socket(AF_PACKET): %s "
                "(needs CAP_NET_RAW)\n", strerror(errno));
        return 1;
    }
    int ifindex; uint8_t mac[6];
    if (get_ifindex_mac(fd, ifname, &ifindex, mac) != 0) return 1;

    char mbuf[18];
    printf("SCA-L2PROBE-SEND, iface=%s ifindex=%d srcmac=%s -> %s x%d marker='%s'\n",
           ifname, ifindex, macs(mac, mbuf),
           "ab:00:04:01:01:01", count, marker);
    fflush(stdout);

    uint8_t frame[64];
    memset(frame, 0, sizeof(frame));
    memcpy(frame + 0, GROUP_MAC, 6);          /* dst = group multicast */
    memcpy(frame + 6, mac, 6);                /* src = our HW MAC       */
    frame[12] = (SCA_ETHERTYPE >> 8) & 0xff;  /* ethertype 0x6007       */
    frame[13] = SCA_ETHERTYPE & 0xff;
    /* payload (46 bytes min so the frame reaches the 60-byte L2 minimum) */
    size_t mlen = strlen(marker);
    if (mlen > 40) mlen = 40;
    memcpy(frame + 14, marker, mlen);
    size_t framelen = 60; /* 14 header + 46 payload */

    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(SCA_ETHERTYPE);
    sa.sll_ifindex = ifindex;
    sa.sll_halen = 6;
    memcpy(sa.sll_addr, GROUP_MAC, 6);

    int sent = 0;
    for (int i = 0; i < count; i++) {
        ssize_t r = sendto(fd, frame, framelen, 0, (struct sockaddr *)&sa, sizeof(sa));
        if (r < 0) {
            fprintf(stderr, "SCA-L2PROBE-E-SENDTO, seq=%d: %s\n", i, strerror(errno));
        } else {
            sent++;
            printf("SCA-L2PROBE-TX, seq=%d bytes=%zd\n", i, r);
            fflush(stdout);
        }
        if (interval_ms > 0 && i + 1 < count) {
            struct timespec req = { interval_ms / 1000, (long)(interval_ms % 1000) * 1000000L };
            nanosleep(&req, NULL);
        }
    }
    printf("SCA-L2PROBE-SENT, frames=%d\n", sent);
    fflush(stdout);
    close(fd);
    return sent > 0 ? 0 : 1;
}

static int do_recv(const char *ifname, int seconds, const char *pcap_out)
{
    int fd = socket(AF_PACKET, SOCK_RAW, htons(SCA_ETHERTYPE));
    if (fd < 0) {
        fprintf(stderr, "SCA-L2PROBE-E-SOCKET, socket(AF_PACKET): %s "
                "(needs CAP_NET_RAW)\n", strerror(errno));
        return 1;
    }
    int ifindex; uint8_t ourmac[6];
    if (get_ifindex_mac(fd, ifname, &ifindex, ourmac) != 0) return 1;

    /* Bind to the interface + ethertype so only 0x6007 frames on this NIC are
     * delivered. */
    struct sockaddr_ll sa;
    memset(&sa, 0, sizeof(sa));
    sa.sll_family = AF_PACKET;
    sa.sll_protocol = htons(SCA_ETHERTYPE);
    sa.sll_ifindex = ifindex;
    if (bind(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
        fprintf(stderr, "SCA-L2PROBE-E-BIND, bind(%s): %s\n", ifname, strerror(errno));
        return 1;
    }

    /* PROMISC so a group-multicast frame flooded by the wire is delivered even
     * though its dst (AB-00-04-01-01-01) is not our unicast MAC. */
    struct packet_mreq mr;
    memset(&mr, 0, sizeof(mr));
    mr.mr_ifindex = ifindex;
    mr.mr_type = PACKET_MR_PROMISC;
    if (setsockopt(fd, SOL_PACKET, PACKET_ADD_MEMBERSHIP, &mr, sizeof(mr)) < 0) {
        fprintf(stderr, "SCA-L2PROBE-W-PROMISC, PACKET_ADD_MEMBERSHIP failed: %s "
                "(continuing; group frames may be missed)\n", strerror(errno));
    }

    FILE *pf = NULL;
    if (pcap_out && pcap_out[0] && pcap_open(pcap_out, &pf) != 0) return 1;

    char mbuf[18];
    printf("SCA-L2PROBE-RECV, iface=%s ifindex=%d ourmac=%s seconds=%d pcap=%s\n",
           ifname, ifindex, macs(ourmac, mbuf), seconds, pcap_out ? pcap_out : "(none)");
    fflush(stdout);

    /* Bound each recvfrom so we wake to check the deadline on an idle wire. */
    struct timeval tv = { 1, 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    time_t deadline = time(NULL) + seconds;
    long total = 0, peer = 0;
    int peer_seen = 0;
    uint8_t buf[2048];
    while (time(NULL) < deadline) {
        ssize_t n = recvfrom(fd, buf, sizeof(buf), 0, NULL, NULL);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) continue;
            fprintf(stderr, "SCA-L2PROBE-E-RECV, %s\n", strerror(errno));
            break;
        }
        if (n < 14) continue;
        uint16_t et = (uint16_t)((buf[12] << 8) | buf[13]);
        if (et != SCA_ETHERTYPE) continue; /* belt over the protocol bind */
        total++;
        if (pf) pcap_write(pf, buf, (size_t)n);
        int is_peer = (memcmp(buf + 6, ourmac, 6) != 0);
        char sbuf[18], dbuf[18];
        printf("SCA-L2PROBE-RX, %s src=%s dst=%s ethertype=0x%04x len=%zd\n",
               is_peer ? "PEER" : "self", macs(buf + 6, sbuf), macs(buf + 0, dbuf), et, n);
        fflush(stdout);
        if (is_peer) {
            peer++;
            if (!peer_seen) {
                peer_seen = 1;
                printf("SCA-L2PROBE-PEER-OK, first peer 0x6007 frame received src=%s\n",
                       macs(buf + 6, sbuf));
                fflush(stdout);
            }
        }
    }
    if (pf) fclose(pf);
    printf("SCA-L2PROBE-DONE, total_frames=%ld peer_frames=%ld peer_seen=%d\n",
           total, peer, peer_seen);
    fflush(stdout);
    close(fd);
    return peer_seen ? 0 : 2; /* 2 = ran clean but saw no peer frame */
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "send") == 0 && argc >= 6)
        return do_send(argv[2], atoi(argv[3]), atoi(argv[4]), argv[5]);
    if (argc >= 2 && strcmp(argv[1], "recv") == 0 && argc >= 4)
        return do_recv(argv[2], atoi(argv[3]), argc >= 5 ? argv[4] : NULL);

    fprintf(stderr,
        "usage:\n"
        "  %s send <iface> <count> <interval_ms> <marker>\n"
        "  %s recv <iface> <seconds> [pcap_out]\n"
        "  ethertype 0x%04x, group multicast AB-00-04-01-01-01 (DEC LAVC/SCA).\n",
        argv[0], argv[0], SCA_ETHERTYPE);
    return 64;
}
