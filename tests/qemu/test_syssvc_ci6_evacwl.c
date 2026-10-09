/*
 * test_syssvc_ci6_evacwl.c - the evacuation workload's takeover proof
 * (vms-06c).
 *
 * Runs the REAL, VMS-native EVACWL.EXE (tests/lab/ci6/EVACWL.C, compiled by
 * OVMX's own TCC + LINK.EXE --use DECC$SHR, staged at
 * /vms/SYS0/SYSCOMMON/SYSEXE/EVACWL.EXE by tests/lab/ci6/mk_evacwl_native.sh
 * -- the same build-static/native directory glob every booted-OVMX native
 * image rides, no new Dockerfile staging needed) TWICE, as two genuinely separate
 * processes activated through IMGACT.EXE exactly as a customer's compiled
 * program would be: this harness does not call sys$enq/RMS itself to
 * emulate the workload, it execs the real image.
 *
 * SETUP: this harness defines the EVAC$DATA logical (LNM$SYSTEM, which is
 * executive-resident and therefore visible to a distinct process, the same
 * property test_syssvc_lnm_crossproc.c proves) pointing at VDA0:[OVMXDIR],
 * the writable ODS-2 scratch directory other RMS suites already use on
 * this harness's mounted system disk -- so EVACWL's own "EVAC$DATA:
 * EVAC.DAT" open resolves there with no new volume to mount.
 *
 * PROOF: instance A starts directly (no "standby"), instance B starts in
 * "standby" (NL then CONVERT to EX) within the same fork burst, so B's
 * $ENQW genuinely queues behind A's held EX and is granted only once A's
 * $DEQ runs at exit -- not a race that happens to resolve favourably. After
 * both exit clean, this harness reads EVAC.DAT back itself (RMS $GET, the
 * same public API) and asserts:
 *   - exactly A's record count + B's record count rows exist;
 *   - SEQ is the unbroken sequence 1..N across BOTH instances (B's first
 *     record continues at A's last seq + 1 -- the takeover-continuity
 *     property the item names);
 *   - every record in the first run of rows carries ONE pid, every record
 *     in the second run carries a DIFFERENT one (two distinct holders, not
 *     the same process writing twice).
 *
 * NO /dev/vms -> honest SKIP (77): EVACWL.EXE's own $ENQW would fail
 * SS$_NOSUCHDEV with no executive, so this harness checks that property
 * directly (via sys$crelnm, the first executive call it makes) rather than
 * exec a native image that can only fail the same way less legibly.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <sys/wait.h>
#include <stdint.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "lnmdef.h"
#include "vms_kif.h"
#include "rms.h"

#define EXIT_SKIP 77
#define WAIT_TIMEOUT_MS 30000
#define ODS2_UNIT "VDA0:"
#define EVACWL_IMAGE "/vms/SYS0/SYSCOMMON/SYSEXE/EVACWL.EXE"

#define RECLEN       64
#define FLD_PID_OFF  16
#define FLD_PID_LEN  10
#define FLD_SEQ_OFF  26
#define FLD_SEQ_LEN  10

static int pass = 0, fail = 0;

#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* DEFINE a name in LNM$SYSTEM (the executive-resident table, visible to any
 * process -- test_syssvc_lnm_crossproc.c's own proof). */
static uint32_t def_system(const char *name, const char *val)
{
    struct dsc$descriptor_s td = mkdsc("LNM$SYSTEM");
    struct dsc$descriptor_s nd = mkdsc(name);
    struct item_list_3 il[2];
    memset(il, 0, sizeof(il));
    il[0].buflen    = (uint16_t)strlen(val);
    il[0].item_code = LNM$_STRING;
    il[0].bufaddr   = (void *)val;
    return sys$crelnm(NULL, &td, &nd, NULL, il);
}

static void erase_evac_dat(void)
{
    char spec[128];
    struct FAB fab = cc$rms_fab;
    snprintf(spec, sizeof(spec), "%s[OVMXDIR]EVAC.DAT", ODS2_UNIT);
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

/* Read EVAC.DAT back through the public RMS API: *pids receives each
 * record's PID field, *seqs its SEQ field, up to maxrecs; returns the
 * count actually read. */
static int read_evac_dat(unsigned long *pids, unsigned long *seqs, int maxrecs)
{
    char spec[128];
    struct FAB fab = cc$rms_fab;
    struct RAB rab;
    int n = 0;

    snprintf(spec, sizeof(spec), "%s[OVMXDIR]EVAC.DAT", ODS2_UNIT);
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_fac = FAB$M_GET;
    fab.fab$b_shr = FAB$M_GET | FAB$M_PUT;

    if (!(sys$open(&fab, 0, 0) & 1))
        return -1;

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$b_rac = RAB$C_SEQ;
    if (!(sys$connect(&rab, 0, 0) & 1)) {
        sys$close(&fab, 0, 0);
        return -1;
    }

    while (n < maxrecs) {
        char rec[RECLEN];
        rab.rab$l_ubf = rec;
        rab.rab$w_usz = RECLEN;
        uint32_t st = sys$get(&rab, 0, 0);
        if (st == RMS$_EOF)
            break;
        if (!(st & 1))
            break;
        char pidtxt[FLD_PID_LEN + 1], seqtxt[FLD_SEQ_LEN + 1];
        memcpy(pidtxt, rec + FLD_PID_OFF, FLD_PID_LEN); pidtxt[FLD_PID_LEN] = '\0';
        memcpy(seqtxt, rec + FLD_SEQ_OFF, FLD_SEQ_LEN); seqtxt[FLD_SEQ_LEN] = '\0';
        pids[n] = strtoul(pidtxt, NULL, 10);
        seqs[n] = strtoul(seqtxt, NULL, 10);
        n++;
    }

    sys$disconnect(&rab, 0, 0);
    sys$close(&fab, 0, 0);
    return n;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);

    printf("=== test_syssvc_ci6_evacwl (EVACWL takeover continuity, vms-06c) ===\n");

    if (!executive_present()) {
        uint32_t st = def_system("EVAC$DATA", "VDA0:[OVMXDIR]");
        printf("  INFO: sys$crelnm/system with no executive returned status %u\n", st);
        CHECK(!(st & 1),
              "parent: sys$crelnm does NOT report success when the executive was never reached");
        printf("=== test_syssvc_ci6_evacwl: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    uint32_t st = def_system("EVAC$DATA", "VDA0:[OVMXDIR]");
    CHECK(st & 1, "parent: EVAC$DATA defined in LNM$SYSTEM");

    erase_evac_dat();

    const int COUNT_A = 3, COUNT_B = 2;
    char a_count[8], b_count[8];
    snprintf(a_count, sizeof(a_count), "%d", COUNT_A);
    snprintf(b_count, sizeof(b_count), "%d", COUNT_B);

    pid_t pid_a = fork();
    if (pid_a < 0) { printf("  FAIL: fork() for instance A\n"); return 1; }
    if (pid_a == 0) {
        char *argv[] = { (char *)EVACWL_IMAGE, a_count, NULL };
        execv(EVACWL_IMAGE, argv);
        _exit(127);
    }

    pid_t pid_b = fork();
    if (pid_b < 0) { printf("  FAIL: fork() for instance B\n"); kill(pid_a, SIGKILL); return 1; }
    if (pid_b == 0) {
        char *argv[] = { (char *)EVACWL_IMAGE, (char *)"standby", b_count, NULL };
        execv(EVACWL_IMAGE, argv);
        _exit(127);
    }

    int rc_a = -1, rc_b = -1;
    {
        int ws;
        for (int i = 0; i < WAIT_TIMEOUT_MS / 50 && (rc_a < 0 || rc_b < 0); i++) {
            if (rc_a < 0) {
                pid_t w = waitpid(pid_a, &ws, WNOHANG);
                if (w == pid_a) rc_a = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
            }
            if (rc_b < 0) {
                pid_t w = waitpid(pid_b, &ws, WNOHANG);
                if (w == pid_b) rc_b = WIFEXITED(ws) ? WEXITSTATUS(ws) : -1;
            }
            if (rc_a >= 0 && rc_b >= 0) break;
            struct pollfd nothing = { .fd = -1, .events = 0 };
            poll(&nothing, 1, 50);
        }
    }
    if (rc_a < 0) { kill(pid_a, SIGKILL); waitpid(pid_a, NULL, 0); }
    if (rc_b < 0) { kill(pid_b, SIGKILL); waitpid(pid_b, NULL, 0); }

    CHECK(rc_a == 0, "instance A (direct EX) exited clean");
    CHECK(rc_b == 0, "instance B (standby NL->EX) exited clean, having waited for A");

    unsigned long pids[32], seqs[32];
    int n = read_evac_dat(pids, seqs, 32);
    CHECK(n == COUNT_A + COUNT_B,
          "EVAC.DAT holds exactly A's + B's record count");

    int seq_ok = (n == COUNT_A + COUNT_B);
    for (int i = 0; seq_ok && i < n; i++)
        if (seqs[i] != (unsigned long)(i + 1)) seq_ok = 0;
    CHECK(seq_ok, "SEQ is the unbroken sequence 1..N across both instances (takeover continuity)");

    int holder_ok = (n == COUNT_A + COUNT_B);
    if (holder_ok) {
        for (int i = 1; i < COUNT_A; i++)
            if (pids[i] != pids[0]) holder_ok = 0;
        for (int i = COUNT_A + 1; i < n; i++)
            if (pids[i] != pids[COUNT_A]) holder_ok = 0;
        if (pids[0] == pids[COUNT_A]) holder_ok = 0;
    }
    CHECK(holder_ok,
          "the first COUNT_A records share one PID, the rest a DIFFERENT one (a real takeover, not one process writing twice)");

    erase_evac_dat();

    printf("=== test_syssvc_ci6_evacwl: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
