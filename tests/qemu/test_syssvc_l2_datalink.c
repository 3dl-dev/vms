/*
 * test_syssvc_l2_datalink.c - the executive L2 (raw datalink) socket surface
 * (rd vms-7eb, the auth slice of vms-1e4), driven against a real executive
 * through /dev/vms via the freestanding kif client (src/libvmssys/vms_kif.c),
 * the same footing as test_syssvc_cluster_member.c and test_syssvc_dlm_xnode.c.
 *
 * WHAT THIS PROVES. vms.ko opens a kernel-owned AF_PACKET/SOCK_RAW socket on
 * a real host NIC -- the primitive that lets a NON-ROOT VMS process do raw L2
 * I/O for the SCS cluster wire (ethertype 0x6007) without CAP_NET_RAW, because
 * the kernel owns the socket:
 *
 *   1. VMS_IOCTL_L2_OPEN on a real, non-loopback Ethernet interface (found by
 *      scanning /sys/class/net -- the same "skip loopback, require an Ethernet
 *      link-layer type" classification exec_netdev_primary uses in the
 *      kernel, see src/kernel/exec_kbackend_linux.h) with ethertype 0x6007 ->
 *      SS$_NORMAL, a nonzero ifindex, and a hardware address.
 *   2. VMS_IOCTL_L2_SEND a small frame to the broadcast address -> SS$_NORMAL
 *      with the full length reported sent (no peer needed -- this proves the
 *      send path, not delivery).
 *   3. VMS_IOCTL_L2_CLOSE -> SS$_NORMAL.
 *   4. AUTH GATE (vms-1e4): a caller WITHOUT the PHY_IO privilege gets
 *      SS$_NOPRIV from OPEN -- proven by temporarily disabling PHY_IO via
 *      $SETPRV (vms_kif_setprv), attempting OPEN, then restoring it.
 *   5. Honest failure: OPEN against a nonexistent interface name ->
 *      SS$_NOSUCHDEV, never a fabricated handle (INV-6).
 *
 * INV-6: every assertion below drives the REAL executive through /dev/vms;
 * nothing here hand-opens a userspace raw socket or fakes a frame send.
 *
 * Honest SKIP (77) when /dev/vms is absent, or when no real (non-loopback)
 * Ethernet interface can be found to drive OPEN against -- the test_syssvc_*
 * honest-skip-77 contract, ci.yml kernel-executive-negative-control.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include "vms_ioctl.h"
#include "vms_kif.h"

#define SS_NORMAL     1u
#define SS_BADPARAM   20u
#define SS_NOPRIV     36u
#define SS_NOSUCHDEV  2680u
#define EXIT_SKIP     77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/*
 * find_ether_iface - the userspace twin of exec_netdev_primary's
 * classification (exec_kbackend_linux.h SS11): scan /sys/class/net for the
 * first non-loopback interface whose ARPHRD_* link type is Ethernet (1).
 * Returns 1 (+ name filled) if found, 0 if not (honest skip -- this harness's
 * QEMU boot always attaches a virtio-net NIC (tests/qemu/run_tests.sh), so a
 * real environment finds one; an environment that genuinely has none cannot
 * exercise this facility at all).
 */
static int find_ether_iface(char *name, size_t namesz)
{
    DIR *d;
    struct dirent *de;
    int found = 0;

    d = opendir("/sys/class/net");
    if (!d)
        return 0;

    while ((de = readdir(d)) != NULL) {
        char typepath[320];
        FILE *tf;
        int type = -1;

        if (de->d_name[0] == '.')
            continue;
        if (strcmp(de->d_name, "lo") == 0)
            continue;

        snprintf(typepath, sizeof(typepath), "/sys/class/net/%s/type", de->d_name);
        tf = fopen(typepath, "r");
        if (!tf)
            continue;
        if (fscanf(tf, "%d", &type) != 1)
            type = -1;
        fclose(tf);

        if (type == 1 /* ARPHRD_ETHER */) {
            snprintf(name, namesz, "%s", de->d_name);
            found = 1;
            break;
        }
    }
    closedir(d);
    return found;
}

int main(void)
{
    char ifname[16];
    uint32_t st, handle = 0, ifindex = 0;
    uint8_t hwaddr[6];
    int i;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_l2_datalink ===\n");

    int probe = open("/dev/vms", O_RDWR);
    if (probe < 0) {
        printf("=== test_syssvc_l2_datalink: 0 passed, 0 failed (SKIPPED: no "
               "/dev/vms -- the L2 socket is executive-resident) ===\n");
        return EXIT_SKIP;
    }
    close(probe);

    if (!find_ether_iface(ifname, sizeof(ifname))) {
        printf("=== test_syssvc_l2_datalink: 0 passed, 0 failed (SKIPPED: no "
               "non-loopback Ethernet interface found under /sys/class/net) ===\n");
        return EXIT_SKIP;
    }
    printf("  (driving against interface \"%s\")\n", ifname);

    /* ---- 1. OPEN on a real interface, ethertype 0x6007 (SCS) ----------- */
    memset(hwaddr, 0, sizeof(hwaddr));
    st = vms_kif_l2_open(ifname, 0x6007u, &handle, &ifindex, hwaddr);
    CHECK(st == SS_NORMAL, "L2_OPEN on a real interface -> SS$_NORMAL");
    CHECK(handle != 0, "L2_OPEN returns a nonzero handle");
    CHECK(ifindex != 0, "L2_OPEN resolves a nonzero interface index");
    {
        int all_zero = 1;
        for (i = 0; i < 6; i++)
            if (hwaddr[i] != 0) { all_zero = 0; break; }
        CHECK(!all_zero, "L2_OPEN reports a nonzero hardware (MAC) address");
    }

    /* ---- 2. SEND a small frame to the broadcast address ---------------- */
    if (st == SS_NORMAL) {
        static const uint8_t bcast[6] = { 0xff, 0xff, 0xff, 0xff, 0xff, 0xff };
        /* vms-a84d: the datalink contract is a FULLY-BUILT Ethernet frame --
         * the executive sends it VERBATIM (scsd.c already builds the header;
         * exec_l2_send no longer prepends one). So build a complete 60-byte
         * frame here: dst | src | ethertype | payload (60 = the minimum
         * standard Ethernet frame). Sending a bare payload would now go on the
         * wire as a malformed short frame. */
        uint8_t frame[60];
        uint32_t actlen = 0;

        memset(frame, 0, sizeof(frame));
        memcpy(frame + 0, bcast, 6);       /* dst = broadcast */
        memcpy(frame + 6, hwaddr, 6);      /* src = our resolved MAC (from L2_OPEN) */
        frame[12] = 0x60; frame[13] = 0x07;/* ethertype 0x6007, network order */
        memset(frame + 14, 0xA5, sizeof(frame) - 14); /* payload */
        st = vms_kif_l2_send(handle, ifindex, 0x6007u, bcast, frame,
                             (uint32_t)sizeof(frame), &actlen);
        CHECK(st == SS_NORMAL, "L2_SEND a full broadcast frame -> SS$_NORMAL");
        CHECK(actlen == sizeof(frame), "L2_SEND reports the full frame length sent");

        /* ---- 3. CLOSE -------------------------------------------------- */
        st = vms_kif_l2_close(handle);
        CHECK(st == SS_NORMAL, "L2_CLOSE -> SS$_NORMAL");
    } else {
        printf("  (skipping SEND/CLOSE checks -- OPEN did not succeed)\n");
    }

    /* ---- 4. AUTH GATE: no PHY_IO -> SS$_NOPRIV from OPEN ---------------- */
    {
        uint64_t prev = 0;
        uint32_t sp_st, h2 = 0, ix2 = 0;

        sp_st = vms_kif_setprv(VMS_PRV_M_PHY_IO, 0 /* disable */, 0 /* temporary */, &prev);
        CHECK(sp_st == SS_NORMAL, "$SETPRV disable PHY_IO -> SS$_NORMAL");

        st = vms_kif_l2_open(ifname, 0x6007u, &h2, &ix2, NULL);
        CHECK(st == SS_NOPRIV, "L2_OPEN without PHY_IO -> SS$_NOPRIV");
        CHECK(h2 == 0, "L2_OPEN without PHY_IO mints no handle");

        sp_st = vms_kif_setprv(VMS_PRV_M_PHY_IO, 1 /* enable */, 0 /* temporary */, NULL);
        CHECK(sp_st == SS_NORMAL, "$SETPRV re-enable PHY_IO -> SS$_NORMAL");

        /* Prove PHY_IO is back: OPEN succeeds again, then clean up. */
        st = vms_kif_l2_open(ifname, 0x6007u, &h2, &ix2, NULL);
        CHECK(st == SS_NORMAL, "L2_OPEN succeeds again once PHY_IO is restored");
        if (st == SS_NORMAL)
            (void)vms_kif_l2_close(h2);
    }

    /* ---- 5. Honest failure: OPEN against a nonexistent interface ------- */
    {
        uint32_t h3 = 0, ix3 = 0;

        st = vms_kif_l2_open("ovmx-no-such-if0", 0x6007u, &h3, &ix3, NULL);
        CHECK(st == SS_NOSUCHDEV,
              "L2_OPEN against a nonexistent interface -> SS$_NOSUCHDEV (honest, no fake handle)");
        CHECK(h3 == 0, "L2_OPEN against a nonexistent interface mints no handle");
    }

    printf("=== test_syssvc_l2_datalink: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
