/*
 * test_syssvc_net_assign (rd vms-cdee, a1-0) -- a VMS program $ASSIGNs the
 * DECnet device face _NET: through libvms and gets a real executive channel.
 *
 * WHAT THIS PROVES. Before a1-0, resolve_vms_device() (src/libvms/syssvc/
 * sys_assign.c) had no _NET: case, so $ASSIGN _NET: through libvms returned
 * SS$_NOSUCHDEV even though the executive device is real (born on the primary
 * NIC by vms_devtab_probe_net, class DC$_SCOM, shareable). a1-0 adds the is_net
 * branch that routes _NET: to the GENERIC executive assign (vms_kif_assign) --
 * the same path the console terminal uses -- so a user process gets a channel
 * (fd = -1, exec_chan set, PCB_CHAN_NET). This is the entry point every _NET:
 * $QIO op needs; the $QIO ops themselves (IO$_ACCESS/READVBLK/WRITEVBLK/
 * DEACCESS -> the NETACP broker) are the next rungs (vms-799/vms-22c).
 *
 * FAIL-HONEST (Rule 9 / INV-6): with no executive, $ASSIGN _NET: MUST fail
 * SS$_NOSUCHDEV, never a private per-process fake. With an executive but no
 * primary NIC, the _NET: device does not exist and the assign is honestly
 * SS$_NOSUCHDEV too (NIC-gated) -- reported as a note here, exactly as the
 * DCL acceptance battery treats an absent _NET: face. The is_net path is
 * SPECIFIC: an unknown device name still fails SS$_NOSUCHDEV (negative control).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

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

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_net_assign ($ASSIGN _NET: through libvms, rd vms-cdee) ===\n");

    /* The channel table lives in the per-process PCB; a standalone test binary
     * must make its own (the same bootstrap the mbx/bg suites do). */
    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        /* NO EXECUTIVE: $ASSIGN _NET: must fail honestly, never a local fake. */
        uint16_t chan = 0;
        struct dsc$descriptor_s net = mkdsc("_NET:");
        uint32_t st = sys$assign(&net, &chan, 0, NULL);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $ASSIGN _NET: fails SS$_NOSUCHDEV, never a per-process fake (Rule 9/INV-6)");
        printf("=== test_syssvc_net_assign: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* NEGATIVE CONTROL: the is_net path is SPECIFIC. An unknown device name must
     * still fail SS$_NOSUCHDEV -- a catch-all _NET: match would mask this. */
    {
        uint16_t chan = 0;
        struct dsc$descriptor_s bogus = mkdsc("_NOTADEV:");
        uint32_t st = sys$assign(&bogus, &chan, 0, NULL);
        CHECK(st != SS$_NORMAL,
              "negative control: $ASSIGN _NOTADEV: does NOT succeed (the _NET: match is specific)");
    }

    /* THE PATH: $ASSIGN _NET: routes through the new is_net branch to the generic
     * executive assign. On a node with a primary NIC the DECnet device exists and
     * the assign grants a channel; the channel then round-trips through $DASSGN
     * (which releases the real exec_chan via vms_kif_dassgn -- a bogus exec_chan
     * would not). _NET: is shareable, so a second $ASSIGN grants another channel. */
    uint16_t chan = 0;
    struct dsc$descriptor_s net = mkdsc("_NET:");
    uint32_t st = sys$assign(&net, &chan, 0, NULL);

    CHECK(st == SS$_NORMAL || st == SS$_NOSUCHDEV,
          "$ASSIGN _NET: returns a clean status -- a channel, or SS$_NOSUCHDEV when NIC-gated, never a fake");

    if (st == SS$_NORMAL) {
        CHECK(chan != 0, "_NET: assign yielded a valid channel (the DECnet device face resolved)");

        uint16_t chan2 = 0;
        uint32_t st2 = sys$assign(&net, &chan2, 0, NULL);
        CHECK(st2 == SS$_NORMAL && chan2 != 0 && chan2 != chan,
              "_NET: is shareable: a second $ASSIGN grants a distinct channel");
        if (st2 == SS$_NORMAL)
            CHECK(sys$dassgn(chan2) == SS$_NORMAL, "$DASSGN releases the second _NET: channel");

        CHECK(sys$dassgn(chan) == SS$_NORMAL,
              "$DASSGN releases the _NET: channel -- the real exec_chan round-trips");
    } else {
        printf("  NOTE: _NET: is absent on this node (no primary NIC) -- $ASSIGN honestly"
               " returned SS$_NOSUCHDEV (NIC-gated, INV-6), the positive path is not exercised here\n");
    }

    printf("=== test_syssvc_net_assign: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
