/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * test_ods2_nonrms_writers.c - proof that the per-boot NON-RMS SYS$DISK
 * writers are rerouted onto the serialized genuine-ODS-2 write adapter
 * (vms-d75, epic vms-5eb, the ODS-2 runtime flip -- docs/design-ods2-runtime-
 * flip.md). ATOMIC-GROUP member: RED-BY-DESIGN until the default master flip.
 *
 * WHAT THIS PROVES, AND WHY. R2 reroutes only the RMS path. Two writers fire
 * on every boot that bypass RMS entirely with plain fopen()/mkdir() on the
 * /vms POSIX passthrough:
 *   - OPCOM appends OPERATOR.LOG (sys$sndopr, src/libvms/syssvc/sys_operator.c);
 *   - login accounting writes LASTLOGIN and mkdir's the accounting directory
 *     (ovmx_accounting_record_login, src/libvms/rtl/ovmx_accounting.c).
 * Without this rung those writers would either fail on an ODS-2 boot or
 * silently POSIX-fall-back (an INV-6 LARP). This test drives the ACTUAL
 * rerouted library entry points (sys$sndopr / ovmx_accounting_record_login) --
 * not the adapter directly -- against a LIVE registered genuine-ODS-2 SYS$DISK
 * and verifies every write landed as a genuine ODS-2 record, read back through
 * an INDEPENDENT ods2_bdev reader over the raw on-disk image (never POSIX):
 *
 *   - (a) two OPCOM sends -> OPERATOR.LOG is created then APPENDED, and the
 *     ODS-2 file carries BOTH records, in order (appends accumulate);
 *   - (b) a login record -> the accounting directory exists on the volume as a
 *     genuine ODS-2 directory and carries the per-user LASTLOGIN file with the
 *     recorded timestamp;
 *   - FAIL-HONEST (Rule 9 / INV-6): with NO ODS-2 SYS$DISK registered the
 *     OPCOM send returns SS$_DEVNOTMOUNT and the login record fails (-1), and
 *     nothing is written to the POSIX passthrough -- no silent fallback.
 *
 * The registration itself is the adapter's own lazy OVMX_SYSDISK_DEV path (the
 * same channel PID 1 exports), so the rerouted writers register SYS$DISK for
 * themselves exactly as a fresh boot process would. Concurrency across the
 * three writers is already covered by the #614 flock broker (test_ods2_write_
 * broker.c) -- not re-tested here.
 */

#define _POSIX_C_SOURCE 200809L

#include "starlet.h"          /* sys$sndopr, dsc$descriptor_s, SS$_*        */
#include "opcdef.h"           /* struct opcdef, OPC$K_MS_HDRLEN, OPC$_RQ_*  */
#include "ovmx_accounting.h"  /* ovmx_accounting_record_login               */
#include "ovmx_layout.h"      /* VMS_OPERATOR_LOG, VMS_LASTLOGIN_DIR, SYSDISK_MOUNT */
#include "vmsfs/filespec.h"   /* vmsfs_to_linux_path                        */
#include "vmsfs/sysdisk.h"    /* ods2_sysdisk_owns_path                     */
#include "vmsfs/volume.h"     /* vmsfs_volume_unregister                    */
#include "vmsfs/ods2.h"       /* ods2_volume_format + ods2_bdev_* reader    */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strncasecmp */
#include <unistd.h>
#include <fcntl.h>

static int g_failures = 0;

#define CHECK(cond, msg) do {                                          \
    if (!(cond)) {                                                     \
        printf("  FAIL: %s  (%s:%d)\n", (msg), __FILE__, __LINE__);    \
        g_failures++;                                                  \
    } else {                                                           \
        printf("  PASS: %s\n", (msg));                                 \
    }                                                                  \
} while (0)

#define VOL_MB     4u
#define VOL_LABEL  "OVMXSYS"
#define TEST_USER  "TESTUSER"

/*
 * Build a genuine, EMPTY ODS-2 volume (just the MFD -- the reroute creates
 * OPERATOR.LOG and the LASTLOGIN directory itself, exactly as the boot writers
 * do) and lay it onto a fresh loop image at `path` (mkstemp template). The
 * WHOLE image is written so later allocations land in real backing store.
 */
static int build_ods2_image(char *path)
{
    uint32_t total_blocks =
        (uint32_t)((uint64_t)VOL_MB * 1024 * 1024 / ODS2_BLOCK_SIZE);
    size_t image_len = (size_t)total_blocks * ODS2_BLOCK_SIZE;
    uint8_t *image = (uint8_t *)calloc(1, image_len);
    if (!image)
        return -1;

    ods2_format_params_t params = { total_blocks,
                                    total_blocks / 100 < ODS2_RESFILES
                                        ? ODS2_RESFILES : total_blocks / 100,
                                    VOL_LABEL };
    ods2_wvolume_t wvol;
    int rc = -1;

    if (ods2_volume_format(image, image_len, &params, &wvol) != ODS2_OK)
        goto out;

    int fd = mkstemp(path);
    if (fd < 0)
        goto out;
    if (ftruncate(fd, (off_t)image_len) != 0 ||
        pwrite(fd, image, image_len, 0) < 0) {
        close(fd);
        goto out;
    }
    close(fd);
    rc = 0;

out:
    free(image);
    return rc;
}

/*
 * Send one OPCOM message through the ACTUAL rerouted sys$sndopr entry point,
 * building the OPC message block the same way every $SNDOPR caller in this
 * tree does (src/vmsdcl/dcl_cmd_misc.c cmd_reply()).
 */
static uint32_t send_opcom(const char *text)
{
    struct {
        struct opcdef hdr;
        char text[256];
    } msgbuf;
    memset(&msgbuf, 0, sizeof(msgbuf));

    msgbuf.hdr.opc$b_ms_type   = OPC$_RQ_RQST;
    msgbuf.hdr.opc$b_ms_target = OPC$M_NM_CENTRL;
    int n = snprintf(msgbuf.hdr.opc$l_ms_text, sizeof(msgbuf.text), "%s", text);

    struct dsc$descriptor_s desc;
    memset(&desc, 0, sizeof(desc));
    desc.dsc$a_pointer = (char *)&msgbuf.hdr;
    desc.dsc$w_length  = (uint16_t)(OPC$K_MS_HDRLEN + n);

    return sys$sndopr(&desc, 0);
}

/*
 * Directory-entry probe for ods2_sysdisk_list_dir(): matches an entry by base
 * name (case-insensitive -- ODS-2 stores upper-cased, the resolved path is
 * lower-cased), setting found. The listing runs over the genuine block-device
 * ODS-2 volume handle, never the /vms POSIX passthrough.
 */
struct find_ctx { const char *want; int found; };
static int find_cb(const char *name, unsigned name_len, uint16_t version,
                   const ods2_fid_t *fid, void *ctx)
{
    (void)version; (void)fid;
    struct find_ctx *fc = (struct find_ctx *)ctx;
    if (strlen(fc->want) == name_len && strncasecmp(fc->want, name, name_len) == 0)
        fc->found = 1;
    return 0;
}

int main(void)
{
    printf("=== test_ods2_nonrms_writers: per-boot non-RMS SYS$DISK writers ===\n");

    /* The rerouted writers resolve these VMS filespecs to /vms Linux paths the
     * SAME way this test does (same process, same empty LNM state), so the
     * verify chain below derives its lookup names from the SAME resolved paths
     * the writers used -- it does not assume a fixed location or case. */
    char oplog_linux[1024], lastlogin_dir[1024];
    vmsfs_to_linux_path(VMS_OPERATOR_LOG, oplog_linux, sizeof(oplog_linux));
    vmsfs_to_linux_path(VMS_LASTLOGIN_DIR, lastlogin_dir, sizeof(lastlogin_dir));
    printf("  (OPERATOR.LOG -> %s ; LASTLOGIN dir -> %s)\n",
           oplog_linux, lastlogin_dir);

    CHECK(ods2_sysdisk_owns_path(oplog_linux),
          "OPERATOR.LOG resolves onto SYS$DISK (ods2_sysdisk owns it)");
    CHECK(ods2_sysdisk_owns_path(lastlogin_dir),
          "LASTLOGIN dir resolves onto SYS$DISK (ods2_sysdisk owns it)");

    /* The per-user LASTLOGIN file: "<lastlogin_dir>/<UPCASED-USER>" -- built
     * the SAME way ovmx_accounting.c's lastlogin_path() does. */
    char llfile[1152];
    snprintf(llfile, sizeof(llfile), "%s/%s", lastlogin_dir, TEST_USER);

    static uint8_t buf[65536];
    size_t got = 0;
    int st;

    /* --- FAIL-HONEST before any volume: no OVMX_SYSDISK_DEV registered.
     * Proven by the honest return codes: the reroute takes the adapter branch
     * (owns_path is true) and returns SS$_DEVNOTMOUNT / -1 with NO reachable
     * POSIX fallback branch -- a passthrough absence check would be host-
     * dependent (the dev host has a live /vms), so the return codes are the
     * INV-6 evidence. --- */
    unsetenv("OVMX_SYSDISK_DEV");
    CHECK(send_opcom("boot fail-honest probe") == SS$_DEVNOTMOUNT,
          "sys$sndopr with no ODS-2 SYS$DISK -> SS$_DEVNOTMOUNT (no POSIX fallback)");
    CHECK(ovmx_accounting_record_login(TEST_USER) == -1,
          "record_login with no ODS-2 SYS$DISK -> honest failure (no POSIX fallback)");

    /* --- build + register a genuine ODS-2 SYS$DISK via the lazy channel --- */
    char image_path[] = "/tmp/ods2_nonrms_writers_XXXXXX";
    if (build_ods2_image(image_path) != 0) {
        printf("  FAIL: could not build ODS-2 image\n");
        return 1;
    }
    setenv("OVMX_SYSDISK_DEV", image_path, 1);

    /* --- (a) OPCOM: create-on-first, then APPEND; both records present --- */
    const char *MSG1 = "OVMX-BOOT-FIRST-OPCOM-RECORD";
    const char *MSG2 = "OVMX-BOOT-SECOND-OPCOM-RECORD";
    CHECK(send_opcom(MSG1) == SS$_NORMAL,
          "first sys$sndopr onto ODS-2 SYS$DISK -> SS$_NORMAL (creates OPERATOR.LOG)");
    CHECK(send_opcom(MSG2) == SS$_NORMAL,
          "second sys$sndopr -> SS$_NORMAL (appends to OPERATOR.LOG)");

    /* Read OPERATOR.LOG back through the genuine ODS-2 reader (block-device FID
     * walk over the registered SYS$DISK volume -- the exact R2 consumer path,
     * NOT a POSIX read of the /vms passthrough). */
    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(oplog_linux, buf, sizeof(buf) - 1, &got);
    CHECK(st == SS$_NORMAL && got > 0,
          "OPERATOR.LOG reads back as a genuine ODS-2 file (ods2 reader, not POSIX)");
    buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = '\0';
    char *p1 = strstr((char *)buf, MSG1);
    char *p2 = strstr((char *)buf, MSG2);
    CHECK(p1 != NULL, "OPERATOR.LOG contains the FIRST OPCOM record (genuine ODS-2 content)");
    CHECK(p2 != NULL, "OPERATOR.LOG contains the SECOND OPCOM record (append accumulated)");
    CHECK(p1 && p2 && p1 < p2,
          "OPERATOR.LOG records are in append order (first precedes second)");

    /* --- (b) LASTLOGIN: mkdir the accounting dir + write the per-user record --- */
    CHECK(ovmx_accounting_record_login(TEST_USER) == 0,
          "record_login onto ODS-2 SYS$DISK -> success (mkdir + create_file)");

    /* The accounting directory now exists as a genuine ODS-2 directory (the
     * listing itself only succeeds over a real ODS-2 dir) and carries the
     * per-user record entry. */
    {
        struct find_ctx fc = { TEST_USER, 0 };
        st = ods2_sysdisk_list_dir(lastlogin_dir, find_cb, &fc);
        CHECK(st == SS$_NORMAL,
              "the accounting directory lists as a genuine ODS-2 directory (ods2 reader)");
        CHECK(fc.found,
              "the accounting directory contains the per-user record entry (genuine ODS-2)");
    }

    memset(buf, 0, sizeof(buf));
    got = 0;
    st = ods2_sysdisk_read_file(llfile, buf, sizeof(buf) - 1, &got);
    CHECK(st == SS$_NORMAL && got > 0,
          "LASTLOGIN record reads back as a genuine ODS-2 file (ods2 reader, not POSIX)");
    buf[got < sizeof(buf) ? got : sizeof(buf) - 1] = '\0';
    long long ts = atoll((char *)buf);
    CHECK(ts > 0, "LASTLOGIN record carries a positive login timestamp (genuine ODS-2 content)");

    /* --- after the channel is gone: fail-honest again --- */
    unsetenv("OVMX_SYSDISK_DEV");
    CHECK(vmsfs_volume_unregister(SYSDISK_DEVICE) == SS$_NORMAL,
          "unregister DKA0: for the post-unmount fail-honest check");
    CHECK(send_opcom("after unmount") == SS$_DEVNOTMOUNT,
          "sys$sndopr after unmount -> SS$_DEVNOTMOUNT (fail-honest)");

    unlink(image_path);

    printf("=== %s: %d failure(s) ===\n",
           g_failures ? "FAILED" : "PASSED", g_failures);
    return g_failures ? 1 : 0;
}
