/*
 * test_syssvc_tcpip_config.c - TCP/IP Services CONFIG/MANAGEMENT plane against
 * a real /dev/vms (vms-67f).
 *
 * ============================================================
 * WHAT THIS PROVES. The config half of TCP/IP Services for OVMX: an operator
 * can configure OVMX IP networking THE VMS WAY -- set the local host name,
 * domain, and an interface address -- and it (a) takes effect on the interface
 * over the real substrate stack and (b) is recorded in the VMS-faithful
 * TCPIP$* SYSTEM logical names, readable back by any process. This is the
 * engine src/vmstcpip/mgmt/tcpip_config.h, which TCPIP$CONFIG.COM and the DCL
 * `TCPIP SET/SHOW` verbs drive; this suite drives it directly, byte-exact,
 * against a real executive.
 *
 * THE INV-6 CORE. A TCPIP$* name is created in LNM$SYSTEM, the executive-
 * resident system-wide logical-name table (over /dev/vms, vms-d37). The proof
 * that it is NOT a per-process fake is twofold:
 *   1. The value written through the config engine's own define is read back
 *      through the INDEPENDENT public sys$trnlnm against LNM$SYSTEM -- the same
 *      executive arena test_syssvc_lnm_crossproc.c proves is cross-process.
 *   2. With NO executive, the define fails SS$_NOSUCHDEV -- an honest error,
 *      never a local table reporting success while sharing nothing.
 * And the interface half is proven against the kernel's OWN address table
 * (SIOCGIFADDR), not a value this process stashed.
 *
 * NO NIC in the syssvc harness (`-nic none`, tests/qemu/init.sh): the only
 * interface is loopback. The interface-apply proof therefore configures a
 * SECONDARY address on a `lo:0` alias -- a genuine kernel address-table entry
 * (the substrate really holds it), applied and read back over the real stack,
 * WITHOUT disturbing 127.0.0.1 that other suites (test_syssvc_bg_echo) rely on.
 * The engine's apply path is identical for a real EWA0:-backed interface; only
 * the interface name differs.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * anchored by tcpip-config-hostaddr-not-defined, which drops the
 * TCPIP$INET_HOSTADDR define in tcpip_cfg_configure() (st = SS$_NORMAL without
 * creating the logical). configure() still returns success and the host/domain
 * logicals + the interface address are still real, so ONLY this suite's
 * "TCPIP$INET_HOSTADDR reads back the configured host address" assertion
 * reddens -- the exact config-does-not-round-trip regression the plane exists
 * to prevent.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "ssdef.h"
#include "vms_kif.h"

#include "../../src/vmstcpip/mgmt/tcpip_config.h"

#define EXIT_SKIP 77

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* The config we exercise. A private RFC-1918 address on a loopback alias so it
 * cannot collide with anything the harness reaches. */
#define TEST_HOST   "OVMXNODE"
#define TEST_DOMAIN "ovmx.example"
#define TEST_IFACE  "lo:0"
#define TEST_ADDR   "10.67.240.7"
#define TEST_MASK   "255.255.255.0"

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/* Delete a TCPIP$* name from LNM$SYSTEM (clean slate; ignore "not found"). */
static void deassign_system(const char *name)
{
    struct dsc$descriptor_s td = tcpip_cfg_dsc_(TCPIP_LNM_TABLE);
    struct dsc$descriptor_s nd = tcpip_cfg_dsc_(name);
    (void)sys$dellnm(&td, &nd, NULL);
}

/*
 * (a) No executive: the config engine's LNM$SYSTEM define/translate must fail
 * honestly with SS$_NOSUCHDEV -- never a per-process table (INV-6). This is the
 * config plane's own no-executive regression proof.
 */
static void run_no_executive(void)
{
    char val[64];
    uint32_t st;

    printf("  FAIL: cannot open /dev/vms\n");

    st = tcpip_cfg_define_system(TCPIP_LNM_INET_HOST, TEST_HOST);
    CHECK(st == SS$_NOSUCHDEV,
          "no executive: defining a TCPIP$ SYSTEM logical fails SS$_NOSUCHDEV, never a per-process fake");

    st = tcpip_cfg_translate_system(TCPIP_LNM_INET_HOST, val, sizeof(val));
    CHECK(st == SS$_NOSUCHDEV,
          "no executive: translating a TCPIP$ SYSTEM logical fails SS$_NOSUCHDEV, never a per-process fake");

    st = tcpip_cfg_configure(TEST_HOST, TEST_DOMAIN, NULL, NULL, NULL, NULL);
    CHECK(st == SS$_NOSUCHDEV,
          "no executive: tcpip_cfg_configure (VMS-way config) fails SS$_NOSUCHDEV as a whole, applying no fake");
}

/*
 * (b) With a real executive: configure the VMS way, then prove it round-trips
 * -- read every TCPIP$ logical back through the INDEPENDENT sys$trnlnm (same
 * executive arena, cross-process by construction) AND read the interface's own
 * address back from the kernel.
 */
static void run_configure_roundtrip(void)
{
    char val[64];
    uint32_t st;
    int syserr = 0;

    /* Clean slate: LNM$SYSTEM persists across suites in the same booted guest. */
    deassign_system(TCPIP_LNM_INET_HOST);
    deassign_system(TCPIP_LNM_INET_DOMAIN);
    deassign_system(TCPIP_LNM_INET_HOSTADDR);

    /* Configure the VMS way: apply the address to the interface over the
     * substrate AND record host/domain/address in the TCPIP$* SYSTEM logicals,
     * in ONE call -- exactly what TCPIP$CONFIG drives. */
    st = tcpip_cfg_configure(TEST_HOST, TEST_DOMAIN, TEST_IFACE, TEST_ADDR,
                             TEST_MASK, &syserr);
    CHECK(st & 1,
          "tcpip_cfg_configure applies the interface address and records the TCPIP$ logicals in one VMS-way call");
    if (!(st & 1))
        printf("  INFO: tcpip_cfg_configure returned status %u, errno %d\n",
               st, syserr);

    /* Round-trip 1: the local host name, read back through the PUBLIC
     * sys$trnlnm against LNM$SYSTEM -- the config engine's define and the
     * independent public translate see the SAME executive value. */
    st = tcpip_cfg_translate_system(TCPIP_LNM_INET_HOST, val, sizeof(val));
    CHECK((st & 1) && strcmp(val, TEST_HOST) == 0,
          "TCPIP$INET_HOST reads back the configured host name via sys$trnlnm");

    /* Round-trip 2: the domain. */
    st = tcpip_cfg_translate_system(TCPIP_LNM_INET_DOMAIN, val, sizeof(val));
    CHECK((st & 1) && strcmp(val, TEST_DOMAIN) == 0,
          "TCPIP$INET_DOMAIN reads back the configured domain via sys$trnlnm");

    /* Round-trip 3: the host address. THIS is the config-round-trip invariant
     * the negctl tcpip-config-hostaddr-not-defined targets. */
    st = tcpip_cfg_translate_system(TCPIP_LNM_INET_HOSTADDR, val, sizeof(val));
    /* negctl: tcpip-config-hostaddr-not-defined */
    CHECK((st & 1) && strcmp(val, TEST_ADDR) == 0,
          "TCPIP$INET_HOSTADDR reads back the configured host address via sys$trnlnm");

#if defined(__linux__)
    /* The interface really reflects it: the kernel's OWN address table
     * (SIOCGIFADDR) reports the address we applied -- not a per-process fake. */
    val[0] = '\0';
    st = tcpip_cfg_read_iface_addr(TEST_IFACE, val, sizeof(val));
    CHECK((st & 1) && strcmp(val, TEST_ADDR) == 0,
          "the interface itself reflects the configured address (SIOCGIFADDR over the real substrate)");
    if (!((st & 1) && strcmp(val, TEST_ADDR) == 0))
        printf("  INFO: interface %s address read back as '%s' (status %u)\n",
               TEST_IFACE, val, st);
#endif

    /* Reconfiguring supersedes: change the host name and the logical follows,
     * proving the config is writable, not write-once. */
    st = tcpip_cfg_define_system(TCPIP_LNM_INET_HOST, "OVMXNODE2");
    CHECK((st & 1) || st == SS$_SUPERSEDE,
          "reconfiguring TCPIP$INET_HOST supersedes the prior value");
    st = tcpip_cfg_translate_system(TCPIP_LNM_INET_HOST, val, sizeof(val));
    CHECK((st & 1) && strcmp(val, "OVMXNODE2") == 0,
          "the superseded TCPIP$INET_HOST reads back the new value");
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_tcpip_config (TCP/IP Services config plane, vms-67f) ===\n");

    if (!executive_present()) {
        run_no_executive();
        printf("=== test_syssvc_tcpip_config: %d passed, %d failed (SKIPPED: no /dev/vms -- executive-present scenarios not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    run_configure_roundtrip();

    printf("=== test_syssvc_tcpip_config: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
