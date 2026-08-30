/*
 * test_syssvc_rms_scratch_create.c - SYSTEM's sys$create() + $PUT of an RMS
 * INDEXED file (src/vmsrms/rms_core.c + rms_idx.c + rms_prolog3.c) over the
 * Files-11 (ODS-2) ACP, and a byte-for-byte keyed round-trip back through the
 * genuine on-disk Prolog-3 index (vms-401, epic vms-d0c). No .rms_idx sidecar,
 * no /vms passthrough, no faked success -- real IO$_CREATE / IO$_WRITEVBLK /
 * IO$_READVBLK on a real /dev/vms (Rule 9 / INV-6).
 *
 * HISTORY. This suite began as the vms-221 pin: PARTS's RMS indexed create hit
 * EACCES because the RETIRED userspace path (rms_idx_put -> btree_save's
 * ".rms_idx" sidecar over the /vms passthrough) reopened a zero-UIC file for
 * write on its second periodic save. The converged Files-11 program
 * (files11-acp-pivot) retired that sidecar entirely: an indexed file is now a
 * genuine Prolog-3 image authored on the fresh ACP data fork
 * (rms_idx_author_p3 -> rms_p3_create/put), so there is no sidecar to reopen
 * and no passthrough UIC to get wrong. What remains worth proving is that the
 * REAL ACP indexed write path holds up under load.
 *
 * THE vms-401 DEFECT THIS SUITE NOW PINS (ACP block extend). rms_p3_create/put
 * grow the file one bucket at a time (IO$_WRITEVBLK past EOF -> implicit
 * extend). vms_ioctl_acp_writevb (src/kernel-core/vmsfs_acp.c) allocated each
 * one-block run into a FRESH channel-window extent (ch->win) and a fresh FH2
 * retrieval pointer (ods2_fh2_map_append, src/vmsfs/ods2/ods2_edit.c) -- never
 * coalescing physically-contiguous runs, though VMS maps a contiguous file with
 * ONE retrieval pointer. The fixed 24-entry window (ACP_WINDOW_MAX) therefore
 * exhausted after 24 blocks: MEASURED, $PUT #65 failed SS$_DEVICEFULL ->
 * RMS$_WPL, the file capped at 24 blocks, and the first 64 records read back
 * fine. THE FIX (vms-401): the extend path now grows the last window extent /
 * last FH2 retrieval pointer in place when the freshly allocated LBNs are
 * contiguous with it -- so a contiguous grower stays ONE extent, and 205
 * records (crossing both periodic split points and a root-level growth,
 * multi-level index vms-5a3) all land and all round-trip by key.
 *
 * SYSTEM identity is preserved (the child drops to UIC [1,4] before create):
 * SYSTEM genuinely creates + fills the indexed file over the ACP.
 *
 * This rig insmods vms.ko directly and never runs PID 1, so it MOUNTS the ODS-2
 * SYSTEM DISK on VDA0: itself (the boot-time precondition) and creates in the
 * real fixture directory [OVMXDIR] -- the same idiom test_syssvc_rms_acp.c uses.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms: exits EXIT_SKIP (77) in the
 * CI negative-control rig (no executive at all, vms-0ff), never a fake pass.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <grp.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "vms_kif.h"
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "vms/logical.h"

#include "rms/rms.h"
#include "rms/fab.h"
#include "rms/rab.h"
#include "rms/xab.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* SYSTEM's UIC as shipped in SYSUAF.DAT ("SYSTEM|...|1|4|...") -- pinned to
 * the data file, not re-derived (vms-9b7: no second SYSUAF parser). Same
 * convention test_syssvc_scratch_writable.c (vms-e5c) uses. */
#define SYSTEM_UIC_GROUP   1u
#define SYSTEM_UIC_MEMBER  4u

/* PARTS's own record shape (src/apps/parts/parts.h): 56-byte fixed record,
 * an 8-byte primary key at offset 0. Mirrored exactly here so this test
 * exercises the identical rms_idx.c code path PARTS does. */
#define REC_KEY_SIZE  8
#define REC_SIZE      56
#define PUT_COUNT     205   /* > 200: crosses BOTH periodic btree_save()
                              * points (100 and 200) that the divergence
                              * above depends on. */

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

/* Provision [SYSTMP] (SYS$SCRATCH:'s target) exactly the way vms-e5c's fix
 * does (src/ovmx_init/ovmx_init.c's provision_writable_dir(), duplicated
 * here for the same reason test_syssvc_scratch_writable.c duplicates it:
 * this rig insmods vms.ko directly and never runs PID 1's boot sequence,
 * and vms-e5c is a separate, unmerged item outside this item's lane
 * (src/ovmx_init is not touched here) -- so this test stands up its own
 * genuinely-SYSTEM-writable directory rather than depending on that item
 * landing first. */
static void provision_writable_dir_for_test(const char *path)
{
    mkdir(path, 0775);
    if (chown(path, (uid_t)SYSTEM_UIC_MEMBER, (gid_t)SYSTEM_UIC_GROUP) != 0)
        fprintf(stderr, "  SETUP-WARN: chown(%s) failed: %s\n", path, strerror(errno));
    if (chmod(path, 0775) != 0)
        fprintf(stderr, "  SETUP-WARN: chmod(%s) failed: %s\n", path, strerror(errno));
}

static void init_key(struct XABKEY *xab)
{
    memset(xab, 0, sizeof(*xab));
    xab->xab$b_cod  = XAB$C_KEY;
    xab->xab$b_bln  = sizeof(struct XABKEY);
    xab->xab$b_ref  = 0;
    xab->xab$b_dtp  = XAB$C_STG;
    xab->xab$w_flg  = 0;
    xab->xab$w_pos0 = 0;
    xab->xab$b_siz0 = REC_KEY_SIZE;
    xab->xab$b_nseg = 1;
    xab->xab$w_tks  = REC_KEY_SIZE;
    xab->xab$l_nxt  = NULL;
}

static void init_fab(struct FAB *fab, struct XABKEY *xab, const char *filespec)
{
    memset(fab, 0, sizeof(*fab));
    fab->fab$b_bid = FAB$C_BID;
    fab->fab$b_bln = (uint8_t)sizeof(struct FAB);
    fab->fab$b_org = FAB$C_IDX;
    fab->fab$b_rfm = FAB$C_FIX;
    fab->fab$w_mrs = REC_SIZE;
    fab->fab$b_fac = FAB$M_PUT | FAB$M_GET;
    fab->fab$b_shr = FAB$M_NIL;
    fab->fab$l_fna = (char *)filespec;
    fab->fab$b_fns = (uint8_t)strlen(filespec);
    fab->fab$l_xab = xab;
    fab->_rms_file = 0;   /* vms-bc7: RMS handle (was _linux_fd) */
}

static void init_rab(struct RAB *rab, struct FAB *fab)
{
    memset(rab, 0, sizeof(*rab));
    rab->rab$b_bid = RAB$C_BID;
    rab->rab$b_bln = (uint8_t)sizeof(struct RAB);
    rab->rab$l_fab = fab;
    rab->rab$b_rac = RAB$C_SEQ;
}

/*
 * Child process: drop to SYSTEM's UIC exactly like tools/vms_login.c, then
 * drive the REAL sys$create()/sys$connect()/sys$put()/sys$close() the same
 * way PARTS's src/apps/parts/parts_db.c does, reporting every status/errno
 * on the pipe so the parent can pin exactly where (if anywhere) this fails.
 */
static void child_main(int wfd, const char *vms_spec)
{
    char msg[1152];
    struct FAB fab;
    struct RAB rab;
    struct XABKEY xab;

    if (setgroups(0, NULL) != 0 ||
        setgid((gid_t)SYSTEM_UIC_GROUP) != 0 ||
        setuid((uid_t)SYSTEM_UIC_MEMBER) != 0) {
        snprintf(msg, sizeof(msg), "DROP_FAILED=%d\n", errno);
        (void)!write(wfd, msg, strlen(msg));
        _exit(2);
    }
    snprintf(msg, sizeof(msg), "UID=%d GID=%d\n", (int)geteuid(), (int)getegid());
    (void)!write(wfd, msg, strlen(msg));

    /* Raw-open comparison, same directory, same dropped credentials --
     * mirrors vms-e5c's own positive control, run from inside THIS process
     * so it is a true apples-to-apples comparison against sys$create()
     * below (same fork, same UID/GID, same filesystem state). */
    {
        int rawfd = open("/vms/SYSTMP/VMS221_RAW.DAT",
                          O_CREAT | O_WRONLY | O_EXCL, 0644);
        snprintf(msg, sizeof(msg), "RAW_OPEN=%d ERRNO=%d (%s)\n",
                 rawfd, rawfd < 0 ? errno : 0,
                 rawfd < 0 ? strerror(errno) : "-");
        (void)!write(wfd, msg, strlen(msg));
        if (rawfd >= 0) { close(rawfd); unlink("/vms/SYSTMP/VMS221_RAW.DAT"); }
    }

    init_key(&xab);
    init_fab(&fab, &xab, vms_spec);
    init_rab(&rab, &fab);

    uint32_t st = sys$create(&fab, 0, 0);
    snprintf(msg, sizeof(msg), "CREATE_STATUS=%u (%s) CREATE_STV=%u (%s)\n",
             (unsigned)st, $VMS_STATUS_SUCCESS(st) ? "OK" : "FAIL",
             (unsigned)fab.fab$l_stv,
             fab.fab$l_stv ? strerror((int)fab.fab$l_stv) : "-");
    (void)!write(wfd, msg, strlen(msg));
    snprintf(msg, sizeof(msg), "RESOLVED_PATH=%s\n", fab._resolved_path);
    (void)!write(wfd, msg, strlen(msg));

    if (!$VMS_STATUS_SUCCESS(st)) {
        _exit(0);
    }

    st = sys$connect(&rab, 0, 0);
    snprintf(msg, sizeof(msg), "CONNECT_STATUS=%u (%s)\n", (unsigned)st,
             $VMS_STATUS_SUCCESS(st) ? "OK" : "FAIL");
    (void)!write(wfd, msg, strlen(msg));

    char record[REC_SIZE];
    memset(record, 'X', sizeof(record));
    uint32_t last_put_status = 0;
    unsigned failed_at = 0;
    for (unsigned k = 1; k <= PUT_COUNT; k++) {
        char keybuf[REC_KEY_SIZE + 1];
        /* "PNnnnnnn", exactly REC_KEY_SIZE bytes, no NUL terminator in the
         * record itself -- matches PARTS's own parts_make_key(). */
        snprintf(keybuf, sizeof(keybuf), "PN%06u", k);
        memcpy(record, keybuf, REC_KEY_SIZE);
        rab.rab$b_rac = RAB$C_SEQ;
        rab.rab$l_rbf = record;
        rab.rab$w_rsz = REC_SIZE;
        uint32_t pst = sys$put(&rab, 0, 0);
        if (!$VMS_STATUS_SUCCESS(pst) && failed_at == 0) {
            failed_at = k;
            last_put_status = pst;
        }
        if (k == 100 || k == 200 || k == PUT_COUNT) {
            snprintf(msg, sizeof(msg), "PUT_AT_%u_STATUS=%u (%s) STV=%u (%s)\n",
                     k, (unsigned)pst, $VMS_STATUS_SUCCESS(pst) ? "OK" : "FAIL",
                     (unsigned)fab.fab$l_stv,
                     fab.fab$l_stv ? strerror((int)fab.fab$l_stv) : "-");
            (void)!write(wfd, msg, strlen(msg));
        }
    }
    if (failed_at) {
        snprintf(msg, sizeof(msg), "FIRST_PUT_FAILURE_AT=%u STATUS=%u\n",
                 failed_at, (unsigned)last_put_status);
        (void)!write(wfd, msg, strlen(msg));
    } else {
        snprintf(msg, sizeof(msg), "ALL_%u_PUTS_SUCCEEDED\n", PUT_COUNT);
        (void)!write(wfd, msg, strlen(msg));
    }

    /* ROUND-TRIP (vms-401): the records were $PUT into the genuine on-disk
     * Prolog-3 index over the ACP (no .rms_idx sidecar). Read every one BACK BY
     * KEY through the same open file and verify the body byte-for-byte -- proof
     * the created indexed file is a real, re-readable Prolog-3 image, not a
     * write-only success. */
    {
        char rdbuf[REC_SIZE + 8];
        unsigned readback_ok = 0, readback_fail = 0;
        for (unsigned k = 1; k <= PUT_COUNT; k++) {
            char keybuf[REC_KEY_SIZE + 1], want[REC_SIZE];
            snprintf(keybuf, sizeof(keybuf), "PN%06u", k);
            memset(want, 'X', sizeof(want));
            memcpy(want, keybuf, REC_KEY_SIZE);

            memset(rdbuf, 0, sizeof(rdbuf));
            rab.rab$b_rac = RAB$C_KEY;
            rab.rab$l_kbf = keybuf;
            rab.rab$b_ksz = REC_KEY_SIZE;
            rab.rab$b_krf = 0;
            rab.rab$l_rop = 0;
            rab.rab$l_ubf = rdbuf;
            rab.rab$w_usz = REC_SIZE;
            rab.rab$w_rsz = 0;
            uint32_t gs = sys$get(&rab, 0, 0);
            if ($VMS_STATUS_SUCCESS(gs) && rab.rab$w_rsz == REC_SIZE &&
                memcmp(rdbuf, want, REC_SIZE) == 0)
                readback_ok++;
            else
                readback_fail++;
        }
        snprintf(msg, sizeof(msg),
                 "READBACK_BY_KEY_OK=%u READBACK_BY_KEY_FAIL=%u OF=%u\n",
                 readback_ok, readback_fail, PUT_COUNT);
        (void)!write(wfd, msg, strlen(msg));
    }

    sys$disconnect(&rab, 0, 0);
    uint32_t cst = sys$close(&fab, 0, 0);
    snprintf(msg, sizeof(msg), "CLOSE_STATUS=%u (%s)\n", (unsigned)cst,
             $VMS_STATUS_SUCCESS(cst) ? "OK" : "FAIL");
    (void)!write(wfd, msg, strlen(msg));

    /* ISOLATION: erase the file (IO$_DELETE) so the net [OVMXDIR] state is
     * restored for any later suite booted in the same VM against this VDA0:
     * -- the same discipline test_syssvc_rms_acp.c / test_syssvc_acp_create.c
     * keep. Best-effort; not asserted. */
    {
        struct FAB efab;
        init_fab(&efab, &xab, vms_spec);
        uint32_t est = sys$erase(&efab, 0, 0);
        snprintf(msg, sizeof(msg), "ERASE_STATUS=%u (%s)\n", (unsigned)est,
                 $VMS_STATUS_SUCCESS(est) ? "OK" : "FAIL");
        (void)!write(wfd, msg, strlen(msg));
    }

    _exit(0);
}

static int run_scenario(const char *vms_spec, char *out, size_t outsz)
{
    int p[2];
    if (pipe(p) != 0) return -1;

    fflush(NULL);
    pid_t pid = fork();
    if (pid < 0) { close(p[0]); close(p[1]); return -1; }

    if (pid == 0) {
        close(p[0]);
        child_main(p[1], vms_spec);
        _exit(0);
    }

    close(p[1]);
    size_t used = 0;
    for (;;) {
        ssize_t n = read(p[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(p[0]);

    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/*
 * field_is_ok - true if "<label>...(OK)" appears on the SAME line as
 * <label> in the child's transcript, before the "(OK)"/"(FAIL)" tag the
 * child prints right after the numeric status (see child_main() above).
 * Avoids pinning the exact numeric RMS status code, which is a real but
 * unimportant implementation detail here -- only success/failure matters.
 */
static int field_is_ok(const char *out, const char *label)
{
    const char *l = strstr(out, label);
    const char *eol, *ok;

    if (!l) return 0;
    eol = strchr(l, '\n');
    ok = strstr(l, "(OK)");
    return ok != NULL && (!eol || ok < eol);
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_rms_scratch_create (SYSTEM sys$create + 205 $PUT of "
           "an RMS indexed file over the Files-11 ACP + keyed round-trip, "
           "vms-401) ===\n");

    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI negative-control rig, not the product\n");
        printf("=== test_syssvc_rms_scratch_create: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    {
        char p[512];
        int vst = vmsfs_to_linux_path(VMS_SYSTMP, p, sizeof(p));
        printf("  DIAG: vmsfs_to_linux_path(VMS_SYSTMP)=%d path=%s\n", vst, p);
        if (vst == 1)
            provision_writable_dir_for_test(p);

        struct stat vst_parent, vst_dir;
        if (stat("/vms", &vst_parent) == 0)
            printf("  DIAG: /vms mode=%o uid=%u gid=%u\n",
                   vst_parent.st_mode & 07777, vst_parent.st_uid, vst_parent.st_gid);
        else
            printf("  DIAG: stat(/vms) failed: %s\n", strerror(errno));
        if (stat(p, &vst_dir) == 0)
            printf("  DIAG: %s mode=%o uid=%u gid=%u\n",
                   p, vst_dir.st_mode & 07777, vst_dir.st_uid, vst_dir.st_gid);
        else
            printf("  DIAG: stat(%s) failed: %s\n", p, strerror(errno));
    }

    /* Mount the ODS-2 SYSTEM DISK executive-global on VDA0: so the child's
     * sys$create()'s $ASSIGN sees a mounted volume (vms-401). PID 1 does this at
     * boot; this rig insmods vms.ko directly and never runs the boot sequence,
     * so it stands up the precondition itself -- the same idiom every ACP suite
     * uses (test_syssvc_rms_acp.c, test_syssvc_acp_create.c). Executive-global,
     * so the post-fork child (dropped to SYSTEM) sees the same mounted volume. */
    {
        uint32_t mst = vms_kif_acp_mount("VDA0:");
        printf("  DIAG: vms_kif_acp_mount(VDA0:)=%u (%s)\n", (unsigned)mst,
               $VMS_STATUS_SUCCESS(mst) ? "OK" : "FAIL");
        CHECK($VMS_STATUS_SUCCESS(mst),
              "the ODS-2 SYSTEM DISK mounted executive-global on VDA0: (the ACP "
              "precondition PID 1 satisfies at boot; this rig mounts it itself, "
              "same idiom as test_syssvc_rms_acp.c)");
    }

    static char out[8192];
    if (run_scenario("VDA0:[OVMXDIR]VMS221.DAT", out, sizeof(out)) != 0) {
        CHECK(0, "could not run the SYSTEM sys$create scenario");
        printf("=== test_syssvc_rms_scratch_create: %d passed, %d failed ===\n", pass, fail);
        return fail > 0 ? 1 : 0;
    }
    printf("%s", out);

    CHECK(strstr(out, "DROP_FAILED") == NULL,
          "SYSTEM's credential drop to UIC [1,4] succeeded");
    CHECK(strstr(out, "RESOLVED_PATH=VMS221.DAT") != NULL,
          "sys$create() parsed the filespec through the Files-11 ACP path "
          "(rms_acp_specs_from_fab): the device (VDA0:) and directory "
          "([OVMXDIR]) were split off and the ODS-2 file name resolved to "
          "VMS221.DAT -- NOT treated as a literal Linux/relative path. The "
          "vms-221 regression (resolve_filename() checking vmsfs_to_linux_path()'s "
          "VMS status code with `== 0`, so odd=success never matched and every "
          "VMS spec fell through to a literal path) belongs to the retired POSIX "
          "$CREATE (rms_posix_create); the Rule-9 runtime creates on the mounted "
          "volume over the ACP, which is what this proves.");
    /* negctl: rms-create-filespec-not-translated */
    CHECK(strstr(out, "RESOLVED_PATH=./") == NULL &&
          strstr(out, "RESOLVED_PATH=VDA0") == NULL,
          "sys$create() did NOT fall back to treating the raw VMS spec "
          "string as a literal path (no './' prefix, no undivided 'VDA0:...' "
          "device left glued to the name)");
    CHECK(field_is_ok(out, "CREATE_STATUS="),
          "sys$create() of the RMS indexed file on the mounted ODS-2 volume "
          "succeeded (a real IO$_CREATE FID + ACP write window, no EACCES)");
    CHECK(field_is_ok(out, "CONNECT_STATUS="),
          "sys$connect() on the freshly created file succeeded");
    CHECK(field_is_ok(out, "PUT_AT_100_STATUS="),
          "sys$put() #100 into the genuine on-disk Prolog-3 index over the ACP "
          "succeeded (vms-401 retired the faked .rms_idx sidecar)");
    CHECK(field_is_ok(out, "PUT_AT_200_STATUS="),
          "sys$put() #200 succeeded (records ride real Prolog-3 bucket writes "
          "over IO$_WRITEVBLK, no sidecar reopen)");
    {
        char want[64];
        snprintf(want, sizeof(want), "ALL_%u_PUTS_SUCCEEDED\n", PUT_COUNT);
        CHECK(strstr(out, want) != NULL,
              "every sys$put() succeeded -- no silent mid-run EACCES");
    }
    CHECK(field_is_ok(out, "CLOSE_STATUS="),
          "sys$close() of the created indexed file succeeded");
    /* vms-401: the created indexed file is a real, re-readable Prolog-3 image. */
    CHECK(strstr(out, "READBACK_BY_KEY_FAIL=0 ") != NULL &&
          strstr(out, "READBACK_BY_KEY_OK=0 ") == NULL,
          "ROUND-TRIP: every record $PUT into VDA0:[OVMXDIR]VMS221.DAT reads back "
          "BY KEY byte-for-byte through the genuine Prolog-3 index over the ACP "
          "(no sidecar) -- 205 records cross both periodic split points and a "
          "root-level growth (multi-level index, vms-5a3)");

    vms_kif_acp_dmount("VDA0:");

    printf("=== test_syssvc_rms_scratch_create: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
