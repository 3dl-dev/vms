/*
 * test_syssvc_delprc.c - sys$delprc actually resolves and terminates a
 * process THROUGH THE EXECUTIVE (vms-1a8), and enforces the DCL
 * Dictionary's GROUP/WORLD privilege rule for the process-target forms of
 * the STOP command it backs.
 *
 * THE FACADE THIS PROVES DEAD. Before this item, sys$delprc discarded its
 * prcnam argument outright and treated pidadr as a bare Linux pid number --
 * it never went near /dev/vms at all. That made DCL's STOP command
 * (cmd_stop, src/vmsdcl/dcl_cmd_process.c) unable to implement its
 * documented process-target forms: `(void)cmd;` ignored the target
 * entirely and self-exited the CALLING session, returning SS$_NORMAL as if
 * the named process had been stopped. tests/dcl/test_stop_facade_gate.sh
 * proves the negative half of that on any host (never $STATUS=1 for a
 * target STOP cannot even reach without an executive); THIS suite proves
 * the positive half that requires a real, insmod'd vms.ko: a NAMED target
 * process, created by ONE process, is deleted by a DIFFERENT one that only
 * ever gave the executive its name or its VMS process ID -- and is
 * afterward actually gone, not merely reported gone.
 *
 * PRIVILEGE (OpenVMS DCL Dictionary, STOP command; see the full citation in
 * sys$delprc's own comment, src/libvms/syssvc/sys_process.c): deleting a
 * process in the caller's own UIC group without GROUP requires SS$_NOPRIV,
 * and the target must be left ALIVE -- a privilege check that refuses the
 * call but still terminates the process would be worse than no check at
 * all. P3 below proves both halves against a real target, not just the
 * refusal.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be
 * opened -- the CI negative-control rig, never the product (Rule 9: PID 1
 * refuses to boot without the executive) -- this exercises the
 * no-fabricated-success checks in device_absent_checks() and exits
 * EXIT_SKIP (77), never a fake pass.
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

#define HOLD_SCRIPT     "/tmp/ovmx1a8_hold.sh"
#define SUBJECT_IMAGE   "/bin/sh"

#define TARGET_NAME     "OVMX1A8TGT1"   /* P1: stopped by name */
#define TARGET2_NAME    "OVMX1A8TGT2"   /* P2: stopped by /IDENTIFICATION */
#define TARGET3_NAME    "OVMX1A8TGT3"   /* P3: privless refusal target */
#define TARGET_SIG_NAME "OVMX904SIG"    /* P6: suspnd/resume/forcex by VMS pid */
#define ABSENT_NAME     "OVMX1A8NONE"   /* never created */

static struct dsc$descriptor_s str_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/* spawn_named - $CREPRC a long-lived (600s) named subprocess. Returns the
 * $CREPRC status; *out_pid is the EXECUTIVE-assigned VMS process ID. */
static uint32_t spawn_named(const char *prcnam, uint32_t *out_pid)
{
    struct dsc$descriptor_s img = str_dsc(SUBJECT_IMAGE);
    struct dsc$descriptor_s in  = str_dsc(HOLD_SCRIPT);
    struct dsc$descriptor_s nd  = str_dsc(prcnam);

    *out_pid = 0;
    return sys$creprc(out_pid, &img, &in, NULL, NULL, NULL, NULL, &nd,
                      0, 0, 0, 0);
}

/* linux_pid_of - the Linux pid backing a VMS process, read from its row.
 * Two separate namespaces since vms-2b8: only the executive knows which
 * Linux task a VMS process ID names. Returns 0 if the row does not resolve. */
static uint32_t linux_pid_of(uint32_t vms_pid)
{
    struct vms_procinfo info;

    if (!(vms_kif_getjpi_pid(vms_pid, &info) & 1))
        return 0;
    return info.linux_pid;
}

/* proc_state - the single state character from /proc/<lpid>/stat. The comm
 * field is parenthesized and may itself contain spaces/parens, so the state
 * is the character two past the LAST ')'. Returns 0 if it cannot be read. */
static char proc_state(uint32_t lpid)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/stat", (unsigned)lpid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    char buf[512];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    if (!rp || !rp[1] || !rp[2]) return 0;
    return rp[2];               /* ") <state>" */
}

/* wait_proc_stopped - a bounded poll (OBSERVED condition, never a fixed sleep,
 * matching wait_for_exec above) for the target's Linux task to reach ('T')
 * or leave (running/sleeping) the stopped state after a SIGSTOP/SIGCONT is
 * delivered asynchronously. Returns 0 on success, -1 on timeout. */
static int wait_proc_stopped(uint32_t lpid, int want_stopped)
{
    for (int i = 0; i < 400; i++) {          /* up to ~10s at 25ms */
        char s = proc_state(lpid);
        int stopped = (s == 'T' || s == 't');
        if (stopped == want_stopped) return 0;
        usleep(25000);
    }
    return -1;
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

/* wait_for_exec - bounded poll (an OBSERVED condition, never a fixed sleep)
 * for the subject to have actually exec'd its image, so the target is
 * genuinely running (not just registered) before a test tries to stop it. */
static int wait_for_exec(uint32_t pid, const char *want)
{
    char comm[64];
    for (int i = 0; i < 2000; i++) {          /* <= ~20s, then give up */
        if (comm_of(pid, comm, sizeof(comm)) == 0 && strcmp(comm, want) == 0)
            return 0;
        usleep(10000);
    }
    return -1;
}

/*
 * wait_and_reap - bounded wait for OUR OWN CHILD to actually terminate,
 * reaping it in the process. Returns 0 if it terminated (and was reaped)
 * within the bound, -1 on timeout -- in which case NOTHING has been
 * reaped, and the caller must still clean up (see reap() below).
 *
 * kill(pid, 0) == ESRCH is NOT the right test here, and an earlier version
 * of this file used it and got a FALSE FAIL on a delprc that had actually
 * worked: a SIGTERM'd child does not vanish from the pid table, it becomes
 * a ZOMBIE this process's own waitpid() has not yet reaped, and
 * kill(zombie_pid, 0) still returns 0 -- a zombie IS a live process-table
 * entry with a real pid. waitpid(WNOHANG) is the only test that means
 * "this specific child has actually terminated" for a process's OWN
 * subprocess (which is exactly what $CREPRC's non-detached form, spawn_named()
 * above, creates).
 */
static int wait_and_reap(uint32_t linux_pid)
{
    for (int i = 0; i < 2000; i++) {          /* <= ~20s, then give up */
        int st;
        pid_t r = waitpid((pid_t)linux_pid, &st, WNOHANG);
        if (r == (pid_t)linux_pid) return 0;
        if (r < 0 && errno == ECHILD) return 0;  /* reaped elsewhere already */
        usleep(10000);
    }
    return -1;
}

/* reap - wait out the target's zombie. BOUNDED, deliberately: it must
 * never trust that a preceding CHECK() already confirmed the target dead --
 * a regression in the code under test (a delprc that reports success
 * without actually killing anything, say) must fail this suite FAST, not
 * block here for up to the subject's own 600s `sleep` and take the whole
 * QEMU harness's outer timeout down with it as an unreadable hang instead
 * of a clean FAIL line. */
static void reap(uint32_t linux_pid)
{
    if (linux_pid == 0) return;
    for (int i = 0; i < 200; i++) {          /* ~2s */
        int st;
        pid_t r = waitpid((pid_t)linux_pid, &st, WNOHANG);
        if (r == (pid_t)linux_pid || (r < 0 && errno == ECHILD)) return;
        usleep(10000);
    }
    kill((pid_t)linux_pid, SIGKILL);
    int st;
    while (waitpid((pid_t)linux_pid, &st, 0) < 0 && errno == EINTR)
        ;
}

/*
 * device_absent_checks - the negative-control path. With no /dev/vms
 * present, NOTHING here may report success -- a public sys$ entry point
 * that fabricates an answer when it cannot reach the executive would turn
 * these green, and the CI negative-control job asserts on exit code 77.
 */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    struct dsc$descriptor_s nd = str_dsc(TARGET_NAME);
    uint32_t st = sys$delprc(NULL, &nd);
    CHECK(!(st & 1), "sys$delprc by name does not report success with no executive");

    uint32_t pid = 1;
    st = sys$delprc(&pid, NULL);
    CHECK(!(st & 1), "sys$delprc by pid does not report success with no executive");

    printf("=== test_syssvc_delprc: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);  /* line-buffer: a still-buffered
                                         * write must not splice into a
                                         * forked child's output */
    printf("=== test_syssvc_delprc: sys$delprc via the public sys$ API (vms-1a8) ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    /* The subject's script. It must block, or the subject exits before it
     * can be observed; `sleep` is a busybox applet installed by init.sh
     * (same convention as tests/qemu/test_syssvc_procnam.c). */
    FILE *hs = fopen(HOLD_SCRIPT, "w");
    if (!hs) {
        printf("  FAIL: cannot write %s\n", HOLD_SCRIPT);
        printf("=== test_syssvc_delprc: 0 passed, 1 failed ===\n");
        return 1;
    }
    fprintf(hs, "sleep 600\n");
    fclose(hs);
    chmod(HOLD_SCRIPT, 0644);

    /* ---- P1: STOP <name> -- a real target, resolved by NAME, actually
     * terminated, and confirmed gone from the executive's own table. ---- */
    {
        uint32_t vms_pid = 0, lpid = 0;

        uint32_t cst = spawn_named(TARGET_NAME, &vms_pid);
        CHECK(cst & 1, "P1: $CREPRC creates the named target");
        lpid = linux_pid_of(vms_pid);
        CHECK(lpid != 0, "P1: the target resolves to a real Linux pid");
        CHECK(wait_for_exec(lpid, "sh") == 0,
              "P1: the target has actually exec'd its image");
        CHECK(kill((pid_t)lpid, 0) == 0, "P1: the target is alive before STOP");

        struct dsc$descriptor_s nd = str_dsc(TARGET_NAME);
        uint32_t dst = sys$delprc(NULL, &nd);
        CHECK(dst == SS$_NORMAL, "P1: sys$delprc(name) returns SS$_NORMAL");

        int reaped = (wait_and_reap(lpid) == 0);
        CHECK(reaped, "P1: the target's Linux process actually terminated "
                      "(reaped, not merely signalled)");
        if (!reaped) reap(lpid);       /* force-cleanup so nothing lingers */

        struct vms_procinfo info;
        uint32_t gst = vms_kif_getjpi_pid(vms_pid, &info);
        CHECK(gst == SS$_NONEXPR,
              "P1: the executive's table no longer resolves the stopped "
              "target ($GETJPI -> SS$_NONEXPR, the same status SHOW SYSTEM "
              "reads to decide a row is gone)");
    }

    /* ---- P2: STOP/IDENTIFICATION=pid -- same proof, resolved by VMS
     * PROCESS ID instead of name. ---- */
    {
        uint32_t vms_pid = 0, lpid = 0;

        uint32_t cst = spawn_named(TARGET2_NAME, &vms_pid);
        CHECK(cst & 1, "P2: $CREPRC creates the second named target");
        lpid = linux_pid_of(vms_pid);
        CHECK(lpid != 0, "P2: the target resolves to a real Linux pid");
        CHECK(wait_for_exec(lpid, "sh") == 0,
              "P2: the target has actually exec'd its image");

        uint32_t dst = sys$delprc(&vms_pid, NULL);
        CHECK(dst == SS$_NORMAL, "P2: sys$delprc(pid) returns SS$_NORMAL");

        int reaped = (wait_and_reap(lpid) == 0);
        CHECK(reaped, "P2: the target's Linux process actually terminated "
                      "(reaped, not merely signalled)");
        if (!reaped) reap(lpid);       /* force-cleanup so nothing lingers */

        struct vms_procinfo info;
        uint32_t gst = vms_kif_getjpi_pid(vms_pid, &info);
        CHECK(gst == SS$_NONEXPR,
              "P2: the executive's table no longer resolves the stopped "
              "target");
    }

    /* ---- P3: PRIVILEGE -- a caller in the TARGET's own UIC group, with
     * GROUP (and WORLD) stripped from its privilege mask, is refused
     * SS$_NOPRIV, and the target is left ALIVE. A privilege check that
     * refuses the call but still kills the process would be worse than
     * none: it would tell the caller "no" while doing "yes" anyway. ---- */
    {
        uint32_t vms_pid = 0, lpid = 0;

        uint32_t cst = spawn_named(TARGET3_NAME, &vms_pid);
        CHECK(cst & 1, "P3: $CREPRC creates the privilege-test target");
        lpid = linux_pid_of(vms_pid);
        CHECK(lpid != 0, "P3: the target resolves to a real Linux pid");
        CHECK(wait_for_exec(lpid, "sh") == 0,
              "P3: the target has actually exec'd its image");

        /*
         * Neither the target nor the helper below calls setgid(): both stay
         * in group 0, the root parent's own UIC group -- exactly the
         * SAME-GROUP arrangement the Dictionary's GROUP-privilege rule
         * covers (the cross-group/WORLD case is already exercised
         * indirectly: vms_kif_getjpi_pid/prcnam refuse a cross-group
         * resolution outright, before sys$delprc's own check ever runs --
         * see that function's comment). Only the HELPER's own privilege
         * mask changes, by vms_kif_setident() below; the target's mask is
         * irrelevant to a check that gates the CALLER, not the victim.
         */
        int pipefd[2];
        if (pipe(pipefd) != 0) {
            CHECK(0, "P3: pipe() for the privilege-stripped helper");
        } else {
            pid_t hp = fork();
            if (hp == 0) {
                /* Child: strip GROUP and WORLD from its own privilege mask
                 * under a NEW username (identity-establishment requires one;
                 * this process's own PCB-less row has none yet), then try to
                 * delete TARGET3. */
                close(pipefd[0]);
                struct vms_procinfo self_info;
                uint32_t st = vms_kif_getjpi_self(&self_info);
                if (!(st & 1)) { uint32_t z = 0; write(pipefd[1], &z, 4); _exit(1); }

                uint64_t stripped = self_info.perm_privs &
                    ~(VMS_PRV_M_WORLD | (1ULL << VMS_PRV_V_GROUP));
                st = vms_kif_setident("OVMX1A8HELP", self_info.uic, stripped);
                if (!(st & 1)) { uint32_t z = 0; write(pipefd[1], &z, 4); _exit(1); }

                struct dsc$descriptor_s nd = str_dsc(TARGET3_NAME);
                uint32_t dst = sys$delprc(NULL, &nd);
                write(pipefd[1], &dst, 4);
                _exit(0);
            }
            close(pipefd[1]);
            uint32_t helper_status = 0;
            ssize_t r = read(pipefd[0], &helper_status, 4);
            close(pipefd[0]);
            int hst;
            while (waitpid(hp, &hst, 0) < 0 && errno == EINTR)
                ;

            CHECK(r == 4, "P3: the privilege-stripped helper reported a status");
            /* negctl: delprc-privcheck-bypassed */
            CHECK(helper_status == SS$_NOPRIV,
                  "P3: sys$delprc refuses a same-group target without "
                  "GROUP privilege (SS$_NOPRIV, DCL Dictionary STOP)");
            CHECK(kill((pid_t)lpid, 0) == 0,
                  "P3: the target is still ALIVE after the refused delete "
                  "-- a refusal that still killed it would be worse than "
                  "no check at all");
        }

        kill((pid_t)lpid, SIGKILL);
        reap(lpid);
    }

    /* ---- P4: nonexistent targets, by name and by pid, both draw the
     * authentic SS$_NONEXPR -- never a fabricated success, and never
     * confused with the privilege refusal above. ---- */
    {
        struct dsc$descriptor_s nd = str_dsc(ABSENT_NAME);
        uint32_t st = sys$delprc(NULL, &nd);
        CHECK(st == SS$_NONEXPR,
              "P4: sys$delprc(name) on a nonexistent process returns "
              "SS$_NONEXPR");

        /* A VMS process ID nothing has ever been assigned. assign_vms_pid
         * (src/kernel/vms_module.c) hands out small sequential values, so
         * this sentinel is never a live row without also being an
         * astronomically unlikely collision. */
        uint32_t bogus_pid = 0xEFFFFFFFu;
        st = sys$delprc(&bogus_pid, NULL);
        CHECK(st == SS$_NONEXPR,
              "P4: sys$delprc(pid) on a nonexistent process returns "
              "SS$_NONEXPR");
    }

    /* ---- P5 (vms-904): sys$forcex/suspnd/resume on a NONEXISTENT VMS pid
     * draw the same authentic SS$_NONEXPR. They used to cast the VMS pid
     * straight into kill() and return a fake SS$_NORMAL after signalling an
     * unrelated Linux process (or nothing). ---- */
    {
        uint32_t bogus_pid = 0xEFFFFFFFu;
        uint32_t st = sys$forcex(&bogus_pid, NULL, 0);
        CHECK(st == SS$_NONEXPR,
              "P5: sys$forcex(nonexistent VMS pid) returns SS$_NONEXPR, not a "
              "fake SS$_NORMAL from a mis-cast kill() (vms-904)");
        st = sys$suspnd(&bogus_pid, NULL);
        CHECK(st == SS$_NONEXPR,
              "P5: sys$suspnd(nonexistent VMS pid) returns SS$_NONEXPR (vms-904)");
        st = sys$resume(&bogus_pid, NULL);
        CHECK(st == SS$_NONEXPR,
              "P5: sys$resume(nonexistent VMS pid) returns SS$_NONEXPR (vms-904)");
    }

    /* ---- P6 (vms-904): sys$suspnd/resume/forcex BY VMS PID signal the
     * target's REAL Linux pid (executive-resolved), not the process at the
     * numeric value of the VMS pid. VMS pid != Linux pid, so the old mis-cast
     * signalled a different process (or nothing) and lied SS$_NORMAL. ---- */
    {
        uint32_t vms_pid = 0, lpid = 0;
        uint32_t cst = spawn_named(TARGET_SIG_NAME, &vms_pid);
        CHECK(cst & 1, "P6: $CREPRC creates the signal target");
        lpid = linux_pid_of(vms_pid);
        CHECK(lpid != 0, "P6: the target resolves to a real Linux pid");
        CHECK(wait_for_exec(lpid, "sh") == 0,
              "P6: the target reached its sh image");
        CHECK((uint32_t)lpid != vms_pid,
              "P6: VMS pid != Linux pid -- the numeric value the old mis-cast "
              "would have signalled is a DIFFERENT process");

        uint32_t sst = sys$suspnd(&vms_pid, NULL);
        CHECK(sst == SS$_NORMAL, "P6: sys$suspnd(child VMS pid) returns SS$_NORMAL");
        CHECK(wait_proc_stopped(lpid, 1) == 0,
              "P6: the child's REAL Linux process is STOPPED -- SIGSTOP hit the "
              "executive-resolved pid, not the mis-cast VMS-pid value");

        uint32_t rst = sys$resume(&vms_pid, NULL);
        CHECK(rst == SS$_NORMAL, "P6: sys$resume(child VMS pid) returns SS$_NORMAL");
        CHECK(wait_proc_stopped(lpid, 0) == 0,
              "P6: the child's REAL Linux process RESUMED -- SIGCONT hit the "
              "resolved pid");

        uint32_t fst = sys$forcex(&vms_pid, NULL, 0);
        CHECK(fst == SS$_NORMAL, "P6: sys$forcex(child VMS pid) returns SS$_NORMAL");

        kill((pid_t)lpid, SIGKILL);
        reap(lpid);
    }

    printf("=== test_syssvc_delprc: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
