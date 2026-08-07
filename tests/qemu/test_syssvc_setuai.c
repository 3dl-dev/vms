/*
 * test_syssvc_setuai.c - $SETUAI's SYSPRV test is decided by the EXECUTIVE,
 * and a caller's own claim about itself does not decide it (vms-cb5 round 5).
 *
 * WHAT THIS SUITE EXISTS TO KILL. src/libvms/syssvc/sys_uai.c guarded the one
 * service that rewrites SYSUAF.DAT -- including UAI$_PWD, an account's
 * password hash -- like this:
 *
 *     struct vms_pcb *pcb = vms_pcb_get();
 *     if (pcb && !(pcb->cur_privs & PRV$M_SYSPRV))
 *         return SS$_NOPRIV;
 *
 * Two ways through it, and both are exercised below by execution rather than
 * argued:
 *
 *   1. NO PCB, NO TEST. vms_pcb_get() returns NULL for a process that never
 *      called vms_pcb_init() (src/vmsprocess/vms_pcb.c), so `pcb &&` made the
 *      condition false and the service ran with NO privilege test at all.
 *      Scenario 1 is exactly that process.
 *
 *   2. THE MASK WAS THE CALLER'S OWN WORD. sys$setprv
 *      (src/libvms/syssvc/sys_misc.c) writes pcb->cur_privs for the calling
 *      process with no validation of any kind. Scenario 3 grants itself
 *      SYSPRV that way, over an executive row that does not hold it.
 *
 * SO THE PRIVILEGE TEST HAS TO BE READ FROM SOMEWHERE THE CALLER CANNOT
 * WRITE, which on OVMX means the executive's row (CLAUDE.md Rule 11): the
 * authorized mask is derived at registration from capable(CAP_SYS_ADMIN), and
 * the only way to change it is VMS_IOCTL_SETIDENT, which without SETPRV
 * accepts nothing that is not a weakening (src/kernel/vms_proctab.c). That is
 * the same source tools/vms_authorize.c's check_privilege() reads (vms-b2e),
 * and tests/qemu/test_syssvc_authorize.c is the sibling proof for AUTHORIZE.
 *
 * WHY IT NEEDS A REAL /dev/vms, and why it may not be a host test. With no
 * executive the corrected code refuses EVERY caller, so all three refusals
 * below are equally explained by "the executive read failed" -- and an
 * implementation that returned SS$_NOPRIV unconditionally would look
 * identical to a correct one. Scenario 4 is the positive that separates them,
 * and it can only be established through a real VMS_IOCTL_SETIDENT. In the CI
 * negative-control rig (booted with no executive on purpose -- vms-0ff: PID 1
 * refuses to boot without one, so that is a RIG state, never a product state)
 * this suite exits EXIT_SKIP, never a fake pass.
 *
 * EVERY VERDICT IS READ BY A PROCESS THAT DID NOT WRITE IT. Each scenario
 * runs in its own fork()ed child, so no scenario can be explained by state
 * another left in its address space; and the SYSUAF.DAT evidence is read by
 * the PARENT, which is a third process that neither authenticated nor wrote.
 *
 * SCENARIO 4 ALSO PINS THE UIC WRITE-BACK BASE (vms-e60). $SETUAI's rewrite
 * printed the two UIC fields with %u while parse_uaf_line() reads them with
 * strtoul(..., 8), so rewriting any record whose UIC digits differ between
 * the bases silently changed that account's UIC. USER1 ships 200|202, which
 * differs; SYSTEM's 1|4 does not, which is what hid it. The assertion is on
 * the FIELD TEXT of the rewritten row, so it fires on the value written, not
 * on a value re-derived by the same parser that produced it.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <sys/wait.h>

#include "starlet.h"
#include "ssdef.h"
#include "uaidef.h"
#include "prvdef.h"
#include "lnmdef.h"          /* struct item_list_3 */
#include "vms/pcb.h"
#include "vms_kif.h"
#include "ovmx_layout.h"
#include "vmsfs/filespec.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* An identity that holds SYSPRV -- the only one $SETUAI may serve. */
#define OK_NAME    "SECURITY"
#define OK_UIC     (((uint32_t)20 << 16) | 5u)
#define OK_PRIVS   (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_SYSPRV)

/*
 * An identity with a real, populated mask that deliberately never sets
 * SYSPRV. OPER and WORLD are in it so that a test of "the mask is non-empty"
 * or "the mask holds some elevated bit" rather than the SYSPRV bit itself
 * would let this caller through.
 */
#define NO_NAME    "FIELDREP"
#define NO_UIC     (((uint32_t)21 << 16) | 6u)
#define NO_PRIVS   (PRV$M_TMPMBX | PRV$M_NETMBX | PRV$M_OPER | PRV$M_WORLD)

/* The account each scenario aims at. USER1's shipped UIC is 200|202, whose
 * digits differ between octal and decimal -- see the header note. */
#define TARGET_USER   "USER1"
#define NEW_DEFDIR    "SYS$SYSDEVICE:[USERS.PROBE]"

/* How a child reports what happened. */
struct probe_result {
    uint32_t setident_status;   /* 0 when the scenario stamps no identity */
    uint32_t self_read;         /* status of vms_kif_getjpi_self */
    uint64_t exec_privs;        /* the mask the EXECUTIVE holds for the child */
    uint64_t pcb_privs;         /* the mask the child's own PCB holds, if any */
    int      had_pcb;           /* 1 if vms_pcb_get() was non-NULL */
    uint32_t setuai_status;     /* what sys$setuai returned */
};

/* Which self-serving claims a scenario stages before calling $SETUAI. */
#define STAGE_NONE        0
#define STAGE_IDENTITY    (1 << 0)   /* vms_kif_setident(name, uic, privs) */
#define STAGE_PCB_SYSPRV  (1 << 1)   /* vms_pcb_init + sys$setprv(SYSPRV) */

static void resolve_sysuaf(char *out, size_t outsz)
{
    out[0] = '\0';
    vmsfs_to_linux_path(VMS_SYSUAF_PATH, out, outsz);
}

/* Read SYSUAF.DAT whole. Returns bytes read, or -1. */
static long slurp_sysuaf(char *buf, size_t bufsz)
{
    char path[1024];
    resolve_sysuaf(path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;
    size_t n = fread(buf, 1, bufsz - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/*
 * Return the whole pipe-delimited SYSUAF row whose first field is 'user',
 * copied into 'out'. Returns 1 if found. The caller compares FIELD TEXT, so
 * nothing here re-parses a number the service just wrote.
 */
static int sysuaf_row(const char *text, const char *user, char *out, size_t outsz)
{
    size_t ulen = strlen(user);
    const char *p = text;

    while (p && *p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);

        if (len > ulen && strncmp(p, user, ulen) == 0 && p[ulen] == '|') {
            if (len >= outsz)
                len = outsz - 1;
            memcpy(out, p, len);
            out[len] = '\0';
            return 1;
        }
        p = eol ? eol + 1 : NULL;
    }
    out[0] = '\0';
    return 0;
}

/* Copy field 'idx' (0-based) of a pipe-delimited row into 'out'. */
static int row_field(const char *row, int idx, char *out, size_t outsz)
{
    const char *p = row;
    int i;

    for (i = 0; i < idx && p; i++) {
        p = strchr(p, '|');
        if (p) p++;
    }
    if (!p)
        return 0;

    const char *end = strchr(p, '|');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= outsz)
        len = outsz - 1;
    memcpy(out, p, len);
    out[len] = '\0';
    return 1;
}

/*
 * run_probe - one scenario, in its own process.
 *
 * The child stages whatever self-serving claims the scenario asks for, reads
 * back the mask the EXECUTIVE holds for it (so the evidence of what the
 * executive thinks is produced in the same run as the call, not asserted from
 * outside), calls sys$setuai, and writes the whole result struct back over a
 * pipe.
 */
static int run_probe(unsigned stage, const char *name, uint32_t uic,
                     uint64_t privs, struct probe_result *out)
{
    int fds[2];

    memset(out, 0, sizeof(*out));
    if (pipe(fds) < 0)
        return -1;

    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]); close(fds[1]);
        return -1;
    }

    if (pid == 0) {
        struct probe_result r;
        struct vms_procinfo self;
        struct vms_pcb *pcb;
        char defdir[64];
        struct item_list_3 items[2];
        struct dsc$descriptor_s usr;

        close(fds[0]);
        memset(&r, 0, sizeof(r));

        if (stage & STAGE_IDENTITY)
            r.setident_status = vms_kif_setident(name, uic, privs);

        if (stage & STAGE_PCB_SYSPRV) {
            /* The honor-system route, staged deliberately: a process
             * awarding itself SYSPRV in memory it owns. */
            pcb = vms_pcb_init(0);
            if (pcb) {
                uint64_t want = PRV$M_SYSPRV;
                (void)sys$setprv(1, &want, 1, NULL);
            }
        }

        memset(&self, 0, sizeof(self));
        r.self_read = vms_kif_getjpi_self(&self);
        if (r.self_read & 1)
            r.exec_privs = self.cur_privs;

        pcb = vms_pcb_get();
        r.had_pcb = pcb ? 1 : 0;
        if (pcb)
            r.pcb_privs = pcb->cur_privs;

        strncpy(defdir, NEW_DEFDIR, sizeof(defdir) - 1);
        defdir[sizeof(defdir) - 1] = '\0';

        memset(items, 0, sizeof(items));
        items[0].buflen    = (uint16_t)strlen(defdir);
        items[0].item_code = UAI$_DEFDIR;
        items[0].bufaddr   = defdir;

        memset(&usr, 0, sizeof(usr));
        usr.dsc$a_pointer = (char *)TARGET_USER;
        usr.dsc$w_length  = (uint16_t)strlen(TARGET_USER);

        r.setuai_status = sys$setuai(0, NULL, &usr, items, NULL, NULL, 0);

        {
            ssize_t w = write(fds[1], &r, sizeof(r));
            (void)w;
        }
        close(fds[1]);
        _exit(0);
    }

    close(fds[1]);
    {
        size_t used = 0;
        while (used < sizeof(*out)) {
            ssize_t n = read(fds[0], (char *)out + used, sizeof(*out) - used);
            if (n <= 0) break;
            used += (size_t)n;
        }
        close(fds[0]);
        if (used != sizeof(*out)) {
            int st;
            while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
                ;
            return -1;
        }
    }

    {
        int st;
        while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
            ;
    }
    return 0;
}

static void report(const char *label, const struct probe_result *r)
{
    printf("  %s: setident=%u self_read=%u exec_privs=%016llx "
           "had_pcb=%d pcb_privs=%016llx setuai=%u\n",
           label, (unsigned)r->setident_status, (unsigned)r->self_read,
           (unsigned long long)r->exec_privs, r->had_pcb,
           (unsigned long long)r->pcb_privs, (unsigned)r->setuai_status);
}

/*
 * Probe only -- vms_kif_open()/close() decide skip-vs-run and nothing else.
 */
static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0)
        return 0;
    vms_kif_close();
    return 1;
}

int main(void)
{
    static char before[8192], after[8192];
    static char row[1024], field[256];
    struct probe_result r1, r2, r3, r4;
    long nbefore, nafter;

    printf("=== test_syssvc_setuai ($SETUAI's SYSPRV test is the executive's, vms-cb5) ===\n");

    if (!executive_present()) {
        printf("  INFO: cannot open /dev/vms -- CI negative-control rig, not the product\n");
        printf("=== test_syssvc_setuai: 0 passed, 0 failed (SKIPPED: no /dev/vms) ===\n");
        return EXIT_SKIP;
    }

    nbefore = slurp_sysuaf(before, sizeof(before));
    if (nbefore <= 0) {
        printf("  FAIL: could not read SYSUAF.DAT -- nothing below can be judged\n");
        printf("=== test_syssvc_setuai: 0 passed, 1 failed ===\n");
        return 1;
    }
    CHECK(sysuaf_row(before, TARGET_USER, row, sizeof(row)) == 1,
          "the target account is present in SYSUAF.DAT before any probe");

    /* ---------------------------------------------------------------- *
     * 1. NO PCB AT ALL. The exact caller the `pcb &&` guard let through.
     * ---------------------------------------------------------------- */
    if (run_probe(STAGE_NONE, NULL, 0, 0, &r1) != 0) {
        printf("  FAIL: scenario 1 probe did not report\n");
        fail++;
    } else {
        report("no-pcb", &r1);
        CHECK(r1.had_pcb == 0,
              "1: the probe genuinely had NO PCB (vms_pcb_get() == NULL)");
        CHECK((r1.self_read & 1) &&
              !(r1.exec_privs & PRV$M_SYSPRV),
              "1: the executive's row for it does not hold SYSPRV");
        CHECK(r1.setuai_status == SS$_NOPRIV,
              "1: $SETUAI refuses a caller with no PCB (was: no test ran at all)");
    }

    /* ---------------------------------------------------------------- *
     * 2. A REAL AUTHENTICATED IDENTITY THAT IS NOT AUTHORIZED.
     * ---------------------------------------------------------------- */
    if (run_probe(STAGE_IDENTITY, NO_NAME, NO_UIC, NO_PRIVS, &r2) != 0) {
        printf("  FAIL: scenario 2 probe did not report\n");
        fail++;
    } else {
        report("no-sysprv", &r2);
        CHECK(r2.setident_status == SS$_NORMAL,
              "2: the executive accepted the non-SYSPRV identity");
        CHECK((r2.self_read & 1) && r2.exec_privs == NO_PRIVS,
              "2: the executive's row holds exactly the OPER|WORLD mask, no SYSPRV");
        CHECK(r2.setuai_status == SS$_NOPRIV,
              "2: $SETUAI refuses an authenticated identity without SYSPRV");
    }

    /* ---------------------------------------------------------------- *
     * 3. THE CALLER'S OWN CLAIM. Executive says no; the PCB says SYSPRV.
     *    Under the deleted code this caller was SERVED.
     * ---------------------------------------------------------------- */
    if (run_probe(STAGE_IDENTITY | STAGE_PCB_SYSPRV,
                  NO_NAME, NO_UIC, NO_PRIVS, &r3) != 0) {
        printf("  FAIL: scenario 3 probe did not report\n");
        fail++;
    } else {
        report("pcb-claims-sysprv", &r3);
        CHECK(r3.had_pcb == 1 && (r3.pcb_privs & PRV$M_SYSPRV) != 0,
              "3: the probe's OWN PCB really does carry SYSPRV");
        CHECK((r3.self_read & 1) && !(r3.exec_privs & PRV$M_SYSPRV),
              "3: the executive's row for it still does not hold SYSPRV");
        CHECK(r3.setuai_status == SS$_NOPRIV,
              "3: $SETUAI refuses it anyway -- the PCB mask does not decide");
    }

    /* Nothing above was authorized, so nothing above may have written. */
    nafter = slurp_sysuaf(after, sizeof(after));
    CHECK(nafter == nbefore && memcmp(before, after, (size_t)nbefore) == 0,
          "1-3: SYSUAF.DAT is byte-identical after all three refusals");

    /* ---------------------------------------------------------------- *
     * 4. THE POSITIVE. Without it every refusal above is equally
     *    explained by "$SETUAI refuses everyone".
     * ---------------------------------------------------------------- */
    if (run_probe(STAGE_IDENTITY, OK_NAME, OK_UIC, OK_PRIVS, &r4) != 0) {
        printf("  FAIL: scenario 4 probe did not report\n");
        fail++;
    } else {
        report("sysprv", &r4);
        CHECK(r4.setident_status == SS$_NORMAL,
              "4: the executive accepted the SYSPRV identity");
        CHECK((r4.self_read & 1) && (r4.exec_privs & PRV$M_SYSPRV) != 0,
              "4: the executive's row for it holds SYSPRV");
        CHECK(r4.setuai_status == SS$_NORMAL,
              "4: $SETUAI serves a SYSPRV identity -- the refusals are not blanket");
    }

    nafter = slurp_sysuaf(after, sizeof(after));
    CHECK(nafter > 0 && sysuaf_row(after, TARGET_USER, row, sizeof(row)) == 1,
          "4: the target account survives the rewrite");

    if (row_field(row, 4, field, sizeof(field)))
        printf("  rewritten default_dir field: %s\n", field);
    CHECK(row_field(row, 4, field, sizeof(field)) &&
          strcmp(field, NEW_DEFDIR) == 0,
          "4: the parent process reads the new default directory in the file");

    /*
     * The UIC write-back base (vms-e60). USER1 ships 200|202; %u would have
     * written 128|130 here, which the next read takes as octal 88|88... --
     * a different UIC for the same account.
     */
    if (row_field(row, 2, field, sizeof(field)))
        printf("  rewritten uic_group field: %s\n", field);
    CHECK(row_field(row, 2, field, sizeof(field)) && strcmp(field, "200") == 0,
          "4: the rewritten UIC GROUP field still reads 200 (octal, vms-e60)");
    if (row_field(row, 3, field, sizeof(field)))
        printf("  rewritten uic_member field: %s\n", field);
    CHECK(row_field(row, 3, field, sizeof(field)) && strcmp(field, "202") == 0,
          "4: the rewritten UIC MEMBER field still reads 202 (octal, vms-e60)");

    printf("=== test_syssvc_setuai: %d passed, %d failed ===\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
