/*
 * test_syssvc_lnm_crossproc.c - executive-resident LNM$SYSTEM through the
 * PUBLIC sys$ API, A-defines / B-translates (vms-d37).
 *
 * ============================================================
 * THIS IS THE POINT OF vms-d37. On OpenVMS a name defined in LNM$SYSTEM by
 * one process is visible to EVERY other process on the node, because the
 * SYSTEM logical name table is executive-resident. In OVMX before this item
 * sys$crelnm/$trnlnm/$dellnm answered from a process-private array
 * (src/libvms/syssvc/sys_logical.c logical_table[]), so DEFINE/SYSTEM in one
 * process was invisible in another -- the exact defect the 0.2 demo hit
 * (@SYS$UPDATE:PARTS_SETUP.COM's DEFINE/SYSTEM was per-process).
 *
 * This suite proves the fix the way the executive requires it to be proved
 * (CLAUDE.md Rule 9): two SEPARATE processes, one real /dev/vms. The parent
 * defines a name in LNM$SYSTEM; the child -- a distinct process with its own
 * executive PCB -- translates it and must see the parent's value. The storage
 * that carries it across is vms.ko's, reached by an ioctl to define and a
 * read-only mmap to translate (design "C-corrected",
 * docs/design-logical-name-placement.md).
 * ============================================================
 *
 * NO STATUS CONSTANT IS ASSERTED BY VALUE for the success path: assertions
 * use the VMS odd/even success convention, so this suite measures the
 * cross-process VISIBILITY property and cannot go red for a status-numbering
 * reason that belongs to another item. The one value it names is
 * SS$_NOLOGNAM, for "the deleted name is gone" and "a never-defined name is
 * absent" -- distinguishing absence (odd/even aside, a specific condition)
 * from the executive-absent SS$_NOSUCHDEV the no-fallback rule requires.
 *
 * NEGATIVE CONTROL (NEW-EXECUTIVE-TEST rule, tests/qemu/facility_defects.sh):
 * the cross-process DELETE assertion is anchored by the lnm-delete-noop
 * defect. That defect makes vms_ioctl_lnm_delete skip freeing the entry while
 * still reporting SS$_NORMAL, so DEASSIGN/SYSTEM "succeeds" but the name is
 * still there -- reddening exactly the "gone here too" assertion and no other
 * (a surgical single-property mutation, not a blunderbuss over the whole
 * facility).
 *
 * SYNCHRONISATION: pipes only, no sleeps; the parent bounds each wait so a
 * failure is a named FAIL line, not a harness-wide QEMU timeout -- the same
 * discipline as test_syssvc_ef_mproc.c.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "lnmdef.h"
#include "ovmx_layout.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/logical.h"

#define EXIT_SKIP 77
/* env-tunable so the negctl full-suite run can bound a broken peer's wait
 * (tests/qemu/inject_and_run.sh sets OVMX_KE_PEER_TIMEOUT_MS); default 20000
 * unchanged. Only a peer that FAILS to deliver ever reaches this bound -- a
 * pristine read returns on its poll the instant the peer writes -- so
 * shortening it bounds a mutation-broken run without touching legit timing. */
static int ke_peer_timeout_ms(void){const char*e=getenv("OVMX_KE_PEER_TIMEOUT_MS");int v=(e&&*e)?atoi(e):0;return v>0?v:20000;}
#define PEER_TIMEOUT_MS ke_peer_timeout_ms()

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

struct child_msg { uint32_t pass; uint32_t fail; };

static int read_bounded(int fd, void *buf, size_t len, int timeout_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    size_t got = 0;
    while (got < len) {
        int pr = poll(&pfd, 1, timeout_ms);
        if (pr == 0) return 0;
        if (pr < 0) return -1;
        ssize_t n = read(fd, (char *)buf + got, len - got);
        if (n <= 0) return -1;
        got += (size_t)n;
    }
    return 1;
}

static int send_token(int fd, char tok) { return write(fd, &tok, 1) == 1 ? 0 : -1; }

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* DEFINE a name in a table via the public sys$crelnm. */
static uint32_t def(const char *table, const char *name, const char *val)
{
    struct dsc$descriptor_s td = mkdsc(table);
    struct dsc$descriptor_s nd = mkdsc(name);
    struct item_list_3 il[2];
    memset(il, 0, sizeof(il));
    il[0].buflen    = (uint16_t)strlen(val);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = (void *)val;
    il[0].retlen    = NULL;
    return sys$crelnm(NULL, &td, &nd, NULL, il);
}

/* TRANSLATE a name via the public sys$trnlnm; out receives the value. */
static uint32_t trn(const char *table, const char *name, char *out, size_t outsz)
{
    struct dsc$descriptor_s td = mkdsc(table);
    struct dsc$descriptor_s nd = mkdsc(name);
    struct item_list_3 il[2];
    uint16_t rl = 0;
    memset(il, 0, sizeof(il));
    il[0].buflen    = (uint16_t)(outsz - 1);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = out;
    il[0].retlen    = &rl;
    out[0] = '\0';
    uint32_t st = sys$trnlnm(NULL, &td, &nd, NULL, il);
    if (rl >= outsz) rl = (uint16_t)(outsz - 1);
    out[rl] = '\0';
    return st;
}

static uint32_t del(const char *table, const char *name)
{
    struct dsc$descriptor_s td = mkdsc(table);
    struct dsc$descriptor_s nd = mkdsc(name);
    return sys$dellnm(&td, &nd, NULL);
}

/*
 * THE 0.2 DEMO BLOCKER, proved cross-process (vms-96e2). SYS$MANAGER:STARTUP.COM
 * does DEFINE/SYSTEM SYS$UPDATE at boot; a later login must @SYS$UPDATE:...,
 * which only works if the name is executive-resident. Below, the PARENT does
 * the DEFINE/SYSTEM (as STARTUP would) and stages the install procedure; the
 * CHILD -- which never defines the name -- resolves SYS$UPDATE:PARTS_SETUP.COM
 * through vmsfs (the @-command path) and OPENS it. The definition crosses
 * processes only through the executive; the device table (DKA0: -> the mount,
 * the ONE Unix-path bridge) is legitimately per-process, so each registers it.
 */
#define SYSUPD_VMS_DIR   "SYS$SYSDEVICE:[SYS0.SYSCOMMON.SYSUPD]"
#define SYSUPD_LINUX_DIR SYSDISK_MOUNT "/SYS0/SYSCOMMON/SYSUPD"
#define SETUP_VMS_SPEC   "SYS$UPDATE:PARTS_SETUP.COM"

/* mkdir -p for a plain Linux path in the rig's /vms. */
static void mkpath(const char *path)
{
    char tmp[1024];
    strncpy(tmp, path, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* Same skip-vs-run decision, and same reasoning, as test_syssvc_ef_mproc.c:
 * open only to decide, never register by hand (kif_bind does that). */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

/*
 * Enumerate probe: SHOW LOGICAL (and SHOW LOGICAL/SYSTEM) does not translate a
 * single name -- it LISTS a table. That path is lnm_enumerate() (the vmslnm
 * MANAGER API DCL's cmd_show_logical calls), which for LNM$SYSTEM must walk the
 * executive-resident arena, not the empty process-private system_table. This
 * callback records whether the enumeration produced a specific name=value pair,
 * so the child can prove a name the PARENT defined is present in a LISTING it
 * runs in its own process (vms-96e2 enumerate closure).
 */
struct enum_probe { const char *want_name; const char *want_val; int found; };

static int enum_probe_cb(const char *name, const lnm_entry_t *entry, void *ctx)
{
    struct enum_probe *p = (struct enum_probe *)ctx;
    if (strcmp(name, p->want_name) == 0 &&
        entry->num_translations > 0 &&
        strcmp(entry->translations[0].value, p->want_val) == 0)
        p->found = 1;
    return 0;
}

static int run_child(int c2p_write, int p2c_read)
{
    struct child_msg msg;
    char tok, val[256];
    uint32_t st;

    /* Wait for the parent to have defined CROSSPROC in LNM$SYSTEM. */
    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'P') {
        printf("  FAIL: child: never saw the parent's define token\n");
        fail++;
    } else {
        st = trn("LNM$SYSTEM", "CROSSPROC", val, sizeof(val));
        printf("  INFO: child: sys$trnlnm(LNM$SYSTEM, CROSSPROC) status=%u value=\"%s\"\n",
               st, val);
        CHECK((st & 1) && strcmp(val, "HELLO") == 0,
              "child: a name DEFINE/SYSTEM'd by the parent is visible here (A defines, B translates, LNM$SYSTEM is executive-resident)");

        /* It is also found through the default LNM$FILE_DEV search order,
         * which reaches LNM$SYSTEM after the process-private tables miss. */
        st = trn("LNM$FILE_DEV", "CROSSPROC", val, sizeof(val));
        CHECK((st & 1) && strcmp(val, "HELLO") == 0,
              "child: the parent's LNM$SYSTEM name resolves through the default search list too");

        /*
         * THE 0.2 DEMO PROOF (vms-96e2). The parent DEFINE/SYSTEM'd SYS$UPDATE
         * (and SYS$SYSDEVICE it resolves through) and staged PARTS_SETUP.COM.
         * This child never defined either name. It registers only DKA0: (the
         * per-process Unix-path bridge), then:
         *   (a) SHOW LOGICAL / F$TRNLNM path: sys$trnlnm(SYS$UPDATE) resolves;
         *   (b) @-command path: vmsfs resolves SYS$UPDATE:PARTS_SETUP.COM and
         *       the install procedure OPENS.
         * Both work only because LNM$SYSTEM is executive-resident.
         */
        vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);

        st = trn("LNM$SYSTEM", "SYS$UPDATE", val, sizeof(val));
        printf("  INFO: child: sys$trnlnm(SYS$UPDATE) status=%u value=\"%s\"\n", st, val);
        CHECK((st & 1) && strcmp(val, SYSUPD_VMS_DIR) == 0,
              "child: SYS$UPDATE DEFINE/SYSTEM'd by the parent resolves here (SHOW LOGICAL / F$TRNLNM)");

        char lp[1024];
        int rr = vmsfs_to_linux_path(SETUP_VMS_SPEC, lp, sizeof(lp));
        printf("  INFO: child: vmsfs_to_linux_path(%s) r=%d path=\"%s\"\n",
               SETUP_VMS_SPEC, rr, rr == 1 ? lp : "");
        CHECK(rr == 1 && strncmp(lp, SYSUPD_LINUX_DIR, strlen(SYSUPD_LINUX_DIR)) == 0,
              "child: @SYS$UPDATE:PARTS_SETUP.COM resolves under the kit directory (cross-process, via vmsfs)");
        FILE *pf = (rr == 1) ? fopen(lp, "r") : NULL;
        CHECK(pf != NULL,
              "child: @SYS$UPDATE:PARTS_SETUP.COM OPENS -- the 0.2 demo blocker is clear");
        if (pf) fclose(pf);

        /*
         * SHOW LOGICAL / SHOW LOGICAL/SYSTEM proof (vms-96e2 enumerate
         * closure): a cross-process define must not only TRANSLATE in another
         * process, it must be LISTED there. lnm_enumerate(LNM$SYSTEM) is the
         * exact manager-API path DCL's cmd_show_logical walks; here it must
         * surface the parent's CROSSPROC=HELLO from THIS separate process,
         * proving the enumerate reads the shared executive arena and not the
         * empty process-private system_table.
         */
        struct enum_probe ep = { "CROSSPROC", "HELLO", 0 };
        uint32_t est = lnm_enumerate(lnm_get_manager(), LNM_SYSTEM_TABLE,
                                     enum_probe_cb, &ep);
        printf("  INFO: child: lnm_enumerate(LNM$SYSTEM) status=%u found=%d\n",
               est, ep.found);
        CHECK(est == SS$_NORMAL && ep.found,
              "child: SHOW LOGICAL's enumerate path LISTS the parent's CROSSPROC=HELLO (cross-process listing reads the shared executive table, not a process-private copy)");
    }

    /* Child defines a name of its own; parent must read it back. */
    st = def("LNM$SYSTEM", "CROSSPROC2", "WORLD");
    CHECK(st & 1, "child: sys$crelnm/system reported success");
    if (send_token(c2p_write, 'C') < 0) fail++;

    /* Parent deletes CROSSPROC; the child must see it gone. */
    if (read_bounded(p2c_read, &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'D') {
        printf("  FAIL: child: never saw the parent's delete token\n");
        fail++;
    } else {
        st = trn("LNM$SYSTEM", "CROSSPROC", val, sizeof(val));
        printf("  INFO: child: after parent DEASSIGN/SYSTEM, sys$trnlnm status=%u\n", st);
        /* negctl: lnm-delete-noop */
        CHECK(st == SS$_NOLOGNAM,
              "child: a name the parent DEASSIGN/SYSTEM'd is gone here too (delete is cross-process, and it is absence, not SS$_NOSUCHDEV)");
    }

    msg.pass = (uint32_t)pass;
    msg.fail = (uint32_t)fail;
    if (write(c2p_write, &msg, sizeof(msg)) != (ssize_t)sizeof(msg))
        return 1;
    return fail > 0 ? 1 : 0;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    int p2c[2], c2p[2];
    char tok, val[256];
    uint32_t st;

    printf("=== test_syssvc_lnm_crossproc (executive-resident LNM$SYSTEM via the PUBLIC sys$ API) ===\n");

    if (!executive_present()) {
        /*
         * CI negative control only (a rig booted without insmod'ing vms.ko).
         * The no-fabricated-success property still holds and is still
         * asserted: DEFINE/SYSTEM must NOT report success when the executive
         * was never reached -- it must fail honestly with SS$_NOSUCHDEV,
         * never fall back to a per-process table (INV-6).
         */
        printf("  FAIL: parent: cannot open /dev/vms\n");
        st = def("LNM$SYSTEM", "CROSSPROC", "HELLO");
        printf("  INFO: sys$crelnm/system with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$crelnm in LNM$SYSTEM does NOT report success when the executive was never reached");
        CHECK(st == SS$_NOSUCHDEV,
              "parent: the executive-absent status is SS$_NOSUCHDEV (no per-process fallback)");
        printf("=== test_syssvc_lnm_crossproc: %d passed, %d failed (SKIPPED: no /dev/vms -- cross-process scenario not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    /* Negative control WITH an executive: a never-defined name is absent,
     * and absent is SS$_NOLOGNAM -- proving the executive is present and
     * answering, so a later miss cannot be an executive-absent artifact. */
    st = trn("LNM$SYSTEM", "OVMX$D37_NEVER_DEFINED", val, sizeof(val));
    CHECK(st == SS$_NOLOGNAM,
          "parent: a never-defined LNM$SYSTEM name is absent (SS$_NOLOGNAM), not an executive-absent error");

    if (pipe(p2c) < 0 || pipe(c2p) < 0) { printf("  FAIL: pipe() failed\n"); return 1; }

    pid_t pid = fork();
    if (pid < 0) { printf("  FAIL: fork() failed\n"); return 1; }
    if (pid == 0) {
        close(p2c[1]); close(c2p[0]);
        _exit(run_child(c2p[1], p2c[0]));
    }
    close(p2c[0]); close(c2p[1]);

    /* Parent (process A) defines CROSSPROC in LNM$SYSTEM. */
    st = def("LNM$SYSTEM", "CROSSPROC", "HELLO");
    printf("  INFO: parent: sys$crelnm(LNM$SYSTEM, CROSSPROC=HELLO) returned status %u\n", st);
    CHECK(st & 1, "parent: sys$crelnm in LNM$SYSTEM reported success");

    /*
     * The 0.2 demo setup (vms-96e2): DEFINE/SYSTEM SYS$SYSDEVICE + SYS$UPDATE
     * exactly as SYS$MANAGER:STARTUP.COM does at boot, and stage the install
     * procedure where SYS$UPDATE: points. The child (a separate process) will
     * resolve and open it without ever defining the name.
     */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    (void)def("LNM$SYSTEM", "SYS$SYSDEVICE", SYSDISK_DEVICE ":");
    uint32_t su = def("LNM$SYSTEM", "SYS$UPDATE", SYSUPD_VMS_DIR);
    /* SS$_NORMAL for a fresh name, SS$_SUPERSEDE if a prior suite in this same
     * booted guest already seeded SYS$UPDATE via lnm_setup_defaults (the
     * executive's LNM$SYSTEM persists across suites) -- both are a successful
     * DEFINE/SYSTEM. */
    CHECK((su & 1) || su == SS$_SUPERSEDE,
          "parent: DEFINE/SYSTEM SYS$UPDATE (the STARTUP.COM boot step) reported success");
    mkpath(SYSUPD_LINUX_DIR);
    {
        char sp[1024];
        if (vmsfs_to_linux_path(SETUP_VMS_SPEC, sp, sizeof(sp)) == 1) {
            FILE *f = fopen(sp, "w");
            if (f) { fputs("$! PARTS_SETUP.COM (cross-process proof)\n$ EXIT\n", f); fclose(f); }
            printf("  INFO: parent: staged install procedure at %s\n", sp);
        }
    }

    if (send_token(p2c[1], 'P') < 0) fail++;

    /* Child defines CROSSPROC2; parent reads it back (B defines, A reads). */
    if (read_bounded(c2p[0], &tok, 1, PEER_TIMEOUT_MS) != 1 || tok != 'C') {
        printf("  FAIL: parent: child never reported its own define\n");
        fail++;
    } else {
        st = trn("LNM$SYSTEM", "CROSSPROC2", val, sizeof(val));
        printf("  INFO: parent: sys$trnlnm(LNM$SYSTEM, CROSSPROC2) status=%u value=\"%s\"\n",
               st, val);
        CHECK((st & 1) && strcmp(val, "WORLD") == 0,
              "parent: a name the child DEFINE/SYSTEM'd is visible here (B defines, A translates)");
    }

    /* Parent deletes CROSSPROC; child confirms it is gone cross-process. */
    st = del("LNM$SYSTEM", "CROSSPROC");
    printf("  INFO: parent: sys$dellnm(LNM$SYSTEM, CROSSPROC) returned status %u\n", st);
    CHECK(st & 1, "parent: sys$dellnm in LNM$SYSTEM reported success");
    if (send_token(p2c[1], 'D') < 0) fail++;

    struct child_msg cm = {0, 0};
    int r = read_bounded(c2p[0], &cm, sizeof(cm), PEER_TIMEOUT_MS);
    if (r != 1) {
        printf("  FAIL: parent: no final report from the child (r=%d)\n", r);
        fail++;
    } else {
        pass += (int)cm.pass;
        fail += (int)cm.fail;
    }

    int wstatus = 0;
    waitpid(pid, &wstatus, 0);

    /* Tidy up so a re-run in the same booted guest starts clean. */
    (void)del("LNM$SYSTEM", "CROSSPROC2");
    (void)del("LNM$SYSTEM", "SYS$UPDATE");
    (void)del("LNM$SYSTEM", "SYS$SYSDEVICE");

    printf("=== test_syssvc_lnm_crossproc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
