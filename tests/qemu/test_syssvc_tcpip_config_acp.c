/*
 * test_syssvc_tcpip_config_acp.c - the TCP/IP config stores (TCPIP$ROUTE.DAT,
 * TCPIP$INTERFACE.DAT, TCPIP$NAMESERVICE.DAT) persist the VMS way: RMS text
 * records written through the Files-11 ODS-2 ACP and read back off the genuine
 * mounted volume (rd vms-210, P1 of the config-persistence-reapply tree vms-a0b2;
 * the sibling of vms-402's TCPIP$HOST.DAT flip; template:
 * test_syssvc_loginout_acp.c / test_syssvc_tcpip_host_acp.c).
 *
 * ============================================================
 * WHY THIS TEST EXISTS. TCPIP SET ROUTE / SET INTERFACE / SET NAME_SERVICE used
 * to fopen(VMS_SYSTEM_DIR/TCPIP$*.DAT), and VMS_SYSTEM_DIR = SYSDISK_MOUNT
 * "/SYS0/SYSCOMMON/SYSEXE" with SYSDISK_MOUNT = "/vms" -- the RETIRED POSIX
 * passthrough (vms-37e). Host-mode tooling still has a writable /vms; the booted
 * runtime does NOT, so those writes silently persisted nothing. The verbs now
 * persist through rms_textfile (RMS $PUT-at-EOF / $CREATE+$PUT over the ACP,
 * vms-274), reaching the genuine ODS-2 SYS$SYSTEM: volume. This test proves that
 * ACP round-trip against a REAL /dev/vms + mounted volume -- the path the host
 * /vms test could never exercise.
 *
 * WHAT IT PROVES (against the harness-mounted writable ODS-2 volume on VDA0:):
 *   (1) ROUTE.DAT append (rms_textfile_append_line): two SET ROUTE records land
 *       and BOTH read back -- append, not supersede;
 *   (2) INTERFACE.DAT append: a SET INTERFACE record lands and reads back;
 *   (3) NAMESERVICE.DAT supersede (rms_textfile_write_line): SET NAME_SERVICE
 *       rewrites the whole record -- a second write REPLACES the first (only the
 *       latest SERVER survives), and a DOMAIN append rides after the SERVER line;
 *   (4) with the volume DISMOUNTED, the read fails honestly (NULL handle, no
 *       /vms POSIX fallback) -- INV-6 / Rule 9.
 *
 * SPEC ISOLATION (same rationale as test_syssvc_loginout_acp.c /
 * test_syssvc_tcpip_host_acp.c): the real-VAX fixture disk's only writable
 * directory is [OVMXDIR] and it carries no [SYS0.SYSCOMMON.SYSEXE] system tree,
 * so this suite exercises the writer/reader MECHANISM against concrete
 * VDA0:[OVMXDIR] specs. The product's own SYS$SYSTEM: composition via
 * LNM$FILE_DEV is the same RMS open path. No /vms POSIX fallback anywhere.
 * ============================================================
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "rmsdef.h"
#include "vms_kif.h"
#include "rms/rms.h"
#include "rms_textfile.h"

#define EXIT_SKIP    77
#define ODS2_UNIT    "VDA0:"
#define ROUTE_SPEC   "VDA0:[OVMXDIR]TCPIP$ROUTE.DAT"
#define IF_SPEC      "VDA0:[OVMXDIR]TCPIP$INTERFACE.DAT"
#define NS_SPEC      "VDA0:[OVMXDIR]TCPIP$NAMESERVICE.DAT"

static int pass = 0, fail = 0;
static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

static void erase_file(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    sys$erase(&fab, 0, 0);
}

/* Read every record of `spec` over the ACP; count how many contain `needle`.
 * Returns -1 if the file cannot be opened (fail-honest). */
static int count_records_with(const char *spec, const char *needle)
{
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) return -1;
    char line[512];
    int too_long = 0, n = 0;
    while (rms_textfile_getline(tf, line, sizeof(line), &too_long)) {
        if (too_long) continue;
        if (strstr(line, needle) != NULL) n++;
    }
    rms_textfile_close(tf);
    return n;
}

/* Total record count of `spec` (for the supersede assertion). -1 if unopenable. */
static int total_records(const char *spec)
{
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) return -1;
    char line[512];
    int too_long = 0, n = 0;
    while (rms_textfile_getline(tf, line, sizeof(line), &too_long))
        n++;
    rms_textfile_close(tf);
    return n;
}

int main(void)
{
    uint32_t st;

    printf("=== test_syssvc_tcpip_config_acp (TCPIP$ROUTE/INTERFACE/NAMESERVICE.DAT "
           "persist via RMS over the Files-11 ACP, vms-210) ===\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms -- the TCP/IP config stores persist through the "
               "ACP; nothing to assert without a real mounted volume (Rule 9).\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(ODS2_UNIT);        /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for the ACP");

    erase_file(ROUTE_SPEC);
    erase_file(IF_SPEC);
    erase_file(NS_SPEC);

    /* ---- (1) ROUTE.DAT: append preserves prior routes ---- */
    {
        int rc1 = rms_textfile_append_line(ROUTE_SPEC, "DEFAULT 10.0.2.2");
        int rc2 = rms_textfile_append_line(ROUTE_SPEC, "192.168.0.0 10.0.2.9 255.255.0.0");
        check(rc1 == 0 && rc2 == 0, "two SET ROUTE records appended to TCPIP$ROUTE.DAT over the ACP");
        check(count_records_with(ROUTE_SPEC, "DEFAULT 10.0.2.2") == 1 &&
              count_records_with(ROUTE_SPEC, "192.168.0.0 10.0.2.9 255.255.0.0") == 1,
              "both route records read back off the ODS-2 volume -- append, not supersede");
    }

    /* ---- (2) INTERFACE.DAT: a SET INTERFACE record lands and reads back ---- */
    {
        int rc = rms_textfile_append_line(IF_SPEC, "SE0 10.0.2.15 255.255.255.0");
        check(rc == 0, "SET INTERFACE record appended to TCPIP$INTERFACE.DAT over the ACP");
        check(count_records_with(IF_SPEC, "SE0 10.0.2.15 255.255.255.0") == 1,
              "the interface record reads back off the ODS-2 volume");
    }

    /* ---- (3) NAMESERVICE.DAT: write_line SUPERSEDES (SET NAME_SERVICE semantics) ---- */
    {
        int rc1 = rms_textfile_write_line(NS_SPEC, "SERVER=10.0.2.3");
        check(rc1 == 0 && count_records_with(NS_SPEC, "SERVER=10.0.2.3") == 1,
              "first SET NAME_SERVICE record written to TCPIP$NAMESERVICE.DAT");
        int rc2 = rms_textfile_write_line(NS_SPEC, "SERVER=10.0.2.4");
        check(rc2 == 0 &&
              count_records_with(NS_SPEC, "SERVER=10.0.2.4") == 1 &&
              count_records_with(NS_SPEC, "SERVER=10.0.2.3") == 0 &&
              total_records(NS_SPEC) == 1,
              "a second SET NAME_SERVICE SUPERSEDES the first (only the latest SERVER survives)");
        int rc3 = rms_textfile_append_line(NS_SPEC, "DOMAIN=ovmx.local");
        check(rc3 == 0 &&
              count_records_with(NS_SPEC, "SERVER=10.0.2.4") == 1 &&
              count_records_with(NS_SPEC, "DOMAIN=ovmx.local") == 1,
              "the DOMAIN record appends after the SERVER record");
    }

    /* ---- (4) it IS the ODS-2 volume: dismount => the reads fail honestly ---- */
    vms_kif_acp_dmount(ODS2_UNIT);
    check(rms_textfile_open(ROUTE_SPEC) == NULL &&
          rms_textfile_open(IF_SPEC) == NULL &&
          rms_textfile_open(NS_SPEC) == NULL,
          "with the volume DISMOUNTED the config-store reads fail-honest (no /vms POSIX fallback)");

    /* Remount + clean up the fixture files. */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "VDA0: remounted");
    erase_file(ROUTE_SPEC);
    erase_file(IF_SPEC);
    erase_file(NS_SPEC);

    printf("=== test_syssvc_tcpip_config_acp: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
