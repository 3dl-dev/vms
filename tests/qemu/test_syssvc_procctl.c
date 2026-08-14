/*
 * test_syssvc_procctl.c - the process-CONTROL-by-pid services signal the
 * RIGHT process (vms-904, executive keystone vms-6b8). Covers sys$forcex,
 * sys$suspnd and sys$resume -- the residual control-by-pid services that,
 * unlike sys$delprc (proven by test_syssvc_delprc.c) and sys$wake (proven
 * cross-process by test_syssvc_hiber_ast.c), still cast their VMS-pid
 * argument straight into kill() -- and re-confirms sys$delprc and sys$wake
 * from the same file so the whole family's "resolve through the executive"
 * contract is exercised together.
 *
 * THE WRONG-PROCESS BUG THIS PROVES DEAD. A control-by-pid service is
 * handed a VMS PROCESS ID, which $CREPRC deliberately makes UNEQUAL to the
 * Linux pid backing the process (two namespaces since vms-2b8; VMS pids come
 * out of VMS_PID_BASE == 0x10000000, so a VMS pid is ALWAYS ~268M+, far
 * above any Linux pid and above /proc/sys/kernel/pid_max). The old
 * sys$forcex/$suspnd/$resume did `kill((pid_t)*pidadr, SIG)` -- signalling
 * whatever Linux process happened to wear that number, or (as here, where the
 * number is astronomically large) ESRCH'ing and returning SS$_NONEXPR while
 * the intended VMS process ran on untouched. Each service must instead
 * RESOLVE the VMS pid to its Linux pid through the executive (the same
 * resolve_control_target() path sys$delprc uses) before any signal.
 *
 * HOW EACH ASSERTION DISCRIMINATES FIXED FROM BUGGY. Every target below is
 * a real $CREPRC child whose VMS pid (>= 0x10000001) is a number no Linux
 * process can have. The FIXED service resolves it and signals the child's
 * real (small) Linux pid, so the child observably stops / resumes / exits
 * and the service returns SS$_NORMAL. The BUGGY raw-cast service calls
 * kill(268-million-something, SIG), which ESRCHes -> returns SS$_NONEXPR and
 * the child is untouched. The two outcomes are disjoint and deterministic:
 * no coincidental vms_pid==linux_pid collision is possible.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. With no executive the
 * services cannot resolve anything and MUST NOT fabricate success (INV-6);
 * device_absent_checks() asserts that and exits EXIT_SKIP (77), never a fake
 * pass -- the same negative-control contract every test_syssvc_* honors.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <errno.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do {                                  \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }   \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }   \
    } while (0)

#define HOLD_SCRIPT     "/tmp/ovmx904_hold.sh"
#define SUBJECT_IMAGE   "/bin/sh"
#define HOLD_SECS       "15"             /* outlives the ms-scale assertions */

#define TGT_SUSP        "OVMX904SUSP"    /* P1/P2: suspended then resumed  */
#define TGT_FX_PID      "OVMX904FXP"     /* P3:    force-exit by VMS pid    */
#define TGT_FX_NAME     "OVMX904FXN"     /* P4:    force-exit by name       */
#define TGT_DELPRC      "OVMX904DEL"     /* P5:    delprc re-confirm        */
#define ABSENT_NAME     "OVMX904NONE"    /* never created                   */

/* A VMS pid nothing is ever assigned. assign_vms_pid hands out
 * VMS_PID_BASE (0x10000000) + a small sequential counter, so this sentinel
 * near the top of the range is never a live row. */
#define BOGUS_VMS_PID   0x1FFFFFFFu

/* Every hold child's Linux pid is registered here the moment it resolves, so
 * a single teardown at the end can reap ALL of them belt-and-suspenders --
 * nothing may orphan into the next suite in the shared single-boot guest and
 * eat the whole-VM wall (the leak this suite must not introduce). Each phase
 * still reaps its own inline; this is the safety net for any path that did
 * not reach its inline reap (e.g. an early CHECK failure). */
static uint32_t g_hold[8];
static int      g_nhold;
static void track_hold(uint32_t linux_pid)
{
    if (linux_pid != 0 && g_nhold < (int)(sizeof(g_hold) / sizeof(g_hold[0])))
        g_hold[g_nhold++] = linux_pid;
}

static struct dsc$descriptor_s str_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* spawn_named - $CREPRC a short-lived named hold subprocess. *out_pid is the
 * EXECUTIVE-assigned VMS process ID (>= 0x10000001). The hold script `exec`s
 * sleep so /bin/sh REPLACES itself with the sleep -- one process, not sh+sleep
 * -- so the pid the test tracks IS the sleeper and reaping it leaves no orphan.
 * The hold is deliberately SHORT (HOLD_SECS): the assertions complete in ms,
 * and a short cap means even a hold child that somehow escaped its reap dies on
 * its own well within the suite rather than lingering into the next one. */
static uint32_t spawn_named(const char *prcnam, uint32_t *out_pid)
{
    struct dsc$descriptor_s img = str_dsc(SUBJECT_IMAGE);
    struct dsc$descriptor_s in  = str_dsc(HOLD_SCRIPT);
    struct dsc$descriptor_s nd  = str_dsc(prcnam);

    *out_pid = 0;
    return sys$creprc(out_pid, &img, &in, NULL, NULL, NULL, NULL, &nd,
                      0, 0, 0, 0);
}

/* linux_pid_of - the Linux pid backing a VMS process, read from its
 * executive row. Only the executive knows this mapping. 0 if unresolved. */
static uint32_t linux_pid_of(uint32_t vms_pid)
{
    struct vms_procinfo info;
    if (!(vms_kif_getjpi_pid(vms_pid, &info) & 1))
        return 0;
    return info.linux_pid;
}

static int comm_of(uint32_t pid, char *out, size_t outsz)
{
    char path[64];
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%u/comm", (unsigned)pid);
    f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)outsz, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return 0;
}

/*
 * proc_state - the single-letter run state from /proc/<pid>/stat (field 3):
 * 'R'/'S' running or sleeping, 'T' stopped (SIGSTOP took), 'Z' zombie,
 * 'X' dead. The comm field (field 2) is parenthesised and may itself
 * contain spaces and ')', so we take the state as the first non-space
 * character AFTER THE LAST ')'. Returns -1 if the process is gone.
 */
static int proc_state(uint32_t pid, char *st)
{
    char path[64], buf[1024];
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%u/stat", (unsigned)pid);
    f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return -1; }
    fclose(f);

    char *rp = strrchr(buf, ')');
    if (!rp) return -1;
    rp++;
    while (*rp == ' ') rp++;
    *st = *rp;
    return 0;
}

/* wait_for_state - bounded poll (an OBSERVED condition, never a fixed sleep)
 * for the process to reach a given run state. 0 on success, -1 on timeout. */
static int wait_for_state(uint32_t pid, char want)
{
    for (int i = 0; i < 2000; i++) {          /* <= ~20s */
        char st;
        if (proc_state(pid, &st) == 0 && st == want) return 0;
        usleep(10000);
    }
    return -1;
}

/* wait_for_notstate - bounded poll for the process to LEAVE a given state
 * (used to prove RESUME actually un-stopped it). */
static int wait_for_notstate(uint32_t pid, char notwant)
{
    for (int i = 0; i < 2000; i++) {          /* <= ~20s */
        char st;
        if (proc_state(pid, &st) == 0 && st != notwant) return 0;
        usleep(10000);
    }
    return -1;
}

/* wait_for_exec - poll until the subject has actually exec'd its image, so
 * the target is genuinely running before a test tries to control it. */
static int wait_for_exec(uint32_t pid, const char *want)
{
    char comm[64];
    for (int i = 0; i < 2000; i++) {          /* <= ~20s */
        if (comm_of(pid, comm, sizeof(comm)) == 0 && strcmp(comm, want) == 0)
            return 0;
        usleep(10000);
    }
    return -1;
}

/* wait_and_reap - bounded wait for OUR OWN CHILD to terminate, reaping it.
 * waitpid(WNOHANG) -- not kill(pid,0) -- is the only test that means "this
 * child has actually terminated": a signalled child becomes a ZOMBIE that
 * kill(pid,0) still sees (see test_syssvc_delprc.c's long note on this). */
static int wait_and_reap(uint32_t linux_pid)
{
    for (int i = 0; i < 2000; i++) {          /* <= ~20s */
        int st;
        pid_t r = waitpid((pid_t)linux_pid, &st, WNOHANG);
        if (r == (pid_t)linux_pid) return 0;
        if (r < 0 && errno == ECHILD) return 0;
        usleep(10000);
    }
    return -1;
}

/* reap - force-clean a target so nothing lingers past the suite. BOUNDED so
 * a code regression fails FAST rather than blocking on the subject's 600s. */
static void reap(uint32_t linux_pid)
{
    if (linux_pid == 0) return;
    kill((pid_t)linux_pid, SIGCONT);   /* in case it was left stopped */
    kill((pid_t)linux_pid, SIGKILL);
    for (int i = 0; i < 200; i++) {    /* ~2s */
        int st;
        pid_t r = waitpid((pid_t)linux_pid, &st, WNOHANG);
        if (r == (pid_t)linux_pid || (r < 0 && errno == ECHILD)) return;
        usleep(10000);
    }
    int st;
    while (waitpid((pid_t)linux_pid, &st, 0) < 0 && errno == EINTR)
        ;
}

/*
 * device_absent_checks - the negative-control path. With no /dev/vms, NO
 * control-by-pid service may report success: it cannot resolve a VMS pid,
 * and a service that signalled a raw pid number anyway would be the exact
 * wrong-process bug (INV-6). The CI negative-control job asserts exit 77.
 */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    uint32_t pid = BOGUS_VMS_PID;

    uint32_t st = sys$forcex(&pid, NULL, 0);
    CHECK(!(st & 1), "sys$forcex by pid does not report success with no executive");

    st = sys$suspnd(&pid, NULL);
    CHECK(!(st & 1), "sys$suspnd by pid does not report success with no executive");

    st = sys$resume(&pid, NULL);
    CHECK(!(st & 1), "sys$resume by pid does not report success with no executive");

    struct dsc$descriptor_s nd = str_dsc(TGT_SUSP);
    st = sys$suspnd(NULL, &nd);
    CHECK(!(st & 1), "sys$suspnd by name does not report success with no executive");

    printf("=== test_syssvc_procctl: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_procctl: control-by-pid signals the RIGHT process "
           "(vms-904) ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    FILE *hs = fopen(HOLD_SCRIPT, "w");
    if (!hs) {
        printf("  FAIL: cannot write %s\n", HOLD_SCRIPT);
        printf("=== test_syssvc_procctl: 0 passed, 1 failed ===\n");
        return 1;
    }
    /* exec: sh becomes the sleep, so the tracked pid is the only process and
     * killing it orphans nothing. Short hold (see HOLD_SECS / spawn_named). */
    fprintf(hs, "exec sleep " HOLD_SECS "\n");
    fclose(hs);
    chmod(HOLD_SCRIPT, 0644);

    /* ---- P1: $SUSPND by VMS pid stops the RIGHT process. ---- */
    uint32_t susp_vms = 0, susp_lpid = 0;
    {
        uint32_t cst = spawn_named(TGT_SUSP, &susp_vms);
        CHECK(cst & 1, "P1: $CREPRC creates the suspend/resume target");
        susp_lpid = linux_pid_of(susp_vms);
        CHECK(susp_lpid != 0, "P1: the target resolves to a real Linux pid");
        track_hold(susp_lpid);
        /* The premise the wrong-process bug fed on: the VMS pid the control
         * service is handed is NOT the Linux pid, and (VMS_PID_BASE) is a
         * number no Linux process can even have. */
        CHECK(susp_vms != susp_lpid && susp_vms >= 0x10000000u,
              "P1: the target's VMS pid differs from its Linux pid and is "
              "above every possible Linux pid ($CREPRC namespace gap)");
        CHECK(wait_for_exec(susp_lpid, "sh") == 0,
              "P1: the target has actually exec'd its image");
        char st0 = 0;
        CHECK(proc_state(susp_lpid, &st0) == 0 && st0 != 'T',
              "P1: the target is running (not stopped) before $SUSPND");

        uint32_t sst = sys$suspnd(&susp_vms, NULL);
        /* negctl: suspnd-vmspid-not-resolved */
        CHECK(sst == SS$_NORMAL, "P1: sys$suspnd(vms_pid) returns SS$_NORMAL");
        CHECK(wait_for_state(susp_lpid, 'T') == 0,
              "P1: the RESOLVED target is STOPPED after $SUSPND -- VMS pid resolved to its Linux pid, not cast into kill()");
    }

    /* ---- P2: $RESUME by VMS pid un-stops the RIGHT process. ---- */
    {
        uint32_t rst = sys$resume(&susp_vms, NULL);
        CHECK(rst == SS$_NORMAL, "P2: sys$resume(vms_pid) returns SS$_NORMAL");
        CHECK(wait_for_notstate(susp_lpid, 'T') == 0,
              "P2: the RESOLVED target left the STOPPED state after $RESUME");
        reap(susp_lpid);
    }

    /* ---- P3: $FORCEX by VMS pid force-exits the RIGHT process. ---- */
    {
        uint32_t vms_pid = 0, lpid = 0;
        uint32_t cst = spawn_named(TGT_FX_PID, &vms_pid);
        CHECK(cst & 1, "P3: $CREPRC creates the force-exit-by-pid target");
        lpid = linux_pid_of(vms_pid);
        CHECK(lpid != 0, "P3: the target resolves to a real Linux pid");
        track_hold(lpid);
        CHECK(wait_for_exec(lpid, "sh") == 0,
              "P3: the target has actually exec'd its image");

        uint32_t fst = sys$forcex(&vms_pid, NULL, 0);
        CHECK(fst == SS$_NORMAL, "P3: sys$forcex(vms_pid) returns SS$_NORMAL");
        int reaped = (wait_and_reap(lpid) == 0);
        CHECK(reaped, "P3: the RESOLVED target actually exited after $FORCEX "
                      "(reaped, not merely signalled)");
        if (!reaped) reap(lpid);

        struct vms_procinfo info;
        uint32_t gst = vms_kif_getjpi_pid(vms_pid, &info);
        CHECK(gst == SS$_NONEXPR,
              "P3: the executive no longer resolves the force-exited target");
    }

    /* ---- P4: $FORCEX by NAME resolves the same way and hits the RIGHT
     * process (the prcnam path, which the old service discarded outright). ---- */
    {
        uint32_t vms_pid = 0, lpid = 0;
        uint32_t cst = spawn_named(TGT_FX_NAME, &vms_pid);
        CHECK(cst & 1, "P4: $CREPRC creates the force-exit-by-name target");
        lpid = linux_pid_of(vms_pid);
        CHECK(lpid != 0, "P4: the target resolves to a real Linux pid");
        track_hold(lpid);
        CHECK(wait_for_exec(lpid, "sh") == 0,
              "P4: the target has actually exec'd its image");

        struct dsc$descriptor_s nd = str_dsc(TGT_FX_NAME);
        uint32_t fst = sys$forcex(NULL, &nd, 0);
        CHECK(fst == SS$_NORMAL, "P4: sys$forcex(name) returns SS$_NORMAL");
        int reaped = (wait_and_reap(lpid) == 0);
        CHECK(reaped, "P4: the named target actually exited after $FORCEX");
        if (!reaped) reap(lpid);
    }

    /* ---- P5: $DELPRC re-confirm -- still resolves and terminates the RIGHT
     * process through the executive after the shared-helper refactor. ---- */
    {
        uint32_t vms_pid = 0, lpid = 0;
        uint32_t cst = spawn_named(TGT_DELPRC, &vms_pid);
        CHECK(cst & 1, "P5: $CREPRC creates the delprc re-confirm target");
        lpid = linux_pid_of(vms_pid);
        CHECK(lpid != 0, "P5: the target resolves to a real Linux pid");
        track_hold(lpid);
        CHECK(wait_for_exec(lpid, "sh") == 0,
              "P5: the target has actually exec'd its image");

        uint32_t dst = sys$delprc(&vms_pid, NULL);
        CHECK(dst == SS$_NORMAL, "P5: sys$delprc(vms_pid) returns SS$_NORMAL");
        int reaped = (wait_and_reap(lpid) == 0);
        CHECK(reaped, "P5: the target actually terminated after $DELPRC");
        if (!reaped) reap(lpid);

        struct vms_procinfo info;
        uint32_t gst = vms_kif_getjpi_pid(vms_pid, &info);
        CHECK(gst == SS$_NONEXPR,
              "P5: the executive no longer resolves the deleted target");
    }

    /* ---- P6: $WAKE re-confirm -- routes through the executive by VMS pid
     * (find_by_vms_pid), so an absent VMS pid draws the executive's own
     * SS$_NONEXPR rather than a fabricated success. The POSITIVE cross-process
     * wake (a hibernating target actually woken) is proven by
     * test_syssvc_hiber_ast.c; here we prove $WAKE does not fake success for a
     * process the executive cannot resolve. ---- */
    {
        uint32_t bogus = BOGUS_VMS_PID;
        uint32_t wst = sys$wake(&bogus, NULL);
        CHECK(wst == SS$_NONEXPR,
              "P6: sys$wake on a nonexistent VMS pid returns SS$_NONEXPR "
              "(executive-resolved, never a fabricated success)");
    }

    /* ---- P7: nonexistent targets draw the authentic SS$_NONEXPR from every
     * control-by-pid service -- never a fabricated success, and never a
     * signal to whatever Linux process wears the numeric id. ---- */
    {
        uint32_t bogus = BOGUS_VMS_PID;

        uint32_t st = sys$forcex(&bogus, NULL, 0);
        CHECK(st == SS$_NONEXPR,
              "P7: sys$forcex(pid) on a nonexistent process returns SS$_NONEXPR");

        st = sys$suspnd(&bogus, NULL);
        CHECK(st == SS$_NONEXPR,
              "P7: sys$suspnd(pid) on a nonexistent process returns SS$_NONEXPR");

        st = sys$resume(&bogus, NULL);
        CHECK(st == SS$_NONEXPR,
              "P7: sys$resume(pid) on a nonexistent process returns SS$_NONEXPR");

        struct dsc$descriptor_s nd = str_dsc(ABSENT_NAME);
        st = sys$suspnd(NULL, &nd);
        CHECK(st == SS$_NONEXPR,
              "P7: sys$suspnd(name) on a nonexistent process returns SS$_NONEXPR");
    }

    /* Teardown: belt-and-suspenders reap of EVERY hold child, so nothing this
     * suite created can orphan into the next suite in the shared single-boot
     * guest. reap() is idempotent (a target already reaped inline draws ECHILD
     * and returns at once), so this is safe over the whole registry. */
    for (int i = 0; i < g_nhold; i++)
        reap(g_hold[i]);

    printf("=== test_syssvc_procctl: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
