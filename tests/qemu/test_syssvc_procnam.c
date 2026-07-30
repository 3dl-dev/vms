/*
 * test_syssvc_procnam.c - The executive process table through the PUBLIC
 * sys$ API: $CREPRC names a process, $GETJPI resolves one BY NAME, and the
 * table enumerates (vms-8019).
 *
 * WHY THIS SUITE EXISTS, AND WHAT IT IS GUARDING AGAINST.
 *
 * tests/qemu/test_kmod_procnam.c already proves the KERNEL side: the table
 * in src/kernel/vms_proctab.c holds a name, scopes it to a UIC group, and
 * survives execve(). It proves nothing at all about whether the userspace
 * system services CALL it. Before this item they did not:
 *
 *   - sys$getjpi ignored both pidadr and prcnam and answered every question
 *     out of the CALLER's own PCB, so asking about another process returned
 *     the asker's own identity, and resolving by name was not implemented.
 *   - sys$creprc set the child's name in a PCB that execve() destroys, so
 *     the name never reached the activated image or any other process.
 *   - SHOW SYSTEM printed exactly ONE row -- the calling process -- and
 *     FABRICATED that row from getpid() when the PCB was empty.
 *
 * Every one of those passes a single-process test perfectly. That is the
 * signature of the facade class this epic exists to delete (CLAUDE.md
 * Rule 11): a per-process fake reports success while sharing nothing. So
 * every assertion below is A-WRITES / B-READS -- the name is written by one
 * Linux process and read back by a DIFFERENT one, through the public API.
 *
 * P1-P10 cover JPI$_PID and JPI$_PRCNAM. Block P11 covers the other three
 * item codes this item rewrote -- JPI$_UIC, JPI$_CPUTIM and JPI$_USERNAME --
 * against a helper that sits in a DIFFERENT UIC GROUP, burns its OWN CPU and
 * stamps its OWN authenticated user name, because on a rig where every
 * process is root and idle and nameless those three answers read identically
 * whether they come from the executive's row or from the caller itself. A
 * test that cannot tell the two apart is not coverage.
 *
 * THE LONG-LIVED SUBJECT PROCESS. $CREPRC's image is exec'd with no
 * arguments, so the subject is /bin/sh with its stdin redirected (by
 * $CREPRC's own input descriptor) to a script that sleeps. That choice is
 * load-bearing, not convenience: /bin/sh knows nothing about VMS and never
 * touches /dev/vms, so when the parent resolves it BY NAME afterwards, the
 * name it finds can only have come from the executive's table -- it
 * survived image activation with no userspace carrier of any kind. This is
 * the direct refutation of the rejected VMS_PRCNAM environment-variable
 * "fix", which could only ever tell the image its own name.
 *
 * Requires a real, insmod'd vms.ko at /dev/vms. If /dev/vms cannot be
 * opened -- which happens ONLY in the CI negative-control rig, never in the
 * product (vms-0ff: PID 1 refuses to boot without the executive) -- it
 * exercises the no-fabricated-success checks in main() and exits with
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
#include <ctype.h>
#include <errno.h>
#include <time.h>

#include <poll.h>
#include <sys/ptrace.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "ovmx_status.h"
#include "jpidef.h"
#include "lib$routines.h"
#include "vms_kif.h"

#define EXIT_SKIP 77

static int pass = 0;
static int fail = 0;

#define CHECK(cond, msg) do {                       \
        if (cond) { pass++; printf("  PASS: %s\n", (msg)); }  \
        else      { fail++; printf("  FAIL: %s\n", (msg)); }  \
    } while (0)

/* A process name and a name one character too long. VMS process names are
 * 1-15 characters; the oracle transcript for the boundary is in the
 * VMS_PRCNAM_XFER comment in src/kernel/vms_ioctl.h (VAX1, OpenVMS VAX
 * V7.3: 15 accepted, 16 rejected %SET-E-NOTSET / -SYSTEM-F-IVLOGNAM, and
 * the existing name left UNCHANGED -- no truncation, no partial apply). */
#define SUBJECT_NAME  "OVMX8019SUBJ"          /* 12 chars */
#define ABSENT_NAME   "OVMX8019NONE"          /* never created */
#define LEN15_NAME    "OVMX8019LEN15A"        /* 14 */
#define LEN16_NAME    "OVMX8019LEN15AB"       /* 15 -- legal */
#define LEN17_NAME    "OVMX8019LEN15ABC"      /* 16 -- one too long */

#define HOLD_SCRIPT   "/tmp/ovmx8019_hold.sh"
#define SUBJECT_IMAGE "/bin/sh"

/* The UIC group the P11 helper moves into. OVMX maps UIC [group,member]
 * onto the task's [gid,uid] (src/kernel/vms_module.c vms_proc_register),
 * so a helper that setgid()s to this value lands in a different UIC group
 * from the root parent (group 0) while staying uid 0 -- which it must, or
 * it loses the privilege to open /dev/vms at all. Same value and same
 * technique as tests/qemu/test_kmod_procnam.c's cross-group case, one
 * layer down. */
#define ALT_UIC_GROUP 300

/* CPU each helper burn consumes, measured by the helper itself with
 * clock() -- CPU time, not wall time, so nothing here is paced by a guess
 * about how fast an emulated guest runs (a fixed-sleep test is a flaky
 * test). Half a second of CPU is ~50 JPI$_CPUTIM units, two orders of
 * magnitude above the 10ms resolution the item code reports in. */
#define BURN_CLOCKS   (CLOCKS_PER_SEC / 2)

/* Authenticated user names stamped on the executive's rows (vms-2b8's
 * vms_kif_setident). Two distinct names, one per process, so a reader
 * that answers about the wrong row is caught by the name it returns. */
#define CALLER_USERNAME "OVMXCALLER"
#define HELPER_USERNAME "OVMXHELPER"

/* ---- P12: SHOW SYSTEM run by a process that may NOT read every row ----
 *
 * The redaction path (src/kernel/vms_proctab.c proc_fill_info) had never
 * been executed by any test, because every process in this rig is uid 0 /
 * gid 0 with CAP_SYS_ADMIN, so vms_proc_may_read() always said yes. P12
 * builds the one arrangement that reaches it through the USER-VISIBLE
 * command: a SHOW SYSTEM whose caller is in its own UIC group and holds no
 * WORLD privilege, looking at a process in a THIRD group. */
/* ---- P13: a signal caught by the CALLER of $CREPRC ------------------
 *
 * The creation handshake's pipe read used to be a bare read(), so ANY
 * signal delivered to the caller through a handler without SA_RESTART
 * made it return -1/EINTR -- which $CREPRC then read as "the child died
 * before reporting" and answered with OVMX$_PRCLOST, a SEVERE status
 * asserting that no process was created, for a process that WAS created
 * and IS in the executive's table. It then reaped that live process,
 * blocking for as long as the created image ran.
 *
 * The production trigger is ordinary: DCL installs its interactive
 * SIGINT/SIGQUIT handlers with sa_flags = 0 (no SA_RESTART) in
 * src/vmsdcl/dcl_main.c, and DCL is the process that calls $CREPRC (RUN,
 * RUN/DETACHED), so a Ctrl-C landing in the handshake window is the
 * real-world case.
 *
 * HOW THE WINDOW IS HIT, AND WHY IT IS NOT A RACE. The first version of
 * this block used a repeating interval timer, which is how the defect was
 * originally demonstrated on a native host (788 of 4000 calls). MEASURED
 * ON THIS RIG, THAT DOES NOT WORK: one run delivered 4716 signals across
 * 6 $CREPRC calls and interrupted the read ZERO times, and 18 calls across
 * three runs never reached it once. The read window is the time the caller
 * spends blocked waiting for the child's report, and under QEMU that is a
 * fraction of a millisecond out of a ~20 ms call -- a few percent per call.
 * A detector that fires a few percent of the time is a flaky gate, which
 * is a broken gate, and it was very nearly reported here as a proof: one
 * early "hang" under the mutation was the timer storm's own 13-fold
 * run-to-run variance, not the defect.
 *
 * So the window is not chased, it is HELD OPEN. A tracer process runs the
 * probe under ptrace with PTRACE_O_TRACEFORK, so every process $CREPRC
 * forks is stopped at birth. While it is stopped it cannot write its
 * report, so the caller's read is blocked and STAYS blocked; the tracer
 * waits for the caller to actually be sleeping (an observed condition in
 * /proc, not a delay), delivers the signal, and only then releases the
 * child. Ordering is enforced by ptrace rather than by timing, so the
 * arrangement is the same on every run and on every machine speed.
 */
#define SIGPROBE_NAME_PFX   "OVMX8019SG"   /* + 2 digits = 12 chars */

/* Iterations. Detection no longer depends on catching anything, so a
 * handful is enough; each costs a fork + exec of /bin/sh under emulation.
 * SIGPROBE_MIN_ARRANGED is the ARRANGEMENT check -- how many calls the
 * tracer actually managed to interrupt. It is not the detector; it is what
 * stops a green result from meaning "the condition never arose". */
#define SIGPROBE_ITERS         6
#define SIGPROBE_MIN_ARRANGED  3

/* How long the tracer waits for the caller to be blocked, polling
 * /proc/<pid>/stat. Bounded so a probe that never sleeps cannot wedge the
 * tracer; the consequence of giving up is one un-arranged call, which the
 * arrangement check counts. */
#define SIGPROBE_SLEEP_POLLS   2000
#define SIGPROBE_SLEEP_POLL_US 1000

/* THE NO-HANG BOUND, and why this number.
 *
 * It has a hard ceiling above it and a measurement below it:
 *   CEILING  tests/qemu/run_tests.sh gives the WHOLE guest 120 s. A bound
 *            at or near that is not a bound at all -- the guest is killed
 *            mid-probe and the suite prints no verdict, which is a harness
 *            timeout, not a test failure. Measured: with the bound at
 *            120000 the injected control produced NO verdict line for this
 *            suite instead of a FAIL.
 *   FLOOR    the probe's own measured cost, printed on every run as part
 *            of the P13 line so the margin is visible rather than claimed.
 *            Without a signal storm the six iterations cost a few hundred
 *            milliseconds on aarch64 TCG with no KVM.
 * If the printed elapsed figure ever approaches this bound, cut
 * SIGPROBE_ITERS first; raising the bound alone eventually hits the
 * ceiling and turns the control back into a harness timeout. */
#define SIGPROBE_BOUND_MS   20000

#define XGRP_UIC_GROUP   302      /* the row the SHOW SYSTEM caller may not read */
#define XGRP_NAME        "OVMX8019XGRP"
#define SHOW_UIC_GROUP   301      /* the SHOW SYSTEM caller's own group */
#define SHOW_NAME        "OVMX8019SHOW"
#define SHOWER_USERNAME  "OVMXSHOWER"
#define SETUP_FAIL_MARK  "OVMX8019SETUPFAIL"

static struct dsc$descriptor_s str_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/*
 * getjpi_prcnam_of - resolve a process BY VMS PID and return its name.
 * Returns the $GETJPI status; name[] is only meaningful on success.
 */
static uint32_t getjpi_prcnam_of(uint32_t vms_pid, char *name, size_t namesz)
{
    struct item_list_3 items[2];
    uint16_t len = 0;

    memset(name, 0, namesz);
    memset(items, 0, sizeof(items));
    items[0].buflen    = (uint16_t)(namesz - 1);
    items[0].item_code = JPI$_PRCNAM;
    items[0].bufaddr   = name;
    items[0].retlen    = &len;

    return sys$getjpi(0, &vms_pid, NULL, items, NULL, NULL, 0);
}

/*
 * getjpi_pid_of - resolve a process BY NAME and return its VMS PID.
 */
static uint32_t getjpi_pid_of(const char *prcnam, uint32_t *out_pid)
{
    struct dsc$descriptor_s nd = str_dsc(prcnam);
    struct item_list_3 items[2];
    uint16_t len = 0;

    *out_pid = 0;
    memset(items, 0, sizeof(items));
    items[0].buflen    = sizeof(uint32_t);
    items[0].item_code = JPI$_PID;
    items[0].bufaddr   = out_pid;
    items[0].retlen    = &len;

    return sys$getjpi(0, NULL, &nd, items, NULL, NULL, 0);
}

/*
 * getjpi_u32_of / getjpi_str_of - read ONE item code about a process
 * selected BY VMS PID. Used by block P11 for the three item codes this
 * item rewrote to read the executive's row (JPI$_UIC, JPI$_CPUTIM,
 * JPI$_USERNAME) rather than the caller's private PCB.
 */
static uint32_t getjpi_u32_of(uint32_t vms_pid, uint32_t item_code,
                              uint32_t *out)
{
    struct item_list_3 items[2];
    uint16_t len = 0;

    *out = 0;
    memset(items, 0, sizeof(items));
    items[0].buflen    = sizeof(uint32_t);
    items[0].item_code = (uint16_t)item_code;
    items[0].bufaddr   = out;
    items[0].retlen    = &len;

    return sys$getjpi(0, &vms_pid, NULL, items, NULL, NULL, 0);
}

static uint32_t getjpi_str_of(uint32_t vms_pid, uint32_t item_code,
                              char *out, size_t outsz)
{
    struct item_list_3 items[2];
    uint16_t len = 0;

    memset(out, 0, outsz);
    memset(items, 0, sizeof(items));
    items[0].buflen    = (uint16_t)(outsz - 1);
    items[0].item_code = (uint16_t)item_code;
    items[0].bufaddr   = out;
    items[0].retlen    = &len;

    return sys$getjpi(0, &vms_pid, NULL, items, NULL, NULL, 0);
}

/* read_full/write_full - pipe I/O that does not silently accept a short
 * transfer. A partially-read report would otherwise look like a helper
 * that failed, or worse, like one that succeeded with garbage. */
static int read_full(int fd, void *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

static int write_full(int fd, const void *buf, size_t n)
{
    size_t put = 0;
    while (put < n) {
        ssize_t w = write(fd, (const char *)buf + put, n - put);
        if (w < 0 && errno == EINTR) continue;
        if (w <= 0) return -1;
        put += (size_t)w;
    }
    return 0;
}

/* What the P11 helper reports back about itself, once. */
struct helper_report {
    uint32_t ok;
    uint32_t vms_pid;
    uint32_t uic;               /* the UIC the EXECUTIVE derived for it */
};

/* Consume BURN_CLOCKS of CPU. volatile so the loop is not optimised out. */
static void burn_cpu(void)
{
    clock_t start = clock();
    volatile unsigned long sink = 0;

    do {
        for (int i = 0; i < 20000; i++)
            sink += (unsigned long)i;
    } while (clock() - start < (clock_t)BURN_CLOCKS);
}

/*
 * alt_group_helper - the P11 subject. Never returns.
 *
 * setgid()s into ALT_UIC_GROUP BEFORE its first vms_kif_* call, because
 * the executive derives the UIC from the task's credentials AT
 * REGISTRATION (vms_proc_register) -- a process cannot declare it later,
 * which is the property that makes JPI$_UIC worth reading from the
 * executive at all. It stays uid 0 so it keeps the privilege to open
 * /dev/vms.
 *
 * Then it burns CPU on demand and acknowledges each burn, so the parent
 * synchronises on an OBSERVED event rather than on a sleep.
 */
static void alt_group_helper(int cmdfd, int repfd)
{
    struct helper_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    if (setgid(ALT_UIC_GROUP) != 0) {
        (void)write_full(repfd, &rep, sizeof(rep));   /* ok == 0 */
        _exit(1);
    }

    /* First kernel-interface call: binds and registers this process. */
    if (!(vms_kif_getjpi_self(&info) & 1)) {
        (void)write_full(repfd, &rep, sizeof(rep));   /* ok == 0 */
        _exit(1);
    }

    /* Stamp an authenticated user name of its OWN on its OWN row. The
     * parent must then read THIS name back for THIS process -- the whole
     * A-writes/B-reads point of JPI$_USERNAME. Same UIC and same
     * authorized mask it already holds, so nothing is being widened;
     * only the name changes. */
    if (!(vms_kif_setident(HELPER_USERNAME, info.uic, info.perm_privs) & 1)) {
        (void)write_full(repfd, &rep, sizeof(rep));   /* ok == 0 */
        _exit(1);
    }

    rep.ok      = 1;
    rep.vms_pid = info.vms_pid;
    rep.uic     = info.uic;
    if (write_full(repfd, &rep, sizeof(rep)) != 0)
        _exit(1);

    for (;;) {
        char ack = 'B';
        burn_cpu();
        if (write_full(repfd, &ack, 1) != 0)
            _exit(0);
        char cmd;
        if (read_full(cmdfd, &cmd, 1) != 0)
            _exit(0);           /* parent closed the command pipe: done */
    }
}

/*
 * xgrp_helper - the P12 subject. Never returns.
 *
 * A NAMED process in a UIC group of its own that has BURNED REAL CPU.
 * Both properties matter:
 *
 *   - the name is how the parent finds its row in SHOW SYSTEM's output,
 *     and enumeration is unprivileged on VMS (oracle-pinned, docs/oracle/
 *     vax73-privileges.md Section 4: a process holding NO privileges still
 *     saw EVERY process, with its name), so the row MUST appear even for a
 *     caller that may not read its identity;
 *   - the CPU is real, so the blank the redacted row prints is a value
 *     that EXISTS and is being WITHHELD -- not a value that happens to be
 *     zero. A test whose subject had burned nothing could not tell those
 *     apart.
 */
static void xgrp_helper(int cmdfd, int repfd)
{
    struct helper_report rep;
    struct vms_procinfo info;

    memset(&rep, 0, sizeof(rep));

    if (setgid(XGRP_UIC_GROUP) != 0) {
        (void)write_full(repfd, &rep, sizeof(rep));   /* ok == 0 */
        _exit(1);
    }
    if (!(vms_kif_getjpi_self(&info) & 1)) {          /* registers */
        (void)write_full(repfd, &rep, sizeof(rep));
        _exit(1);
    }
    if (!(vms_kif_setprn(XGRP_NAME) & 1)) {
        (void)write_full(repfd, &rep, sizeof(rep));
        _exit(1);
    }

    burn_cpu();

    rep.ok      = 1;
    rep.vms_pid = info.vms_pid;
    rep.uic     = info.uic;
    if (write_full(repfd, &rep, sizeof(rep)) != 0)
        _exit(1);

    /* Hold the row alive until the parent closes the command pipe. */
    for (;;) {
        char cmd;
        if (read_full(cmdfd, &cmd, 1) != 0)
            _exit(0);
    }
}

/*
 * spawn_named - $CREPRC a long-lived subject process under a given name.
 * Returns the $CREPRC status; *out_pid is the child pid on success.
 */
static uint32_t spawn_named(const char *prcnam, uint32_t *out_pid)
{
    struct dsc$descriptor_s img = str_dsc(SUBJECT_IMAGE);
    struct dsc$descriptor_s in  = str_dsc(HOLD_SCRIPT);
    struct dsc$descriptor_s nd  = str_dsc(prcnam);

    *out_pid = 0;
    return sys$creprc(out_pid, &img, &in, NULL, NULL, NULL, NULL, &nd,
                      0, 0, 0, 0);
}

/*
 * linux_pid_of - the Linux pid backing a VMS process, from its row.
 *
 * The two namespaces are separate since vms-2b8: the executive assigns
 * VMS process IDs, and only it knows which Linux task each one is. The
 * test needs the Linux pid for kill()/waitpid()/proc and for nothing
 * else. Returns 0 if the row does not resolve.
 */
static uint32_t linux_pid_of(uint32_t vms_pid)
{
    struct vms_procinfo info;

    if (!(vms_kif_getjpi_pid(vms_pid, &info) & 1))
        return 0;
    return info.linux_pid;
}

/*
 * comm_of - the Linux command name currently running as pid, from /proc.
 *
 * Used to prove the subject has actually EXEC'd -- i.e. that the name the
 * executive still answers with belongs to an image that was activated
 * after the name was set, and that knows nothing about it.
 */
static int comm_of(uint32_t pid, char *out, size_t outsz)
{
    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/comm", (unsigned)pid);
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    if (!fgets(out, (int)outsz, f)) { fclose(f); return -1; }
    fclose(f);
    size_t n = strlen(out);
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r')) out[--n] = '\0';
    return 0;
}

/* Wait (bounded) for the subject to reach its post-exec image. Polls an
 * OBSERVED condition -- /proc/<pid>/comm -- rather than sleeping a fixed
 * interval, so the suite is not paced by a guess about an emulated guest's
 * speed (a fixed-sleep test is a flaky test). */
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

static void reap(uint32_t pid)
{
    if (pid == 0) return;
    kill((pid_t)pid, SIGKILL);
    int st;
    while (waitpid((pid_t)pid, &st, 0) < 0 && errno == EINTR)
        ;
}

/* ================================================================
 * P13 machinery: $CREPRC under a caller that catches signals.
 * ================================================================ */

static volatile sig_atomic_t sigprobe_hits = 0;

static void sigprobe_handler(int sig)
{
    (void)sig;
    sigprobe_hits++;
}

/* The merged P13 report: the probe fills everything except `arranged`,
 * which only the tracer can know, and the tracer merges the two before
 * forwarding one struct to the suite. */
struct sigprobe_report {
    uint32_t ok;              /* probe loop completed AND tracer worked */
    uint32_t iters;           /* $CREPRC calls made */
    uint32_t arranged;        /* calls interrupted while the caller waited */
    uint32_t prclost;         /* calls that returned OVMX$_PRCLOST */
    uint32_t failed;          /* calls that returned any other failure */
    uint32_t unresolvable;    /* successes whose pid no row answers for */
};

/*
 * proc_is_sleeping - is this process blocked in the kernel right now?
 *
 * Field 3 of /proc/<pid>/stat, read AFTER the last ')' because the comm
 * field is parenthesised and may contain spaces. 'S' is interruptible
 * sleep; a ptrace-stopped task reads 't' and a running one 'R', so this
 * cannot confuse "stopped by me" with "waiting for the child".
 */
static int proc_is_sleeping(pid_t pid)
{
    char path[64], buf[512], *p;
    FILE *f;
    size_t n;

    snprintf(path, sizeof(path), "/proc/%d/stat", (int)pid);
    f = fopen(path, "r");
    if (!f) return 0;
    n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';
    p = strrchr(buf, ')');
    if (!p || !p[1]) return 0;
    return p[2] == 'S';
}

/*
 * sigprobe_child - create processes while catching signals. Never returns.
 *
 * Runs under the tracer (PTRACE_TRACEME + raise(SIGSTOP) is the standard
 * handshake that lets the tracer set its options before anything else
 * happens). It installs EXACTLY DCL's signal disposition and then just
 * creates processes: the arrangement -- holding the forked child still and
 * delivering the signal -- is entirely the tracer's job, so nothing here
 * depends on timing.
 *
 * Each iteration takes a name of its own and, on success, requires the
 * executive to resolve BY NAME back to the very process ID $CREPRC
 * returned. That is the statement OVMX$_PRCLOST denies: "no VMS process
 * was created ... nothing was ever entered in the table". A run that
 * reports the process lost while the table answers for it by name is a
 * status that does not describe the world.
 *
 * The subject is reaped inside the loop, so at most one is alive at a time
 * and the names are never in contention.
 */
static void sigprobe_child(int repfd)
{
    struct sigprobe_report rep;
    struct sigaction sa;

    memset(&rep, 0, sizeof(rep));

    if (ptrace(PTRACE_TRACEME, 0, 0, 0) != 0) {
        (void)write_full(repfd, &rep, sizeof(rep));     /* ok == 0 */
        _exit(1);
    }
    raise(SIGSTOP);                 /* the tracer sets its options here */

    /* EXACTLY DCL's disposition: sa_flags = 0, i.e. deliberately WITHOUT
     * SA_RESTART. Do not "fix" this to SA_RESTART -- the whole point is
     * that a caller is allowed to choose this, and $CREPRC's answer about
     * the child must not depend on the caller's choice. */
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigprobe_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    if (sigaction(SIGALRM, &sa, NULL) != 0) {
        (void)write_full(repfd, &rep, sizeof(rep));     /* ok == 0 */
        _exit(1);
    }

    while (rep.iters < SIGPROBE_ITERS) {
        char nm[16];
        uint32_t pid = 0, st;

        snprintf(nm, sizeof(nm), SIGPROBE_NAME_PFX "%02u",
                 (unsigned)(rep.iters % 100));

        st = spawn_named(nm, &pid);
        rep.iters++;

        if (st == OVMX$_PRCLOST) {
            rep.prclost++;
        } else if (!(st & 1)) {
            rep.failed++;
        } else {
            uint32_t byname = 0;
            uint32_t lpid = linux_pid_of(pid);
            if (pid == 0 || lpid == 0 ||
                !(getjpi_pid_of(nm, &byname) & 1) || byname != pid)
                rep.unresolvable++;
            reap(lpid);
        }
    }

    rep.ok = 1;
    (void)write_full(repfd, &rep, sizeof(rep));
    _exit(0);
}

/*
 * sigprobe_tracer - hold each forked child still and interrupt the caller.
 * Never returns.
 *
 * THIS IS THE ARRANGEMENT, and it is deterministic by construction:
 *
 *   1. PTRACE_O_TRACEFORK means every process the probe forks is stopped
 *      before it executes an instruction. A stopped child cannot write the
 *      creation report, so the caller's handshake read blocks and STAYS
 *      blocked -- there is no window to race.
 *   2. The tracer waits for the caller to be genuinely asleep (state 'S'
 *      in /proc), which is an OBSERVED condition, not a delay.
 *   3. Only then does it send SIGALRM. Because the probe is traced, the
 *      signal arrives as a signal-delivery-stop, and the tracer injects it
 *      with PTRACE_CONT(..., SIGALRM) -- so the handler runs and the read
 *      returns EINTR, exactly as it would for a DCL user pressing Ctrl-C.
 *   4. Only THEN is the child released (PTRACE_DETACH), so it registers,
 *      reports and execs its image as usual.
 *
 * Correct code retries the read and returns the child's real status. The
 * pre-fix code takes the EINTR as "the child died", reports OVMX$_PRCLOST
 * and reaps a live process -- which blocks for the lifetime of the image
 * it just started, and is what the suite's bounded wait catches.
 */
static void sigprobe_tracer(int repfd)
{
    struct sigprobe_report rep, prep;
    int a[2] = { -1, -1 };
    pid_t probe, held = -1;
    int status, probe_alive = 1;

    memset(&rep, 0, sizeof(rep));
    setpgid(0, 0);                  /* one group the suite can kill */

    if (pipe(a) != 0) {
        (void)write_full(repfd, &rep, sizeof(rep));     /* ok == 0 */
        _exit(1);
    }

    probe = fork();
    if (probe < 0) {
        (void)write_full(repfd, &rep, sizeof(rep));     /* ok == 0 */
        _exit(1);
    }
    if (probe == 0) {
        close(a[0]);
        close(repfd);
        sigprobe_child(a[1]);
        _exit(0);                                       /* not reached */
    }
    close(a[1]);

    /* The probe's own raise(SIGSTOP) -- where its options get set. */
    while (waitpid(probe, &status, 0) < 0 && errno == EINTR)
        ;
    if (!WIFSTOPPED(status) ||
        ptrace(PTRACE_SETOPTIONS, probe, 0, PTRACE_O_TRACEFORK) != 0 ||
        ptrace(PTRACE_CONT, probe, 0, 0) != 0) {
        kill(probe, SIGKILL);
        (void)write_full(repfd, &rep, sizeof(rep));     /* ok == 0 */
        _exit(1);
    }

    while (probe_alive) {
        pid_t pid = waitpid(-1, &status, 0);
        int sig, event;

        if (pid < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (WIFEXITED(status) || WIFSIGNALED(status)) {
            if (pid == probe) probe_alive = 0;
            continue;
        }
        if (!WIFSTOPPED(status))
            continue;

        sig   = WSTOPSIG(status);
        event = (status >> 16) & 0xff;

        if (pid != probe) {
            /* The held child's birth stop. Leave it stopped: releasing it
             * here is exactly the race this design exists to remove. */
            continue;
        }

        if (event == PTRACE_EVENT_FORK) {
            unsigned long msg = 0;
            int i;

            if (ptrace(PTRACE_GETEVENTMSG, probe, 0, &msg) == 0)
                held = (pid_t)msg;
            ptrace(PTRACE_CONT, probe, 0, 0);

            for (i = 0; i < SIGPROBE_SLEEP_POLLS; i++) {
                if (proc_is_sleeping(probe)) break;
                usleep(SIGPROBE_SLEEP_POLL_US);
            }
            if (i < SIGPROBE_SLEEP_POLLS) {
                kill(probe, SIGALRM);   /* interrupts the handshake read */
                rep.arranged++;
            } else if (held > 0) {
                /* Never observed the caller blocked: release the child
                 * rather than wedge, and do not count this call as
                 * arranged. */
                ptrace(PTRACE_DETACH, held, 0, 0);
                held = -1;
            }
            continue;
        }

        if (event != 0) {
            ptrace(PTRACE_CONT, probe, 0, 0);
            continue;
        }

        if (sig == SIGALRM) {
            ptrace(PTRACE_CONT, probe, 0, SIGALRM);     /* inject it */
            if (held > 0) {
                ptrace(PTRACE_DETACH, held, 0, 0);      /* now let it run */
                held = -1;
            }
            continue;
        }

        ptrace(PTRACE_CONT, probe, 0, sig);
    }

    memset(&prep, 0, sizeof(prep));
    if (read_full(a[0], &prep, sizeof(prep)) == 0) {
        rep.ok           = prep.ok;
        rep.iters        = prep.iters;
        rep.prclost      = prep.prclost;
        rep.failed       = prep.failed;
        rep.unresolvable = prep.unresolvable;
    }
    close(a[0]);
    (void)write_full(repfd, &rep, sizeof(rep));
    _exit(0);
}

/* now_ms - CLOCK_MONOTONIC in milliseconds, for the probe's time bound. */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/*
 * read_full_bounded - read_full with a deadline.
 *
 * The bound is the assertion, not a convenience: the defect this block
 * detects made $CREPRC block in waitpid() on a LIVE process, so a probe
 * that hung would otherwise hang the whole QEMU rig instead of reporting
 * a failure. Returns 0 on a complete read, -1 on error or EOF, -2 on
 * timeout.
 */
static int read_full_bounded(int fd, void *buf, size_t n, int timeout_ms)
{
    long long deadline = now_ms() + timeout_ms;
    size_t got = 0;

    while (got < n) {
        struct pollfd pfd;
        long long left = deadline - now_ms();
        int pr;

        if (left <= 0) return -2;
        pfd.fd = fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        pr = poll(&pfd, 1, (int)left);
        if (pr < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (pr == 0) return -2;

        ssize_t r = read(fd, (char *)buf + got, n - got);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        got += (size_t)r;
    }
    return 0;
}

/*
 * run_show_system - run the REAL "SHOW SYSTEM" DCL command and capture it.
 *
 * DCL.EXE is staged into the initramfs at /bin by tests/qemu/Dockerfile
 * (absence there is a FATAL image-build error, not a skip). This is the
 * only place SHOW SYSTEM can be proven: it is a reader of the executive's
 * process table, and ctest never runs anywhere a process table exists.
 *
 * Returns 0 on success with the command's stdout in out.
 */
static int run_show_system(char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);
        execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    const char *script = "SHOW SYSTEM\nLOGOUT\n";
    ssize_t w = write(in_pipe[1], script, strlen(script));
    (void)w;
    close(in_pipe[1]);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/*
 * run_show_system_unpriv - SHOW SYSTEM run by a process that may not read
 * every row in the table.
 *
 * Same as run_show_system, except the child arranges, BEFORE exec'ing
 * DCL.EXE, to be a process the executive will refuse identity reads for:
 *
 *   setgid(SHOW_UIC_GROUP)  -- the executive derives the UIC from the
 *       task's credentials at registration, so this puts it in its own
 *       UIC group, which is neither the parent's (0) nor XGRP's.
 *   vms_kif_setprn(SHOW_NAME) -- so the parent can find THIS process's
 *       own row in the output and compare it against the redacted one.
 *   vms_kif_setident(..., privs & ~(WORLD|SETPRV)) -- drops WORLD, which
 *       is the ONLY privilege that authorises a cross-UIC-group identity
 *       read (oracle-pinned, docs/oracle/vax73-privileges.md Section 5),
 *       and drops SETPRV with it so the drop is one-way.
 *
 * All three survive execve(): the executive's entry is keyed by the pid,
 * which exec does not change. That is the same property the whole item
 * rests on, used here to hand DCL.EXE an identity it could not have
 * given itself.
 *
 * On any setup failure the child prints SETUP_FAIL_MARK on the captured
 * stream instead of exec'ing, so a broken arrangement fails LOUDLY
 * rather than quietly running SHOW SYSTEM with full privilege and
 * passing for the wrong reason.
 */
static int run_show_system_unpriv(char *out, size_t outsz)
{
    int in_pipe[2], out_pipe[2];

    out[0] = '\0';
    if (pipe(in_pipe) < 0) return -1;
    if (pipe(out_pipe) < 0) { close(in_pipe[0]); close(in_pipe[1]); return -1; }

    pid_t pid = fork();
    if (pid < 0) return -1;

    if (pid == 0) {
        struct vms_procinfo info;
        uint32_t st;

        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        close(out_pipe[0]); close(out_pipe[1]);

        if (setgid(SHOW_UIC_GROUP) != 0) {
            printf(SETUP_FAIL_MARK " setgid\n");
            fflush(stdout);
            _exit(126);
        }
        st = vms_kif_getjpi_self(&info);      /* first call: registers */
        if (!(st & 1)) {
            printf(SETUP_FAIL_MARK " register %08X\n", st);
            fflush(stdout);
            _exit(126);
        }
        st = vms_kif_setprn(SHOW_NAME);
        if (!(st & 1)) {
            printf(SETUP_FAIL_MARK " setprn %08X\n", st);
            fflush(stdout);
            _exit(126);
        }
        st = vms_kif_setident(SHOWER_USERNAME, info.uic,
                              info.perm_privs &
                              ~(uint64_t)(VMS_PRV_M_WORLD | VMS_PRV_M_SETPRV));
        if (!(st & 1)) {
            printf(SETUP_FAIL_MARK " setident %08X\n", st);
            fflush(stdout);
            _exit(126);
        }
        if (!(vms_kif_getjpi_self(&info) & 1) ||
            (info.cur_privs & VMS_PRV_M_WORLD) != 0) {
            printf(SETUP_FAIL_MARK " world-still-held\n");
            fflush(stdout);
            _exit(126);
        }

        execl("/bin/DCL.EXE", "DCL.EXE", (char *)NULL);
        printf(SETUP_FAIL_MARK " exec\n");
        fflush(stdout);
        _exit(127);
    }

    close(in_pipe[0]);
    close(out_pipe[1]);
    const char *script = "SHOW SYSTEM\nLOGOUT\n";
    ssize_t w = write(in_pipe[1], script, strlen(script));
    (void)w;
    close(in_pipe[1]);

    size_t used = 0;
    for (;;) {
        ssize_t n = read(out_pipe[0], out + used, outsz - 1 - used);
        if (n <= 0) break;
        used += (size_t)n;
        if (used >= outsz - 1) break;
    }
    out[used] = '\0';
    close(out_pipe[0]);

    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    return 0;
}

/*
 * row_for - the SHOW SYSTEM row naming `name`, or NULL.
 *
 * A row is " %08X %-15s  %s": pid, process name, CPU. Matching is
 * anchored on the name column so a name appearing in the banner or in
 * some other column cannot be mistaken for a row.
 */
static const char *row_for(const char *text, const char *name)
{
    const char *line = text;
    size_t namelen = strlen(name);

    while (line && *line) {
        if (line[0] == ' ') {
            int i;
            for (i = 1; i < 9; i++)
                if (!isxdigit((unsigned char)line[i]))
                    break;
            if (i == 9 && line[9] == ' ' &&
                strncmp(line + 10, name, namelen) == 0)
                return line;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return NULL;
}

/*
 * row_has_value_after_name - does anything but padding follow the process
 * name on this row?
 *
 * This is the redaction detector. SHOW SYSTEM prints a CPU figure after
 * the name column for a row it can source one for, and NOTHING for a row
 * the executive redacted -- no zero, no marker. So "is there a non-space
 * character after the name" is exactly the question, and it does not
 * depend on the column widths this item deliberately did not pin.
 */
static int row_has_value_after_name(const char *row, const char *name)
{
    const char *p = row + 10 + strlen(name);

    for (; *p && *p != '\n'; p++)
        if (*p != ' ' && *p != '\r')
            return 1;
    return 0;
}

/*
 * count_process_rows - how many process rows SHOW SYSTEM printed.
 *
 * A row is a line beginning with a space and eight hex digits (the Pid
 * column, " %08X "). The banner and the column headings do not match.
 */
static int count_process_rows(const char *text)
{
    int rows = 0;
    const char *line = text;

    while (line && *line) {
        if (line[0] == ' ') {
            int i;
            for (i = 1; i < 9; i++)
                if (!isxdigit((unsigned char)line[i]))
                    break;
            if (i == 9 && line[9] == ' ')
                rows++;
        }
        line = strchr(line, '\n');
        if (line) line++;
    }
    return rows;
}

/*
 * device_absent_checks - the negative-control path.
 *
 * With no /dev/vms present, NOTHING here may report success. These are the
 * assertions that make the CI negative-control job meaningful: a public
 * sys$ entry point that fabricates an answer when it cannot reach the
 * executive would turn these green, and the job asserts on this suite's
 * exit code being the honest-skip 77 rather than 0.
 */
static int device_absent_checks(void)
{
    printf("  (no /dev/vms -- running device-absent assertions)\n");

    uint32_t pid = 0;
    uint32_t st = getjpi_pid_of(SUBJECT_NAME, &pid);
    CHECK(!(st & 1),
          "sys$getjpi by name does not report success with no executive");

    char name[64];
    st = getjpi_prcnam_of(1, name, sizeof(name));
    CHECK(!(st & 1),
          "sys$getjpi by pid does not report success with no executive");

    struct vms_procinfo info;
    uint32_t index = 0;
    st = vms_kif_procscan(&index, &info);
    CHECK(!(st & 1),
          "the process-table scan does not report success with no executive");

    printf("=== test_syssvc_procnam: %d passed, %d failed (SKIPPED: no /dev/vms) ===\n",
           pass, fail);
    return fail > 0 ? 1 : EXIT_SKIP;
}

int main(void)
{
    printf("=== test_syssvc_procnam: executive process table via public sys$ API ===\n");

    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0)
        return device_absent_checks();
    close(devfd);

    /* The subject's script. It must block, or the subject exits before it
     * can be observed; `sleep` is a busybox applet installed by init.sh. */
    FILE *hs = fopen(HOLD_SCRIPT, "w");
    if (!hs) {
        printf("  FAIL: cannot write %s\n", HOLD_SCRIPT);
        printf("=== test_syssvc_procnam: 0 passed, 1 failed ===\n");
        return 1;
    }
    fprintf(hs, "sleep 600\n");
    fclose(hs);
    chmod(HOLD_SCRIPT, 0644);

    uint32_t subject = 0, dup_pid = 0, long_pid = 0, ok15_pid = 0;
    uint32_t subject_lpid = 0, ok15_lpid = 0;

    /*
     * THE CALLER'S OWN VMS PROCESS ID COMES FROM THE EXECUTIVE, never
     * from getpid(). vms-2b8 made the executive ASSIGN process IDs from
     * its own generator (src/kernel/vms_module.c assign_vms_pid) instead
     * of adopting the Linux pid, so getpid() is no longer a VMS process
     * ID and a test that used it would be asserting against a number
     * that names no row. This whole suite therefore deals in two clearly
     * separate namespaces: VMS process IDs, which only the executive
     * issues and only $GETJPI resolves, and Linux pids, which are used
     * for nothing but kill()/waitpid()/proc and are obtained from a row
     * through linux_pid_of().
     */
    struct vms_procinfo selfinfo;
    uint32_t st = vms_kif_getjpi_self(&selfinfo);
    CHECK(st & 1, "the caller has a row in the executive's process table");
    uint32_t selfpid = selfinfo.vms_pid;
    CHECK(selfpid != 0, "the executive assigned the caller a VMS process ID");

    /* ---------------------------------------------------------------
     * P1. $CREPRC enters the child in the EXECUTIVE's table under the
     *     requested name, and the name survives image activation.
     * --------------------------------------------------------------- */
    st = spawn_named(SUBJECT_NAME, &subject);
    CHECK(st & 1, "sys$creprc created the subject process");
    /* The pairing matters as much as either half. $CREPRC used to have a
     * path that returned SS$_NORMAL with *pidadr left at zero -- success
     * naming no process, a combination VMS does not produce -- when the
     * forked child died before it could report. That path now returns
     * OVMX$_PRCLOST (src/libvms/include/ovmx_status.h) and reaps the
     * child, and the only success return left in sys$creprc is
     * structurally downstream of a process ID the executive assigned. */
    CHECK(subject != 0, "sys$creprc returned the subject's pid");

    if (!(st & 1) || subject == 0) {
        printf("=== test_syssvc_procnam: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    /* $CREPRC hands back the EXECUTIVE's process ID (vms-2b8), so it must
     * be one the executive can resolve -- and it must not be the caller's
     * own.
     *
     * Mutation M-D ($CREPRC's *pidadr set to the fork() return, which is
     * what it was before this round) was built and run in this harness:
     * 49/0 -> 40/9. Nine assertions flip, because a Linux pid handed out
     * as a VMS process ID poisons every later lookup that uses it. That
     * is the measure of how load-bearing the distinction is. */
    CHECK(subject != selfpid, "the subject's VMS process ID is not the caller's");
    subject_lpid = linux_pid_of(subject);
    CHECK(subject_lpid != 0,
          "the VMS process ID $CREPRC returned resolves to a live row");

    CHECK(wait_for_exec(subject_lpid, "sh") == 0,
          "the subject reached its post-exec image (/bin/sh)");

    /* ---------------------------------------------------------------
     * P2. A-WRITES / B-READS: this process resolves the SUBJECT BY NAME.
     *     The subject is /bin/sh; it has never opened /dev/vms and holds
     *     no copy of its own name. A per-process fake cannot pass this.
     * --------------------------------------------------------------- */
    uint32_t found = 0;
    st = getjpi_pid_of(SUBJECT_NAME, &found);
    CHECK(st & 1, "sys$getjpi resolved another process BY NAME");
    CHECK(found == subject,
          "sys$getjpi by name returned the SUBJECT's pid, not the caller's");

    /* ---------------------------------------------------------------
     * P3. The reverse direction: resolve by PID, get the executive's
     *     name for THAT process -- not the caller's own.
     * --------------------------------------------------------------- */
    char name[64];
    st = getjpi_prcnam_of(subject, name, sizeof(name));
    CHECK(st & 1, "sys$getjpi resolved another process by pid");
    CHECK(strcmp(name, SUBJECT_NAME) == 0,
          "sys$getjpi by pid returned the SUBJECT's name");

    /* And the same call for ourselves must NOT return the subject's name --
     * this is what fails if the reader collapses every query onto one row. */
    char selfname[64];
    st = getjpi_prcnam_of(selfpid, selfname, sizeof(selfname));
    CHECK(st & 1, "sys$getjpi resolved the caller by its own pid");
    CHECK(strcmp(selfname, SUBJECT_NAME) != 0,
          "the caller's row is distinct from the subject's");

    /* ---------------------------------------------------------------
     * P4. NEGATIVE: a name no process holds does not resolve. Paired
     *     with P2 -- a stub that always reports success passes P2 and
     *     fails here.
     * --------------------------------------------------------------- */
    uint32_t none = 0;
    st = getjpi_pid_of(ABSENT_NAME, &none);
    CHECK(st == SS$_NONEXPR,
          "sys$getjpi by an unheld name returns SS$_NONEXPR");

    /* ---------------------------------------------------------------
     * P5. Uniqueness is enforced BY THE EXECUTIVE and reported to the
     *     CREATOR. Oracle (VAX1, VMS VAX V7.3, recorded on vms-8019): a
     *     second process taking a PROCESS_NAME already held in the same
     *     UIC group is refused %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM.
     * --------------------------------------------------------------- */
    st = spawn_named(SUBJECT_NAME, &dup_pid);
    CHECK(st == SS$_DUPLNAM,
          "sys$creprc refuses a duplicate process name with SS$_DUPLNAM");

    /* If the executive ever DOES accept the duplicate -- which happens
     * only with the clash test defeated, i.e. under the
     * proctab-duplicate-name negative control -- reap it at once. Two live
     * rows holding one name make the next lookup's answer depend on hash
     * order, so the assertion below would go red on some runs and green on
     * others: MEASURED, one run of that control reddened it and the next
     * did not. A negative control whose red set varies run to run is a
     * flaky gate. This also stops a `sleep 600` leaking for the rest of
     * the suite. */
    if ((st & 1) && dup_pid != 0)
        reap(linux_pid_of(dup_pid));

    /* The name must still belong to the ORIGINAL subject afterwards --
     * a refused creation must not have stolen or cleared it. */
    found = 0;
    st = getjpi_pid_of(SUBJECT_NAME, &found);
    CHECK((st & 1) && found == subject,
          "the refused duplicate left the original subject's name intact");

    /* ---------------------------------------------------------------
     * P6. An OVERSIZED name is rejected, not truncated. A 15-character
     *     name is legal and must be accepted; 16 must be refused with
     *     SS$_IVLOGNAM, and must NOT have created a process under the
     *     clipped 15-character prefix.
     * --------------------------------------------------------------- */
    st = spawn_named(LEN16_NAME, &ok15_pid);
    CHECK(st & 1, "sys$creprc accepts a 15-character process name");
    ok15_lpid = linux_pid_of(ok15_pid);

    st = spawn_named(LEN17_NAME, &long_pid);
    CHECK(st == SS$_IVLOGNAM,
          "sys$creprc rejects a 16-character process name with SS$_IVLOGNAM");

    /* LEN17_NAME is LEN16_NAME plus one character, so a client that
     * truncates would have created a SECOND process holding LEN16_NAME --
     * or been refused SS$_DUPLNAM by the executive for it. Either way the
     * pid that answers for LEN16_NAME must still be the one legitimately
     * created above. */
    uint32_t clipped = 0;
    st = getjpi_pid_of(LEN16_NAME, &clipped);
    CHECK((st & 1) && clipped == ok15_pid,
          "the rejected 16-character name created nothing under its 15-character prefix");

    /* ---------------------------------------------------------------
     * P7. ENUMERATION -- the reader behind SHOW SYSTEM. The table must
     *     list MORE THAN THE CALLING PROCESS, and must contain the
     *     subject BY NAME. src/vmsdcl/dcl_cmd_show.c walks exactly this
     *     scan; before this item it printed one row and fabricated it.
     * --------------------------------------------------------------- */
    struct vms_procinfo info;
    uint32_t index = 0;
    int rows = 0, saw_self = 0, saw_subject = 0, saw_other_named = 0;

    while (vms_kif_procscan(&index, &info) & 1) {
        rows++;
        if (info.vms_pid == selfpid) saw_self = 1;
        if (info.vms_pid == subject && strcmp(info.prcnam, SUBJECT_NAME) == 0)
            saw_subject = 1;
        if (info.vms_pid != selfpid && info.prcnam[0] != '\0')
            saw_other_named++;
        if (rows > 256) break;              /* runaway cursor guard */
    }

    CHECK(rows > 1, "the process-table scan lists MORE THAN the calling process");
    CHECK(saw_self, "the scan includes the calling process");
    CHECK(saw_subject, "the scan includes the subject, named as the executive knows it");
    CHECK(saw_other_named >= 2,
          "the scan names processes the caller is not (subject + 15-char subject)");

    /* ---------------------------------------------------------------
     * P8. THE USER-VISIBLE COMMAND. Run the real "SHOW SYSTEM" through
     *     the real DCL.EXE against this real executive. It must list
     *     MORE THAN THE CALLING PROCESS and must name the subject --
     *     which is the outcome this item is actually about, and which
     *     the old one-row-from-my-own-PCB implementation could not
     *     produce no matter how many processes existed.
     * --------------------------------------------------------------- */
    static char sysout[65536];
    if (run_show_system(sysout, sizeof(sysout)) != 0) {
        CHECK(0, "SHOW SYSTEM ran under DCL.EXE");
    } else {
        CHECK(strstr(sysout, "Process Name") != NULL,
              "SHOW SYSTEM printed its process table heading");
        int rows = count_process_rows(sysout);
        if (rows <= 1)
            printf("  (SHOW SYSTEM printed %d row(s); output follows)\n%s\n",
                   rows, sysout);
        CHECK(rows > 1, "SHOW SYSTEM listed MORE THAN the calling process");
        CHECK(strstr(sysout, SUBJECT_NAME) != NULL,
              "SHOW SYSTEM named the subject process, which it did not create");
        CHECK(strstr(sysout, LEN16_NAME) != NULL,
              "SHOW SYSTEM named the second subject process too");
    }

    /* ---------------------------------------------------------------
     * P9. lib$getjpi -- the RTL wrapper -- reaches the same executive
     *     row. This coverage was moved here from tests/libvms/
     *     test_lib_rtl.c, which asserted it on a host with no /dev/vms:
     *     an assertion that a VMS system service works with no executive
     *     present is an assertion about a system OVMX never runs as
     *     (Rule 9). Here it runs against a real one.
     * --------------------------------------------------------------- */
    uint32_t item = JPI$_PID;
    uint32_t lpid = 0;
    st = lib$getjpi(&item, NULL, NULL, &lpid, NULL, NULL);
    CHECK(st == SS$_NORMAL, "lib$getjpi(JPI$_PID) returns SS$_NORMAL");
    CHECK(lpid == selfpid, "lib$getjpi(JPI$_PID) returns the caller's own pid");

    /*
     * JPI$_USERNAME reads the AUTHENTICATED name in the executive's row
     * (vms-2b8's vms_kif_setident), so the row must have one before the
     * item means anything. The old assertion here was "returns a
     * non-empty string", which the deleted getpwuid/"UNKNOWN" derivation
     * satisfied without any identity existing anywhere -- it could not
     * fail. Stamp a name, then require that exact name back.
     */
    st = vms_kif_setident(CALLER_USERNAME, selfinfo.uic, selfinfo.perm_privs);
    CHECK(st & 1, "the caller stamped an authenticated identity on its row");

    char ubuf[32];
    memset(ubuf, 0, sizeof(ubuf));
    struct dsc$descriptor_s udesc = str_dsc(ubuf);
    udesc.dsc$w_length = sizeof(ubuf) - 1;
    uint16_t ulen = 0;
    item = JPI$_USERNAME;
    st = lib$getjpi(&item, NULL, NULL, NULL, &udesc, &ulen);
    CHECK(st == SS$_NORMAL, "lib$getjpi(JPI$_USERNAME) returns SS$_NORMAL");
    CHECK(ulen == strlen(CALLER_USERNAME) &&
          strncmp(ubuf, CALLER_USERNAME, ulen) == 0,
          "lib$getjpi(JPI$_USERNAME) returns the name the EXECUTIVE holds");

    char pnbuf[32];
    memset(pnbuf, 0, sizeof(pnbuf));
    struct dsc$descriptor_s pndesc = str_dsc(pnbuf);
    pndesc.dsc$w_length = sizeof(pnbuf) - 1;
    uint16_t pnlen = 0;
    item = JPI$_PRCNAM;
    st = lib$getjpi(&item, NULL, NULL, NULL, &pndesc, &pnlen);
    CHECK(st == SS$_NORMAL, "lib$getjpi(JPI$_PRCNAM) returns SS$_NORMAL");

    reap(subject_lpid);
    reap(ok15_lpid);

    /* ---------------------------------------------------------------
     * P10. A name is released when its process dies, so it can be taken
     *     again. Proves the table is live state, not an append-only log.
     * --------------------------------------------------------------- */
    uint32_t retaken = 0;
    st = spawn_named(SUBJECT_NAME, &retaken);
    CHECK(st & 1, "the subject's name is available again once it has exited");
    reap(linux_pid_of(retaken));
    unlink(HOLD_SCRIPT);

    /* ---------------------------------------------------------------
     * P11. THE OTHER THREE ITEM CODES $GETJPI NOW READS OUT OF THE
     *      EXECUTIVE'S ROW: JPI$_UIC, JPI$_CPUTIM and JPI$_USERNAME.
     *
     * P1-P10 above only exercise JPI$_PID and JPI$_PRCNAM. The three
     * codes below were rewritten by this item from "answer out of the
     * caller's own PCB / getuid() / getrusage(SELF)" to "answer from the
     * row the executive resolved", and a rewrite nothing asserts on is
     * the untested-product-code this epic exists to stop shipping.
     *
     * Every check here is A-writes / B-reads and, more importantly, is
     * DISCRIMINATING -- it fails if the code collapses back onto the
     * caller. That needs a subject whose values genuinely differ from
     * the caller's, which is why the subject is a helper in a DIFFERENT
     * UIC GROUP burning its OWN CPU: on a rig where every process is
     * root and idle, "the executive's UIC" and "my UIC" are the same
     * number and the assertion would read identically either way.
     * --------------------------------------------------------------- */
    int cmdfd[2] = { -1, -1 }, repfd[2] = { -1, -1 };
    pid_t helper = -1;

    if (pipe(cmdfd) != 0 || pipe(repfd) != 0) {
        CHECK(0, "P11: pipes for the cross-UIC-group helper");
    } else if ((helper = fork()) < 0) {
        CHECK(0, "P11: fork of the cross-UIC-group helper");
    } else if (helper == 0) {
        close(cmdfd[1]);
        close(repfd[0]);
        alt_group_helper(cmdfd[0], repfd[1]);
        _exit(0);                               /* not reached */
    } else {
        close(cmdfd[0]);
        close(repfd[1]);

        struct helper_report rep;
        memset(&rep, 0, sizeof(rep));
        int got = read_full(repfd[0], &rep, sizeof(rep));

        CHECK(got == 0 && rep.ok == 1 && rep.vms_pid != 0,
              "a helper in UIC group 300 registered with the executive");
        CHECK(got == 0 && (rep.uic >> 16) == (uint32_t)ALT_UIC_GROUP,
              "the executive derived the helper's UIC group from its credentials");

        if (got == 0 && rep.ok == 1) {
            char ack;
            CHECK(read_full(repfd[0], &ack, 1) == 0,
                  "the helper reported its first CPU burn");

            /* --- JPI$_UIC ------------------------------------------
             * Must be the row the executive holds for the HELPER. If
             * this reverts to pcb->uic or to (getgid()<<16)|getuid(),
             * it becomes the caller's 0x00000000 and both checks fail.
             *
             * Mutation M-A (JPI$_UIC restored to the pre-item
             * "(getgid()<<16)|getuid()") was built and run in this
             * harness: 49/0 -> 47/2, and the two that flipped are
             * exactly the two below. Both are detectors.
             */
            uint32_t helper_uic = 0, self_uic = 0;
            st = getjpi_u32_of(rep.vms_pid, JPI$_UIC, &helper_uic);
            CHECK(st & 1, "sys$getjpi read JPI$_UIC for another process");
            CHECK(helper_uic == rep.uic,
                  "JPI$_UIC is the UIC the EXECUTIVE derived for the helper");
            st = getjpi_u32_of(selfpid, JPI$_UIC, &self_uic);
            CHECK((st & 1) && helper_uic != self_uic,
                  "JPI$_UIC distinguishes the target from the caller");

            /* --- JPI$_CPUTIM ---------------------------------------
             * Measured as a DELTA against the caller's own, so nothing
             * depends on an absolute threshold or on how fast the
             * emulated guest is. Between the two readings the helper
             * burns another BURN_CLOCKS of CPU while the caller is
             * blocked in read(). If JPI$_CPUTIM answered from
             * getrusage(RUSAGE_SELF) -- what it did before this item --
             * both readings would be the CALLER's.
             *
             * MEASURED, not assumed. Mutation M-B (jpi_cputim's
             * "if (linux_pid == getpid())" forced to "if (1)", so every
             * answer is getrusage(RUSAGE_SELF)) was built and run in this
             * harness: 49/0 -> 47/2, flipping both "tracks the TARGET's
             * consumption over time" and "the CPU growth reported is the
             * HELPER's". `t1 > 0` did NOT flip -- the caller has burned
             * CPU of its own by this point -- so it is a companion, not a
             * detector. The two that did flip are the detectors; do not
             * delete either as redundant.
             */
            uint32_t t1 = 0, t2 = 0, s1 = 0, s2 = 0;
            st = getjpi_u32_of(rep.vms_pid, JPI$_CPUTIM, &t1);
            CHECK(st & 1, "sys$getjpi read JPI$_CPUTIM for another process");
            CHECK(t1 > 0, "JPI$_CPUTIM reports CPU the helper actually burned");
            (void)getjpi_u32_of(selfpid, JPI$_CPUTIM, &s1);

            char go = 'G';
            CHECK(write_full(cmdfd[1], &go, 1) == 0,
                  "the helper was told to burn a second CPU quantum");
            CHECK(read_full(repfd[0], &ack, 1) == 0,
                  "the helper reported its second CPU burn");

            (void)getjpi_u32_of(rep.vms_pid, JPI$_CPUTIM, &t2);
            (void)getjpi_u32_of(selfpid, JPI$_CPUTIM, &s2);
            CHECK(t2 > t1,
                  "JPI$_CPUTIM tracks the TARGET's consumption over time");
            CHECK((t2 - t1) > (s2 - s1),
                  "the CPU growth reported is the HELPER's, not the caller's");

            /* --- JPI$_USERNAME -------------------------------------
             * A-WRITES / B-READS on the authenticated identity. The
             * helper stamped HELPER_USERNAME on its OWN row through
             * vms_kif_setident (vms-2b8); the caller stamped
             * CALLER_USERNAME on its own back in P9. Neither process can
             * see the other's string except through the executive: they
             * are separate address spaces, and the helper never tells
             * the parent its name over the pipe.
             *
             * So the pair below is the whole property. If sys$getjpi
             * answers about the caller (the pre-item behaviour), or from
             * any process-local source, the helper query returns
             * CALLER_USERNAME and fails.
             *
             * Mutation M-C (JPI$_USERNAME answered from a
             * vms_kif_getjpi_self() row instead of the resolved one --
             * "answer about me, whoever you asked about") was built and
             * run in this harness: 49/0 -> 48/1, flipping exactly the
             * helper check below.
             */
            char huser[64], suser[64];
            st = getjpi_str_of(rep.vms_pid, JPI$_USERNAME, huser, sizeof(huser));
            CHECK(st & 1, "sys$getjpi read JPI$_USERNAME for another process");
            CHECK(strcmp(huser, HELPER_USERNAME) == 0,
                  "JPI$_USERNAME returns the name the HELPER stamped on its own row");

            st = getjpi_str_of(selfpid, JPI$_USERNAME, suser, sizeof(suser));
            CHECK((st & 1) && strcmp(suser, CALLER_USERNAME) == 0,
                  "JPI$_USERNAME for the caller returns the caller's own stamped name");
        }

        close(cmdfd[1]);            /* releases the helper from its loop */
        close(repfd[0]);
        if (helper > 0) {
            kill(helper, SIGKILL);
            int hst;
            while (waitpid(helper, &hst, 0) < 0 && errno == EINTR)
                ;
        }
    }

    /* ---------------------------------------------------------------
     * P12. SHOW SYSTEM RUN BY A CALLER THAT MAY NOT READ EVERY ROW.
     *
     * WHY THIS BLOCK EXISTS. src/kernel/vms_proctab.c redacts any row
     * the caller may not $GETJPI -- a process in another UIC group when
     * the caller has no WORLD privilege -- zeroing linux_pid, uic, the
     * privilege masks and the user name while KEEPING the process ID and
     * the process name, because on the oracle enumeration is unprivileged
     * and identity is not. Nothing had ever executed that path: every
     * process in this rig is uid 0 / gid 0 with CAP_SYS_ADMIN, so
     * vms_proc_may_read() always returned true.
     *
     * That gap shipped a defect. src/vmsdcl/dcl_cmd_show.c fed the
     * redacted (zero) linux_pid to /proc, ignored the failure, and
     * printed the caller's own buffer initialiser -- so a process whose
     * accounting the caller is FORBIDDEN to read displayed a concrete
     * "0 00:00:00.00". A fabricated accounting value, inside the very
     * function this item converted.
     *
     * THE ARRANGEMENT, and why each piece is load-bearing:
     *   XGRP  -- named, in UIC group 302, has burned REAL CPU.
     *   SHOW  -- runs the real DCL.EXE, in UIC group 301, with WORLD
     *            DROPPED, so the executive refuses it XGRP's identity.
     * The two rows in one output are the discriminator: SHOW's OWN row
     * must carry a CPU figure (it may read itself), and XGRP's must
     * carry NOTHING. A test that looked only at the redacted row could
     * be satisfied by SHOW SYSTEM never printing CPU at all.
     * --------------------------------------------------------------- */
    int xcmd[2] = { -1, -1 }, xrep[2] = { -1, -1 };
    pid_t xproc = -1;

    if (pipe(xcmd) != 0 || pipe(xrep) != 0) {
        CHECK(0, "P12: pipes for the unreadable-row subject");
    } else if ((xproc = fork()) < 0) {
        CHECK(0, "P12: fork of the unreadable-row subject");
    } else if (xproc == 0) {
        close(xcmd[1]);
        close(xrep[0]);
        xgrp_helper(xcmd[0], xrep[1]);
        _exit(0);                               /* not reached */
    } else {
        close(xcmd[0]);
        close(xrep[1]);

        struct helper_report xrp;
        memset(&xrp, 0, sizeof(xrp));
        int xgot = read_full(xrep[0], &xrp, sizeof(xrp));

        CHECK(xgot == 0 && xrp.ok == 1 && xrp.vms_pid != 0,
              "a named process in UIC group 302 registered and burned CPU");

        if (xgot == 0 && xrp.ok == 1) {
            static char unpriv_out[65536];
            if (run_show_system_unpriv(unpriv_out, sizeof(unpriv_out)) != 0) {
                CHECK(0, "P12: SHOW SYSTEM ran under a WORLD-less DCL.EXE");
            } else {
                CHECK(strstr(unpriv_out, SETUP_FAIL_MARK) == NULL,
                      "the SHOW SYSTEM caller really did drop WORLD before exec");

                const char *xrow = row_for(unpriv_out, XGRP_NAME);
                const char *srow = row_for(unpriv_out, SHOW_NAME);

                if (xrow == NULL || srow == NULL)
                    printf("  (P12 rows not found; output follows)\n%s\n",
                           unpriv_out);

                /* Enumeration is not privileged on VMS. */
                CHECK(xrow != NULL,
                      "SHOW SYSTEM lists a process the caller may NOT read, by name");
                /* The control: a row the caller MAY read carries CPU. */
                CHECK(srow != NULL,
                      "SHOW SYSTEM lists the calling process by its own name");
                CHECK(srow != NULL &&
                      row_has_value_after_name(srow, SHOW_NAME),
                      "the readable row carries a CPU figure");
                /* The detector: a row it may NOT read carries none. */
                CHECK(xrow != NULL &&
                      !row_has_value_after_name(xrow, XGRP_NAME),
                      "the UNREADABLE row fabricates NO CPU figure at all");
            }
        }

        close(xcmd[1]);             /* releases the subject */
        close(xrep[0]);
        if (xproc > 0) {
            kill(xproc, SIGKILL);
            int xst;
            while (waitpid(xproc, &xst, 0) < 0 && errno == EINTR)
                ;
        }
    }

    /* ---------------------------------------------------------------
     * P13. A SIGNAL CAUGHT BY THE CALLER MUST NOT CHANGE WHAT $CREPRC
     *      SAYS ABOUT THE CHILD.
     *
     * The creation handshake's read used to be a bare read(), so a
     * signal delivered to the CALLER under a handler without SA_RESTART
     * returned -1/EINTR -- which $CREPRC read as "the child died before
     * reporting" and answered with OVMX$_PRCLOST ("no VMS process was
     * created ... nothing was ever entered in the table") for a process
     * that was created, was in the executive's table and was resolvable
     * BY NAME. Worse, it then reaped that live process, so $CREPRC
     * blocked for the whole lifetime of the image it had just started.
     *
     * That is reachable entirely from the public API, with no race
     * against the child: DCL installs its interactive SIGINT/SIGQUIT
     * handlers with sa_flags = 0 (src/vmsdcl/dcl_main.c) and DCL is what
     * calls $CREPRC. So the probe below adopts that same disposition, has
     * the signal delivered to it while it is provably blocked waiting for
     * the child (see sigprobe_tracer -- the child is held still by ptrace,
     * so this is an ordering, not a race), and asserts the property the
     * status is supposed to carry: every process $CREPRC reports created
     * must be in the table under the name it was given, and the call must
     * return.
     *
     * WHICH ASSERTION IS THE DETECTOR: the bounded completion and
     * `prclost == 0`. `arranged >= SIGPROBE_MIN_ARRANGED` is not a
     * detector, it is the ARRANGEMENT CHECK -- it proves the caller really
     * was interrupted mid-handshake, so a green result means the code
     * survived the condition rather than never meeting it. Without it this
     * block would pass on a run where the tracer never managed it.
     * --------------------------------------------------------------- */
    hs = fopen(HOLD_SCRIPT, "w");           /* P10 unlinked it */
    if (!hs) {
        CHECK(0, "P13: cannot recreate the subject's hold script");
    } else {
        fprintf(hs, "sleep 600\n");
        fclose(hs);
        chmod(HOLD_SCRIPT, 0644);

        int prep[2] = { -1, -1 };
        pid_t probe = -1;

        if (pipe(prep) != 0) {
            CHECK(0, "P13: pipe for the signal probe");
        } else if ((probe = fork()) < 0) {
            CHECK(0, "P13: fork of the signal probe");
        } else if (probe == 0) {
            close(prep[0]);
            sigprobe_tracer(prep[1]);
            _exit(0);                       /* not reached */
        } else {
            struct sigprobe_report prp;
            long long t_start = now_ms(), t_ms;
            int prc;

            close(prep[1]);
            memset(&prp, 0, sizeof(prp));
            prc = read_full_bounded(prep[0], &prp, sizeof(prp),
                                    SIGPROBE_BOUND_MS);
            t_ms = now_ms() - t_start;

            if (prc == -2) {
                /* The defect's signature: $CREPRC blocked on a process
                 * it had just created. Kill the probe's whole group so
                 * its subjects do not outlive it. */
                /* The probe reports ONCE, at the end of its loop, so a
                 * timeout means it is still inside a $CREPRC call -- there
                 * is no partial report to print. */
                printf("  (P13 probe did not report within %d ms -- "
                       "sys$creprc did not return)\n", SIGPROBE_BOUND_MS);
                kill(-probe, SIGKILL);
            }
            close(prep[0]);
            if (probe > 0) {
                int pst;
                kill(probe, SIGKILL);
                while (waitpid(probe, &pst, 0) < 0 && errno == EINTR)
                    ;
            }

            CHECK(prc == 0 && prp.ok == 1,
                  "sys$creprc RETURNED on every call while the caller caught signals");

            if (prc == 0 && prp.ok == 1) {
                printf("  (P13: %u calls, %u interrupted mid-handshake, "
                       "%lld ms of the %d ms bound)\n",
                       (unsigned)prp.iters, (unsigned)prp.arranged,
                       t_ms, SIGPROBE_BOUND_MS);
                /* The ARRANGEMENT check, not a detector: it proves the
                 * caller really was interrupted while waiting for the
                 * child, so a green result means the code survived the
                 * condition rather than never meeting it. */
                CHECK(prp.arranged >= SIGPROBE_MIN_ARRANGED,
                      "the caller really was signalled while blocked in the creation handshake");
                CHECK(prp.prclost == 0,
                      "sys$creprc never reported OVMX$_PRCLOST for a process it created");
                CHECK(prp.failed == 0,
                      "sys$creprc reported no other failure under a non-restarting handler");
                CHECK(prp.unresolvable == 0,
                      "every process sys$creprc reported is in the executive's table, by name");
            }
        }
        unlink(HOLD_SCRIPT);
    }

    printf("=== test_syssvc_procnam: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
