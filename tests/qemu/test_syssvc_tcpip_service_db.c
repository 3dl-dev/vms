/*
 * test_syssvc_tcpip_service_db.c - the PERSISTENT INETD SERVICE DATABASE
 * management plane against a real /dev/vms (#878, vms-71b).
 *
 * ============================================================
 * WHAT THIS PROVES. An operator can manage TCPIP$INETD's service database THE
 * VMS WAY -- TCPIP {SET,ENABLE,DISABLE,DELETE} SERVICE -- and the change is
 * PERSISTED over the Files-11 ACP, so it survives reboot and is reapplied on
 * the next aux-server start. This drives the engine src/vmstcpip/mgmt/
 * tcpip_service_db.h (which those DCL verbs drive) directly, byte-exact,
 * against a real mounted ODS-2 volume.
 *
 * THE INV-6 CORE. The database is a genuine ACP file, and the proof that a
 * persisted change is REAL -- not a per-process fake -- is that it is read back
 * through the INDEPENDENT aux-server parser tcpip_inetd_parse_db() (the SAME
 * code TCPIP$INETD.EXE runs to decide what to bind), reading the persisted
 * bytes off the volume. A SET+ENABLEd service appears to the aux server; a
 * DISABLEd one vanishes from what it would bind (kept as a "!"-record); a
 * DELETEd one is gone. And with NO executive the persistent WRITE fails
 * TCPIP_SVCDB_ENOEXEC -- an honest error, never a table that reports success
 * while persisting nothing.
 *
 * SPEC ISOLATION (same rationale as test_syssvc_tcpip_config_acp.c): the
 * production DCL path targets SYS$SYSTEM:TCPIP$SERVICE.DAT, but the real-VAX
 * fixture disk's only writable directory is VDA0:[OVMXDIR], so this suite
 * drives the engine's `_at` forms against a fixture spec there -- exercising the
 * SAME parse/format/upsert/store logic byte-exact. The production SYS$SYSTEM:
 * path is proven end to end by the cold-boot e2e.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * anchored by tcpip-svcdb-enable-not-persisted, which forces a NEWLY-defined
 * service's enable flag off (SET SERVICE /ENABLE on a fresh service persists it
 * DISABLED). The store still succeeds, the record is still persisted, and the
 * separate ENABLE SERVICE verb (step 3) and the re-SET of an already-enabled
 * service (step 5) are unaffected -- so ONLY the one assertion that a fresh
 * SET SERVICE /ENABLE is visible to the aux server reddens: the exact
 * enablement-on-create-not-persisted regression the plane exists to prevent.
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

#include "../../src/vmstcpip/mgmt/tcpip_service_db.h"   /* the engine under test */
#include "../../src/vmstcpip/services/tcpip_inetd.h"    /* the INDEPENDENT consumer parser */

#define EXIT_SKIP    77
#define ODS2_UNIT    "VDA0:"
#define SVC_SPEC     "VDA0:[OVMXDIR]TCPIP$SERVICE.DAT"

#define DAY_IMG      "SYS$SYSTEM:TCPIP$DAYTIME.EXE"
#define DAY_USER     "TCPIP$DAYTIME"
#define ECHO_IMG     "SYS$SYSTEM:TCPIP$ECHO.EXE"

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

/* Slurp the whole database file `spec` over the ACP into `buf` (one record per
 * line, LF-separated) the VMS way. Returns 0 on success, -1 if unopenable. */
static int slurp_db(const char *spec, char *buf, size_t bufsz)
{
    rms_textfile_t *tf = rms_textfile_open(spec);
    char line[512];
    size_t used = 0;

    if (!tf) return -1;
    buf[0] = '\0';
    while (rms_textfile_getline(tf, line, sizeof(line), NULL) == 1) {
        size_t ll = strlen(line);
        if (used + ll + 2 >= bufsz) break;
        memcpy(buf + used, line, ll);
        used += ll;
        buf[used++] = '\n';
        buf[used] = '\0';
    }
    rms_textfile_close(tf);
    return 0;
}

/* Does the aux-server parser see an ENABLED service `name` on the persisted
 * bytes, and (if found) with the expected port/user/image? Returns 1 if the
 * consumer would bind exactly this service correctly, 0 otherwise. */
static int consumer_sees(const char *name, uint16_t port,
                         const char *user, const char *image)
{
    char text[4096];
    struct tcpip_service svcs[TCPIP_INETD_MAX_SERVICES];
    int n, hits = 0, ok = 0;

    if (slurp_db(SVC_SPEC, text, sizeof(text)) != 0)
        return 0;
    n = tcpip_inetd_parse_db(text, svcs, TCPIP_INETD_MAX_SERVICES);
    for (int i = 0; i < n; i++) {
        if (strcmp(svcs[i].name, name) != 0)
            continue;
        hits++;
        ok = (svcs[i].port == port &&
              (!user || strcmp(svcs[i].user, user) == 0) &&
              (!image || strcmp(svcs[i].image, image) == 0));
    }
    return (hits == 1 && ok);   /* exactly one, correctly parsed */
}

/* How many times the aux-server parser sees an enabled service `name`. */
static int consumer_count(const char *name)
{
    char text[4096];
    struct tcpip_service svcs[TCPIP_INETD_MAX_SERVICES];
    int n, hits = 0;

    if (slurp_db(SVC_SPEC, text, sizeof(text)) != 0)
        return 0;
    n = tcpip_inetd_parse_db(text, svcs, TCPIP_INETD_MAX_SERVICES);
    for (int i = 0; i < n; i++)
        if (strcmp(svcs[i].name, name) == 0)
            hits++;
    return hits;
}

/* No executive: the persistent WRITE cannot fabricate success (INV-6). */
static void run_no_executive(void)
{
    int rc = tcpip_svcdb_set_at(SVC_SPEC, "DAYTIME", 13, DAY_USER, DAY_IMG,
                                NULL, TCPIP_SVC_ENABLE);
    check(rc == TCPIP_SVCDB_ENOEXEC,
          "no executive: SET SERVICE fails TCPIP_SVCDB_ENOEXEC (nothing persisted), never a fake success");
}

static void run_roundtrip(void)
{
    uint32_t st;
    int rc;
    struct tcpip_svcdb_rec recs[TCPIP_SVCDB_MAX];

    st = vms_kif_acp_mount(ODS2_UNIT);        /* idempotent */
    check($VMS_STATUS_SUCCESS(st), "VDA0: mounted executive-global for the ACP");
    erase_file(SVC_SPEC);

    /* (1) SET SERVICE ... /ENABLE persists an enabled record the aux server binds. */
    rc = tcpip_svcdb_set_at(SVC_SPEC, "DAYTIME", 13, DAY_USER, DAY_IMG,
                            NULL, TCPIP_SVC_ENABLE);
    check(rc == TCPIP_SVCDB_OK, "SET SERVICE DAYTIME /ENABLE persists over the ACP");
    /* negctl: tcpip-svcdb-enable-not-persisted */
    check(consumer_sees("DAYTIME", 13, DAY_USER, DAY_IMG),
          "the aux-server parser sees the ENABLED DAYTIME (port/user/image byte-exact) on the persisted bytes");

    /* (2) A new service without /ENABLE is DEFINED but DISABLED (posture): the
     *     record is retained but the aux server does NOT bind it. */
    rc = tcpip_svcdb_set_at(SVC_SPEC, "ECHO", 7, NULL, ECHO_IMG, NULL, TCPIP_SVC_KEEP);
    check(rc == TCPIP_SVCDB_OK, "SET SERVICE ECHO (no /ENABLE) persists");
    check(consumer_count("ECHO") == 0, "a defined-but-disabled service is NOT bound by the aux server (posture-safe)");
    {
        int n = tcpip_svcdb_load_at(SVC_SPEC, recs, TCPIP_SVCDB_MAX);
        check(tcpip_svcdb_find(recs, n, "ECHO") >= 0,
              "...but the disabled ECHO record IS retained in the database (the \"!\"-record)");
    }

    /* (3) ENABLE SERVICE flips it on and the aux server now binds it. */
    rc = tcpip_svcdb_enable_at(SVC_SPEC, "ECHO", 1);
    check(rc == TCPIP_SVCDB_OK, "ENABLE SERVICE ECHO persists");
    check(consumer_count("ECHO") == 1, "the aux-server parser now sees the ENABLED ECHO");

    /* (4) DISABLE SERVICE removes it from what the aux server binds, record kept. */
    rc = tcpip_svcdb_enable_at(SVC_SPEC, "DAYTIME", 0);
    check(rc == TCPIP_SVCDB_OK, "DISABLE SERVICE DAYTIME persists");
    check(consumer_count("DAYTIME") == 0, "the aux server no longer binds the DISABLED DAYTIME");
    {
        int n = tcpip_svcdb_load_at(SVC_SPEC, recs, TCPIP_SVCDB_MAX);
        int i = tcpip_svcdb_find(recs, n, "DAYTIME");
        check(i >= 0 && recs[i].enabled == 0,
              "...but the DAYTIME record is retained, marked disabled");
    }

    /* (5) Re-SET an existing service SUPERSEDES (no duplicate record). */
    rc = tcpip_svcdb_set_at(SVC_SPEC, "ECHO", 7, NULL, ECHO_IMG, NULL, TCPIP_SVC_KEEP);
    check(rc == TCPIP_SVCDB_OK && consumer_count("ECHO") == 1,
          "re-SET of an existing service updates in place -- exactly one ECHO, never a duplicate");

    /* (6) DELETE SERVICE drops the record entirely. */
    rc = tcpip_svcdb_delete_at(SVC_SPEC, "ECHO");
    check(rc == TCPIP_SVCDB_OK, "DELETE SERVICE ECHO persists");
    {
        int n = tcpip_svcdb_load_at(SVC_SPEC, recs, TCPIP_SVCDB_MAX);
        check(tcpip_svcdb_find(recs, n, "ECHO") < 0 && consumer_count("ECHO") == 0,
              "the deleted ECHO is gone from the database and from the aux-server view");
    }
    rc = tcpip_svcdb_enable_at(SVC_SPEC, "ECHO", 1);
    check(rc == TCPIP_SVCDB_ENOSUCH, "ENABLE of a non-existent service reports NOSUCH, not a fake");

    /* (7) It IS the ODS-2 volume: dismount => reads fail honestly AND the
     *     persistent write fails ENOEXEC (never a per-process fake). */
    vms_kif_acp_dmount(ODS2_UNIT);
    check(rms_textfile_open(SVC_SPEC) == NULL,
          "after DISMOUNT the service database is unreadable -- it lived on the real ODS-2 volume");
    rc = tcpip_svcdb_set_at(SVC_SPEC, "DAYTIME", 13, DAY_USER, DAY_IMG, NULL, TCPIP_SVC_ENABLE);
    check(rc == TCPIP_SVCDB_ENOEXEC,
          "with the volume dismounted SET SERVICE fails ENOEXEC -- no fabricated persistence (INV-6)");

    /* Clean slate for any co-resident suite (remount + erase). */
    (void)vms_kif_acp_mount(ODS2_UNIT);
    erase_file(SVC_SPEC);
    vms_kif_acp_dmount(ODS2_UNIT);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_tcpip_service_db (persistent INETD service database, #878 vms-71b) ===\n");

    if (!executive_present()) {
        run_no_executive();
        printf("=== test_syssvc_tcpip_service_db: %d passed, %d failed "
               "(SKIPPED: no /dev/vms -- ACP round-trip not exercised) ===\n", pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    run_roundtrip();
    printf("=== test_syssvc_tcpip_service_db: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
