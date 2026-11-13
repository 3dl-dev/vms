/*
 * test_syssvc_lan_sense.c - the LAN controller unit ETH0: answers $QIO
 * IO$_SENSEMODE from the EXECUTIVE, and the executive datalink opens on that
 * VMS name (rd vms-d3cd), via the PUBLIC sys$ API against a real /dev/vms.
 *
 * WHY. The booted NETACP (NETACP.EXE, executive datalink build) used to find
 * its NIC and MAC with Linux libc -- socket(AF_INET)+SIOCGIFHWADDR, getifaddrs,
 * if_nametoindex. It now does what a VMS program does with a LAN device:
 * $ASSIGN ETH0:, $QIOW IO$_SENSEMODE, read the hardware address out of the
 * extended characteristics (the NMA$C_PCLI_HWA entry), and opens its datalink
 * naming ETH0: -- the executive resolves the host interface privately. This
 * suite proves each of those against the executive:
 *
 *   1. $ASSIGN ETH0: grants a channel (the executive's DC$_SCOM row);
 *   2. $QIOW IO$_SENSEMODE on it: P1 byte 0 is DC$_SCOM, and the extended
 *      characteristics carry parameter 1160 (string) = the hardware address
 *      of a real Ethernet interface of this node, read independently from
 *      /sys/class/net/<if>/address -- not zero, not invented;
 *   3. VMS_IOCTL_L2_OPEN on the VMS name "ETH0:" binds the SAME interface as
 *      the host name does (same ifindex, same hwaddr), and an unknown VMS
 *      device name is SS$_NOSUCHDEV;
 *   4. what the channel cannot do is refused, not faked: IO$_READVBLK is
 *      SS$_ILLIOFUNC; the executive's LAN sense on a channel that is not the
 *      LAN unit (_NET:) is SS$_IVDEVNAM.
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/lan-sense-hwaddr-not-reported.defect.
 * Requires a real, insmod'd vms.ko and a NIC (the harness attaches virtio-net).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dirent.h>
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "iodef.h"
#include "iosbdef.h"
#include "dcdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

extern uint32_t vms$$chan_exec_chan(uint16_t chan);

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Does `mac` equal the address of an Ethernet interface (ARPHRD_ETHER) of
 * this node, read from sysfs? Fills `name` with that interface. */
static int mac_is_a_real_nic(const uint8_t mac[6], char *name, size_t namesz)
{
    DIR *d = opendir("/sys/class/net");
    struct dirent *de;
    int found = 0;
    if (!d) return 0;
    while (!found && (de = readdir(d)) != NULL) {
        char path[320];
        FILE *f;
        int type = -1;
        unsigned b[6];
        if (de->d_name[0] == '.' || strcmp(de->d_name, "lo") == 0)
            continue;
        snprintf(path, sizeof path, "/sys/class/net/%s/type", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fscanf(f, "%d", &type) != 1) type = -1;
        fclose(f);
        if (type != 1)
            continue;
        snprintf(path, sizeof path, "/sys/class/net/%s/address", de->d_name);
        if (!(f = fopen(path, "r"))) continue;
        if (fscanf(f, "%x:%x:%x:%x:%x:%x", &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
            int same = 1;
            for (int i = 0; i < 6; i++)
                if ((uint8_t)b[i] != mac[i]) same = 0;
            if (same) {
                snprintf(name, namesz, "%s", de->d_name);
                found = 1;
            }
        }
        fclose(f);
    }
    closedir(d);
    return found;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_lan_sense ($QIO IO$_SENSEMODE on ETH0:, rd vms-d3cd) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    struct dsc$descriptor_s eth = mkdsc("ETH0:");
    uint16_t chan = 0;
    uint32_t st;

    if (!executive_present()) {
        st = sys$assign(&eth, &chan, 0, NULL);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $ASSIGN ETH0: fails SS$_NOSUCHDEV, never a per-process fake");
        printf("=== test_syssvc_lan_sense: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* ---- 1. $ASSIGN ETH0: ---------------------------------------------- */
    st = sys$assign(&eth, &chan, 0, NULL);
    if (st == SS$_NOSUCHDEV) {
        printf("=== test_syssvc_lan_sense: %d passed, %d failed (SKIPPED: the executive"
               " entered no ETH0: -- this node has no NIC) ===\n", pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }
    CHECK(st == SS$_NORMAL && chan != 0, "$ASSIGN ETH0: grants a channel to the LAN unit");

    /* ---- 2. $QIOW IO$_SENSEMODE ----------------------------------------- */
    uint8_t chars[8];
    uint8_t ext[64];
    struct dsc$descriptor_s xd;
    xd.dsc$w_length = (uint16_t)sizeof ext;
    xd.dsc$b_dtype = DSC$K_DTYPE_T;
    xd.dsc$b_class = DSC$K_CLASS_S;
    xd.dsc$a_pointer = (char *)ext;
    struct _iosb iosb;
    memset(chars, 0xEE, sizeof chars);
    memset(ext, 0, sizeof ext);
    memset(&iosb, 0, sizeof iosb);
    st = sys$qiow(0, chan, IO$_SENSEMODE, &iosb, NULL, 0, chars, sizeof chars,
                  0, (uintptr_t)&xd, 0, 0);
    CHECK(st == SS$_NORMAL && iosb.iosb$w_status == SS$_NORMAL,
          "$QIOW IO$_SENSEMODE on ETH0: completes SS$_NORMAL");
    CHECK(chars[0] == DC$_SCOM, "the characteristics name the device class DC$_SCOM");
    uint16_t id = (uint16_t)(ext[0] | (ext[1] << 8));
    uint16_t len = (uint16_t)(ext[2] | (ext[3] << 8));
    CHECK(iosb.iosb$w_bcnt == 10 && id == (1160u | 0x1000u) && len == 6,
          "the extended characteristics carry NMA$C_PCLI_HWA (1160, a 6-byte string)");
    uint8_t mac[6];
    memcpy(mac, ext + 4, 6);
    int zero = 1;
    for (int i = 0; i < 6; i++) if (mac[i]) zero = 0;
    char nic[64] = "";
    /* negctl: lan-sense-hwaddr-not-reported */
    CHECK(!zero && mac_is_a_real_nic(mac, nic, sizeof nic),
          "the sensed hardware address is a real Ethernet interface's address (sysfs)");
    printf("  (sensed %02X-%02X-%02X-%02X-%02X-%02X = %s)\n",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5], nic[0] ? nic : "?");

    /* ---- 3. L2_OPEN on the VMS name binds the same interface ------------ */
    {
        uint32_t h1 = 0, ix1 = 0, h2 = 0, ix2 = 0;
        uint8_t hw1[6] = {0}, hw2[6] = {0};
        uint32_t s1 = vms_kif_l2_open("ETH0:", 0x6007u, NULL, &h1, &ix1, hw1);
        CHECK(s1 == SS$_NORMAL && h1 != 0, "L2_OPEN on the VMS name ETH0: -> SS$_NORMAL");
        if (nic[0]) {
            uint32_t s2 = vms_kif_l2_open(nic, 0x6007u, NULL, &h2, &ix2, hw2);
            CHECK(s2 == SS$_NORMAL && ix1 != 0 && ix1 == ix2 && memcmp(hw1, hw2, 6) == 0 &&
                  memcmp(hw1, mac, 6) == 0,
                  "ETH0: binds the same interface (ifindex, hwaddr) the sensed address names");
            if (h2) (void)vms_kif_l2_close(h2);
        }
        if (h1) (void)vms_kif_l2_close(h1);
        uint32_t h3 = 0, ix3 = 0;
        uint32_t s3 = vms_kif_l2_open("NOTADEV0:", 0x6007u, NULL, &h3, &ix3, NULL);
        CHECK(s3 == SS$_NOSUCHDEV && h3 == 0,
              "L2_OPEN on a VMS name that is not the LAN unit -> SS$_NOSUCHDEV, no handle");
    }

    /* ---- 4. refused, not faked ---------------------------------------- */
    {
        uint8_t buf[64];
        memset(&iosb, 0, sizeof iosb);
        st = sys$qiow(0, chan, IO$_READVBLK, &iosb, NULL, 0, buf, sizeof buf, 0, 0, 0, 0);
        CHECK(st == SS$_ILLIOFUNC, "IO$_READVBLK on ETH0: is SS$_ILLIOFUNC (not a fake read)");

        uint16_t nchan = 0;
        struct dsc$descriptor_s net = mkdsc("_NET:");
        if (sys$assign(&net, &nchan, 0, NULL) == SS$_NORMAL) {
            uint8_t hw[6];
            uint32_t lk = 0;
            uint32_t ls = vms_kif_lan_sense(vms$$chan_exec_chan(nchan), hw, &lk);
            CHECK(ls == SS$_IVDEVNAM,
                  "the executive's LAN sense on a channel to _NET: (not the LAN unit) -> SS$_IVDEVNAM");
            (void)sys$dassgn(nchan);
        }
    }

    CHECK(sys$dassgn(chan) == SS$_NORMAL, "$DASSGN ETH0: -> SS$_NORMAL");

    printf("=== test_syssvc_lan_sense: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
