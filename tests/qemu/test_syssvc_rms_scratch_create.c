/*
 * test_syssvc_rms_scratch_create.c - reproduce and pin the vms-221 defect:
 * SYSTEM's sys$create() (RMS indexed file, src/vmsrms/rms_core.c) under a
 * genuinely SYSTEM-writable SYS$SCRATCH: directory (vms-e5c's fix, applied
 * here directly since vms-e5c is a separate, unmerged item -- see
 * provision_writable_dir_for_test() below).
 *
 * THE MEASURED DIVERGENCE (vms-221). A plain open(2) on a resolved
 * SYS$SCRATCH: path succeeds for SYSTEM every time (test_syssvc_scratch_
 * writable.c, vms-e5c). PARTS's own sys$create() of an RMS INDEXED file
 * under the SAME directory still hit EACCES. Root cause, traced by reading
 * src/kernel/vmsfs/vmsfs_blkdev.c end to end:
 *
 *   vmsfs_blkdev_create() and vmsfs_blkdev_mkdir() memset() the new file's
 *   on-disk header to zero and populate fh_magic/fh_fid/fh_size/timestamps/
 *   fh_protection/... but NEVER write fh_uic_group or fh_uic_member (see
 *   vmsfs_ondisk.h: they are separate fields alongside fh_protection, not
 *   derived from it). vmsfs_blkdev_iget() reads them straight into the VFS
 *   inode: inode->i_uid = fh_uic_member, inode->i_gid = fh_uic_group. So
 *   EVERY new file/directory this filesystem creates is owned UIC [0,0]
 *   (Linux uid=0/gid=0) on disk, regardless of who created it.
 *
 *   This is invisible on a bare, single-shot open(O_CREAT) or sys$create():
 *   Linux's VFS grants the creator access to a file it JUST created within
 *   the same syscall regardless of the resulting mode/protection (POSIX
 *   "open(O_CREAT, 0000) still returns a usable fd" semantics) -- so the
 *   FIRST open of a brand new name always succeeds no matter what UIC ends
 *   up on disk. It surfaces the moment anything reopens that SAME file for
 *   WRITE in a LATER syscall, because that second open goes through the
 *   real vmsfs_blkdev_permission() SOGW check against the file's actual
 *   (bugged) owner [0,0] -- which is neither System (UIC group 0 IS [0,*],
 *   so this specific bug accidentally self-heals for System-category
 *   checks -- but SYSTEM's real UIC here is [1,4], group 1, so it does NOT
 *   match), nor Owner (uid 4 != 0), nor Group (gid 1 != 0): it falls to
 *   World, and VMSFS_PROT_DEFAULT (0xAA00) denies World WRITE. EACCES.
 *
 *   PARTS's RMS indexed-file create hits exactly this: rms_idx_put()
 *   (src/vmsrms/rms_idx.c) calls btree_save() -- a FRESH fopen(idx_path,
 *   "wb") -- every 100 records (tree->num_records %% 100 == 0), not just
 *   once. The FIRST save (record 100) creates ".rms_idx" fresh and hits the
 *   create-shortcut, masking the zero-UIC bug exactly like a bare
 *   sys$create() does. The SECOND save (record 200) reopens that SAME,
 *   now-existing, [0,0]-owned file for O_TRUNC|O_WRONLY -- a real reopen,
 *   which DOES get the full permission check -- and fails EACCES. Below,
 *   scenario A drives >200 sys$put()s through the real sys$create()+
 *   rms_idx_put() path and asserts exactly where this happens.
 *
 * THE FIX THIS ITEM SHIPS (src/kernel/vmsfs/vmsfs_blkdev.c): both
 * vmsfs_blkdev_create() and vmsfs_blkdev_mkdir() now stamp fh_uic_member/
 * fh_uic_group from the CREATING PROCESS's current_fsuid()/current_fsgid()
 * (mirroring plain Unix inode-creation semantics: the creator owns what it
 * creates), so a subsequent reopen by the SAME identity lands in the Owner
 * category and VMSFS_PROT_DEFAULT's zero-deny Owner nibble grants full
 * access -- matching what the file's protection bits were always meant to
 * mean.
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
    printf("=== test_syssvc_rms_scratch_create (SYSTEM sys$create of an RMS "
           "indexed file under SYS$SCRATCH:, vms-221) ===\n");

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

    static char out[8192];
    if (run_scenario("SYS$SCRATCH:VMS221.DAT", out, sizeof(out)) != 0) {
        CHECK(0, "could not run the SYSTEM sys$create scenario");
        printf("=== test_syssvc_rms_scratch_create: %d passed, %d failed ===\n", pass, fail);
        return fail > 0 ? 1 : 0;
    }
    printf("%s", out);

    CHECK(strstr(out, "DROP_FAILED") == NULL,
          "SYSTEM's credential drop to UIC [1,4] succeeded");
    CHECK(strstr(out, "RAW_OPEN=-1") == NULL,
          "a plain open(2) under SYS$SCRATCH:'s resolved directory succeeds "
          "(the e5c-style positive control, run from inside this SAME "
          "process/credentials for a true apples-to-apples comparison)");
    CHECK(strstr(out, "RESOLVED_PATH=/vms/SYSTMP/") != NULL ||
          strstr(out, "RESOLVED_PATH=/vms/systmp/") != NULL,
          "sys$create() resolved the VMS filespec SYS$SCRATCH:VMS221.DAT to "
          "its REAL translated Linux path under /vms/SYSTMP -- pins the "
          "vms-221 regression directly: resolve_filename() (src/vmsrms/rms_core.c) "
          "used to check vmsfs_to_linux_path()'s return value "
          "with `== 0`, but that function returns a VMS status code "
          "(SS$_NORMAL == 1 on success, odd = success); the check never "
          "matched, so EVERY VMS-spec candidate silently fell through to "
          "being treated as a literal (relative) Linux path instead of a "
          "translated one");
    CHECK(strstr(out, "RESOLVED_PATH=./SYS") == NULL,
          "sys$create() did NOT fall back to treating the raw VMS spec "
          "string as a literal relative Linux path (the exact vms-221 "
          "regression shape)");
    /* negctl: rms-create-filespec-not-translated */
    CHECK(field_is_ok(out, "CREATE_STATUS="),
          "sys$create() of the RMS indexed file under SYS$SCRATCH: succeeded "
          "(the vms-221 EACCES is gone)");
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
          "ROUND-TRIP: every record $PUT into SYS$SCRATCH:VMS221.DAT reads back "
          "BY KEY byte-for-byte through the genuine Prolog-3 index (no sidecar)");

    printf("=== test_syssvc_rms_scratch_create: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
