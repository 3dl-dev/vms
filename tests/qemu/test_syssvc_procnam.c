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
 * against a helper that sits in a DIFFERENT UIC GROUP and burns its OWN CPU,
 * because on a rig where every process is root and idle those three answers
 * read identically whether they come from the executive's row or from the
 * caller's own PCB. A test that cannot tell the two apart is not coverage.
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

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "jpidef.h"
#include "lib$routines.h"
#include "vms_kif.h"
#include "vms/pcb.h"

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

/* What the caller writes into its OWN pcb->username before block P11, so
 * that a username answered out of the caller's private PCB is recognisable
 * as such when it is returned for a DIFFERENT process. */
#define CALLER_PCB_USER "P11CALLER"

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
    waitpid((pid_t)pid, &st, 0);
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

    /* ---------------------------------------------------------------
     * P1. $CREPRC enters the child in the EXECUTIVE's table under the
     *     requested name, and the name survives image activation.
     * --------------------------------------------------------------- */
    uint32_t st = spawn_named(SUBJECT_NAME, &subject);
    CHECK(st & 1, "sys$creprc created the subject process");
    CHECK(subject != 0, "sys$creprc returned the subject's pid");

    if (!(st & 1) || subject == 0) {
        printf("=== test_syssvc_procnam: %d passed, %d failed ===\n", pass, fail + 1);
        return 1;
    }

    CHECK(wait_for_exec(subject, "sh") == 0,
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
    uint32_t selfpid = (uint32_t)getpid();
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

    char ubuf[32];
    memset(ubuf, 0, sizeof(ubuf));
    struct dsc$descriptor_s udesc = str_dsc(ubuf);
    udesc.dsc$w_length = sizeof(ubuf) - 1;
    uint16_t ulen = 0;
    item = JPI$_USERNAME;
    st = lib$getjpi(&item, NULL, NULL, NULL, &udesc, &ulen);
    CHECK(st == SS$_NORMAL, "lib$getjpi(JPI$_USERNAME) returns SS$_NORMAL");
    CHECK(ulen > 0, "lib$getjpi(JPI$_USERNAME) returns a non-empty string");

    char pnbuf[32];
    memset(pnbuf, 0, sizeof(pnbuf));
    struct dsc$descriptor_s pndesc = str_dsc(pnbuf);
    pndesc.dsc$w_length = sizeof(pnbuf) - 1;
    uint16_t pnlen = 0;
    item = JPI$_PRCNAM;
    st = lib$getjpi(&item, NULL, NULL, NULL, &pndesc, &pnlen);
    CHECK(st == SS$_NORMAL, "lib$getjpi(JPI$_PRCNAM) returns SS$_NORMAL");

    reap(subject);
    reap(ok15_pid);

    /* ---------------------------------------------------------------
     * P10. A name is released when its process dies, so it can be taken
     *     again. Proves the table is live state, not an append-only log.
     * --------------------------------------------------------------- */
    uint32_t retaken = 0;
    st = spawn_named(SUBJECT_NAME, &retaken);
    CHECK(st & 1, "the subject's name is available again once it has exited");
    reap(retaken);
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
             * "pcb->uic, else (getgid()<<16)|getuid()") was built and run
             * in this harness: 45/0 -> 43/2, and the two that flipped are
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
             * WHICH OF THESE ACTUALLY DISCRIMINATES, MEASURED, not
             * assumed. Mutation M-B (jpi_cputim's "if (linux_pid ==
             * getpid())" forced to "if (1)", so every answer is
             * getrusage(RUSAGE_SELF)) was built and run in this harness:
             * 45/0 -> 44/1, and the ONE assertion that flipped was
             * "the CPU growth reported is the HELPER's". `t1 > 0` and
             * `t2 > t1` both stayed GREEN under the mutation, because the
             * caller is itself accumulating CPU between the two readings.
             * They are companions, not detectors; the delta comparison is
             * the detector. Do not delete it as redundant.
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
             * The executive's row carries no username yet (vms-2b8 is
             * adding identity to struct vms_proc), so for a non-self
             * target sys$getjpi resolves the account database with the
             * MEMBER half of the executive-reported UIC. Give the
             * CALLER a recognisable self-declared PCB username first:
             * if the non-self branch ever collapses back onto the
             * caller's PCB -- which is exactly what the whole service
             * did before this item -- that string comes back for the
             * helper and the check fails.
             *
             * HONEST LIMIT, stated rather than papered over: this does
             * NOT discriminate "the UIC MEMBER the executive reported"
             * from "getuid()". It cannot, on this rig or in the
             * product: /dev/vms is a root-only misc device, so every
             * process that can reach the executive has uid 0 and both
             * expressions are 0. What IS discriminated is PCB-vs-row,
             * which is the change this item made. The remaining gap
             * closes when vms-2b8 puts a username in the executive's
             * row and the derivation goes away entirely.
             *
             * Mutation M-C (the `is_self ?` guard removed from BOTH the
             * pcb binding and the username branch, i.e. the pre-item
             * "always answer from my own PCB") was built and run in this
             * harness: 45/0 -> 44/1, flipping exactly the
             * "NOT the caller's own PCB username" check below.
             */
            vms_pcb_init(0);
            vms_pcb_set_identity(selfpid, self_uic, CALLER_PCB_USER, "");

            char huser[64], suser[64];
            st = getjpi_str_of(rep.vms_pid, JPI$_USERNAME, huser, sizeof(huser));
            CHECK(st & 1, "sys$getjpi read JPI$_USERNAME for another process");
            CHECK(huser[0] != '\0',
                  "JPI$_USERNAME for another process is not empty");
            CHECK(strcmp(huser, CALLER_PCB_USER) != 0,
                  "JPI$_USERNAME for another process is NOT the caller's own PCB username");

            st = getjpi_str_of(selfpid, JPI$_USERNAME, suser, sizeof(suser));
            CHECK((st & 1) && strcmp(suser, CALLER_PCB_USER) == 0,
                  "JPI$_USERNAME for the CALLER still reads its own PCB (vms-2b8 owns that facade)");
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

    printf("=== test_syssvc_procnam: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
