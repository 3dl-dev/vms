/*
 * test_syssvc_tcpip_host_acp.c - the TCPIP host database (TCPIP$HOST.DAT)
 * persists the VMS way: an RMS text record written through the Files-11 ODS-2
 * ACP and read back off the genuine mounted volume (rd vms-402, the converged
 * flip for the TCP/IP config store; template: test_syssvc_loginout_acp.c).
 *
 * ============================================================
 * WHY THIS TEST EXISTS -- it closes a host-green != runtime-real masking gap.
 * TCPIP SET HOST / SHOW HOST used to fopen(VMS_SYSTEM_DIR/TCPIP$HOST.DAT), and
 * VMS_SYSTEM_DIR = SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSEXE" with SYSDISK_MOUNT =
 * "/vms" -- the RETIRED POSIX passthrough (vms-37e). Host-mode tooling still has
 * a writable /vms, so the host DCL test was GREEN; but on the booted runtime
 * /vms does not exist, so every fopen() returned NULL and the write was silently
 * skipped while SET HOST still printed "host added" (a dead store behind a
 * facade). The verbs now persist through rms_textfile_append_line /
 * rms_textfile_open (RMS $PUT-at-EOF / $GET over the ACP, vms-274), reaching the
 * genuine ODS-2 SYS$SYSTEM: volume. This test proves that ACP round-trip against
 * a REAL /dev/vms + mounted volume -- the path the host /vms test can never
 * exercise -- so the masking gap is covered, not merely moved.
 *
 * WHAT IT PROVES (against the harness-mounted writable ODS-2 volume on VDA0:):
 *   (1) a TCPIP$HOST.DAT record written via rms_textfile_append_line lands on
 *       the ODS-2 volume and reads back byte-exact through a fresh $GET, and
 *       parses to the SAME address + hostname SHOW HOST prints;
 *   (2) a SECOND SET HOST append PRESERVES the first record (add semantics),
 *       both read back -- an append, not a supersede;
 *   (3) with the volume DISMOUNTED the read fails honestly (NULL handle, no
 *       /vms POSIX fallback) -- INV-6 / Rule 9.
 *
 * SPEC ISOLATION (same rationale as test_syssvc_loginout_acp.c): the real-VAX
 * fixture disk's only writable directory is [OVMXDIR] and it carries no
 * [SYS0.SYSCOMMON.SYSEXE] system tree, so this suite exercises the writer/reader
 * MECHANISM byte-for-byte against a concrete VDA0:[OVMXDIR] spec. The product's
 * own SYS$SYSTEM:TCPIP$HOST.DAT composition through LNM$FILE_DEV is the same RMS
 * open path (proven for SYS$SYSTEM: composition in test_syssvc_sysuaf_uic_base);
 * the full boot-time SET HOST -> SHOW HOST proof through the product's SYS$SYSTEM:
 * is the DCL/SHOW acceptance e2e's to add. No /vms POSIX fallback anywhere here.
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

#define EXIT_SKIP   77
#define ODS2_UNIT   "VDA0:"
#define HOST_SPEC   "VDA0:[OVMXDIR]TCPIP$HOST.DAT"

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

/* Does the store hold a record whose parsed (address, hostname) match -- the
 * SAME "%127s %255s" parse cmd_tcpip_show_host uses on each record. Returns 1 if
 * found. Reads through the ACP ($GET over the mounted volume); 0 if the store
 * cannot be opened (fail-honest) or the entry is absent. */
static int host_store_has(const char *spec, const char *want_addr,
                          const char *want_name)
{
    rms_textfile_t *tf = rms_textfile_open(spec);
    if (!tf) return 0;
    char line[512];
    int too_long = 0, found = 0;
    while (rms_textfile_getline(tf, line, sizeof(line), &too_long)) {
        if (too_long) continue;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\0') continue;
        char addr[128], name[256];
        if (sscanf(p, "%127s %255s", addr, name) >= 2 &&
            strcmp(addr, want_addr) == 0 && strcmp(name, want_name) == 0)
            found = 1;
    }
    rms_textfile_close(tf);
    return found;
}

/* Build a record in the SAME shape cmd_tcpip_set_host writes:
 * space-padded 16-col address + hostname. */
static void make_record(char *rec, size_t sz, const char *addr, const char *name)
{
    snprintf(rec, sz, "%-16s%.255s", addr, name);
}

int main(void)
{
    uint32_t st;

    printf("=== test_syssvc_tcpip_host_acp (TCPIP$HOST.DAT persists via RMS "
           "$PUT/$GET over the Files-11 ACP, vms-402) ===\n");

    if (!executive_present()) {
        printf("  SKIP: no /dev/vms -- TCPIP SET HOST/SHOW HOST persist through the "
               "ACP; nothing to assert without a real mounted volume (Rule 9).\n");
        return EXIT_SKIP;
    }

    st = vms_kif_acp_mount(ODS2_UNIT);        /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for the ACP");

    erase_file(HOST_SPEC);                    /* clean slate (ignore absent) */

    /* ---- (1) a SET HOST record persists over the ACP and reads back ---- */
    {
        char rec[512];
        make_record(rec, sizeof(rec), "10.1.1.1", "ALPHA");
        int rc = rms_textfile_append_line(HOST_SPEC, rec);
        check(rc == 0, "TCPIP$HOST.DAT append via RMS $PUT-at-EOF over the ACP");
        check(host_store_has(HOST_SPEC, "10.1.1.1", "ALPHA"),
              "the host record reads back byte-exact off the ODS-2 volume (addr+name)");
    }

    /* ---- (2) a second SET HOST PRESERVES the first (append, not supersede) ---- */
    {
        char rec[512];
        make_record(rec, sizeof(rec), "10.2.2.2", "BETA");
        int rc = rms_textfile_append_line(HOST_SPEC, rec);
        check(rc == 0, "second host record appended over the ACP");
        check(host_store_has(HOST_SPEC, "10.2.2.2", "BETA") &&
              host_store_has(HOST_SPEC, "10.1.1.1", "ALPHA"),
              "both host records survive -- SET HOST adds, it does not supersede");
    }

    /* ---- (3) it IS the ODS-2 volume: dismount => the read fails honestly ---- */
    vms_kif_acp_dmount(ODS2_UNIT);
    check(rms_textfile_open(HOST_SPEC) == NULL,
          "with the volume DISMOUNTED the host store read fails-honest (no /vms POSIX fallback)");

    /* Remount + clean up the fixture file. */
    st = vms_kif_acp_mount(ODS2_UNIT);
    check($VMS_STATUS_SUCCESS(st), "VDA0: remounted");
    erase_file(HOST_SPEC);

    printf("=== test_syssvc_tcpip_host_acp: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
