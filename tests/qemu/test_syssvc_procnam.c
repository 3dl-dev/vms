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

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
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
     * P8. lib$getjpi -- the RTL wrapper -- reaches the same executive
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
     * P9. A name is released when its process dies, so it can be taken
     *     again. Proves the table is live state, not an append-only log.
     * --------------------------------------------------------------- */
    uint32_t retaken = 0;
    st = spawn_named(SUBJECT_NAME, &retaken);
    CHECK(st & 1, "the subject's name is available again once it has exited");
    reap(retaken);
    unlink(HOLD_SCRIPT);

    printf("=== test_syssvc_procnam: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
