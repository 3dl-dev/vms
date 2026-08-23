/*
 * sys_process.c - Process Management System Services
 *
 * Implements VMS process control services on top of Linux process
 * primitives. VMS processes are mapped to Linux processes using
 * fork()/exec(), with VMS PIDs being Linux PIDs.
 *
 * Exit handlers and process identity are stored in the Per-Process
 * Control Block (PCB). Exit handlers declared via sys$dclexh are
 * invoked by sys$exit before the process terminates.
 */

/*
 * OVMX userspace service register (rd vms-5b4) -- gate:
 * tests/integration/test_userspace_service_register.sh
 *
 * $CREPRC, $GETJPI and $GETJPIW reach the executive process table. That used to
 * exempt them from saying anything at all; it no longer does (vms-d89), because
 * reaching the executive is not the same as answering from it, and all three of
 * them answer from both places.
 *
 * WHICH ITEM EACH LINE CITES, AND WHY IT CHANGED (vms-fab). Every declaration in
 * this file used to cite vms-8019, which is CLOSED and whose outcome was met: a
 * process created with a prcnam carries that name where other processes can see
 * it, SHOW SYSTEM lists processes DCL did not create, and $GETJPI resolves a
 * process by name -- all against a real /dev/vms. Nobody is coming back for a
 * done item, so the lines below cite the live items that own what is left:
 *   vms-afd  -- historically $CREPRC did not propagate identity to the
 *               executive, so the child's row carried no user name and a
 *               capable()-derived UIC/privilege set. vms-d31d closed that:
 *               $CREPRC now stamps the child's EXECUTIVE row with the
 *               creator's identity (or the prvadr/uic override) via
 *               vms_kif_setident(), so the child inherits the creator's UIC,
 *               user name and privileges the way OpenVMS $CREPRC does.
 *   vms-pt1  -- the executive process table every process can query, with
 *               VMS-meaningful attributes. That is what the eleven services
 *               below reach nothing of, and where $GETJPI's /proc-derived
 *               JPI$_CPUTIM belongs.
 *
 * OVMX-EXEC: sys$creprc (vms-afd, vms-d31d) -- exec: the created process's
 *     NAME is registered with vms_kif_setprn(); its VMS process id, reported
 *     through pidadr, is the one the executive assigned (read back with
 *     vms_kif_getjpi_self()); and its UIC, user name and privilege masks are
 *     stamped onto its executive row with vms_kif_setident(), inherited from
 *     the creator's executive identity (prvadr/uic override it). Another
 *     process reads all of these back through $GETJPI, and the Files-11
 *     reference monitor enforces the stamped UIC/privileges.
 * OVMX-LOCAL: sys$creprc -- the child's default directory and quotas are still
 *     copied only into the child's process-local PCB; no executive row records
 *     those two yet.
 * OVMX-PARTIAL: sys$getjpi (vms-pt1) -- exec: JPI$_PID, JPI$_PRCNAM,
 *     JPI$_USERNAME and JPI$_UIC are read from the row the executive resolved,
 *     by name or by pid, with no PCB consulted -- which is what lets $GETJPI
 *     describe a process other than its caller at all.
 * OVMX-LOCAL: sys$getjpi -- JPI$_CPUTIM is not the executive's: it comes from
 *     getrusage(RUSAGE_SELF) for the caller and from /proc/<pid>/stat otherwise.
 *     The executive keeps no accounting for a process it has a row for.
 * OVMX-PARTIAL: sys$getjpiw (vms-pt1) -- exec: tail-calls sys$getjpi, so the
 *     same executive-resolved row answers the same items.
 * OVMX-LOCAL: sys$getjpiw -- and inherits the same /proc-derived JPI$_CPUTIM.
 *
 * The services below reach nothing at all, and the recurring shape is the
 * discarded prcnam: each one that accepts a VMS process NAME opens with
 * `(void)prcnam;`, so a caller that names its target rather than numbering it
 * is silently redirected to itself or to a raw Linux pid.
 *
 * OVMX-USERSPACE: sys$exit (vms-pt1) -- runs the exit handlers held in
 *     pcb->exit_handlers[] in the per-process PCB, then _exit()s.
 * OVMX-USERSPACE: sys$dclexh (vms-pt1) -- appends to that same per-process
 *     array; no executive records that the process has an exit handler.
 * OVMX-USERSPACE: sys$forcex (vms-pt1) -- with no pidadr it degenerates to
 *     sys$exit on the caller; otherwise kill() by Linux pid. prcnam discarded.
 * OVMX-PARTIAL: sys$delprc (vms-1a8) -- exec: the target is now RESOLVED in
 *     the executive, by prcnam (within the caller's UIC group) or by VMS
 *     pid (any group, gated by WORLD -- see sys$delprc's own comment), the
 *     same way sys$getjpi resolves above; a caller without GROUP/WORLD for
 *     an other-process target is refused SS$_NOPRIV, and a target the
 *     executive does not carry is SS$_NONEXPR.
 * OVMX-LOCAL: sys$delprc -- the actual termination is still kill(SIGTERM)
 *     against the resolved Linux pid: OVMX has no executive-side "delete
 *     this PCB" primitive of its own, so the signal is the mechanism, same
 *     as sys$exit's _exit(). What changed is that this now signals the
 *     process the EXECUTIVE named, not whatever Linux pid number a
 *     caller's argument happened to be.
 * OVMX-PARTIAL: sys$hiber (vms-feb) -- exec: the hibernate WAIT and the wake
 *     state are the executive's. VMS_IOCTL_HIBER blocks the process in the
 *     executive until a $WAKE is pending for it OR an AST becomes deliverable to
 *     it (an entry another process queued into its AST queue), and the sticky
 *     wake bit lives in the executive -- which is what makes $HIBER interruptible
 *     by a cross-process AST at all, proven by tests/qemu/test_syssvc_hiber_ast.c.
 * OVMX-LOCAL: sys$hiber -- the AST DISPATCH after each executive release runs in
 *     THIS process (vms$$deliver_pending_asts calls the routine, a code address
 *     valid only here), and the re-hibernate loop is userspace; that half is not
 *     the executive's.
 * OVMX-PARTIAL: sys$wake (vms-feb) -- exec: the wake state is the executive's.
 *     VMS_IOCTL_WAKE sets the sticky wake_pending bit on the target and wakes it
 *     if it is hibernating; a cross-process target is resolved by VMS PID in the
 *     executive (gated by GROUP/WORLD), not signalled by raw Linux pid.
 * OVMX-LOCAL: sys$wake -- the pidadr-vs-NULL target selection is made in this
 *     process before the call, and $WAKE by process NAME is not resolved (prcnam
 *     discarded, redirecting a named target to self); those are not the
 *     executive's.
 * OVMX-USERSPACE: sys$suspnd (vms-pt1) -- kill(SIGSTOP), same shape.
 * OVMX-USERSPACE: sys$suspend (vms-pt1) -- tail-calls sys$suspnd.
 * OVMX-USERSPACE: sys$resume (vms-pt1) -- kill(SIGCONT), same shape.
 * OVMX-USERSPACE: sys$setpri (vms-pt1) -- getpriority/setpriority on the
 *     CALLING process; pidadr as well as prcnam is discarded, so it cannot
 *     change any other process's priority however it is invoked.
 * OVMX-USERSPACE: sys$cancel (vms-pt1) -- returns SS$_NORMAL without doing
 *     anything; there is no executive I/O queue to cancel against.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <signal.h>
#include <time.h>
#include <stdio.h>
#include "starlet.h"
#include "ovmx_status.h"
#include "prcdef.h"
#include "prvdef.h"
#include "vms/pcb.h"
#include "vms_kif.h"
/* ovmx_boot_stage_exec_path() + OVMX_BOOT_STAGE_DIR: the ACP-read bootstrap
 * bridge (vms-5f0). $CREPRC genuinely fork()+execve()s a new process, so its
 * image needs a POSIX path; with the /vms passthrough retired a SYS$SYSTEM
 * image (e.g. JOB_CONTROL.EXE from the startup phase driver, or DCL.EXE from
 * SPAWN) is execve'd from the boot-staging tmpfs PID 1 filled off the ODS-2
 * volume over the ACP. */
#include "ovmx_layout.h"

/*
 * sys$exit - Terminate process with a VMS status code.
 *
 * Runs any exit handlers registered via sys$dclexh in LIFO order,
 * then exits. VMS convention: odd status = success (exit code 0),
 * even status = failure (exit code 1).
 */
uint32_t sys$exit(uint32_t code) {
    struct vms_pcb *pcb = vms_pcb_get();

    /* Run exit handlers in LIFO order */
    if (pcb) {
        for (int i = pcb->exit_handler_count - 1; i >= 0; i--) {
            struct pcb_exit_handler *blk = pcb->exit_handlers[i];
            if (blk && blk->handler) {
                uint32_t status = code;
                blk->handler(&status);
            }
        }
    }

    int exit_code = (code & 1) ? 0 : 1;  /* VMS success -> 0, failure -> 1 */
    _exit(exit_code);
    return SS$_NORMAL;  /* Never reached */
}

/*
 * jpi_cputim - CPU time of a process, in the 10-millisecond units
 * JPI$_CPUTIM reports.
 *
 * Self is answered from getrusage(RUSAGE_SELF). Another process is
 * answered from /proc/<pid>/stat fields 14 and 15 (utime, stime, in
 * clock ticks). That is not a fabrication and not a way around the
 * executive: the linux_pid it reads came OUT of the executive's row for
 * the resolved process, and CPU time consumed is a real property of a
 * real process, measured by the kernel that ran it -- the same quantity
 * VMS reports, from the only accounting that exists. The OVMX executive
 * does not maintain its own CPU accounting, so there is nothing here to
 * read it from instead.
 *
 * WHAT THIS RETURNS WHEN IT CANNOT MEASURE, AND WHY IT IS NOT A ZERO
 * (vms-8019 round 7).
 *
 * It used to return the literal 0, and sys$getjpi wrote that 0 into
 * JPI$_CPUTIM with a 4-byte return length and SS$_NORMAL -- so a caller
 * could not tell "consumed no CPU" from "OVMX could not measure it".
 * That is the SAME defect the round-4 ruling removed from SHOW SYSTEM
 * one layer above (dcl_cmd_show.c printed a fabricated "0 00:00:00.00"
 * for a row it could not read), left standing here, so the two readers
 * of one facility disagreed about one condition.
 *
 * It is reachable, not theoretical: the executive resolves the row and
 * returns (vms_ioctl_getjpi reaps dead entries first, so the row was
 * LIVE), and the target task can then exit before the /proc open below.
 * The answer is then wrong rather than merely unknown.
 *
 * Rule 10 allows two answers and this is the first one -- MATCH VMS.
 * Every failure path below is the target no longer existing:
 *   fopen() fails            -- /proc/<pid> is gone, so the task is gone
 *   fread() returns 0        -- the task was released between open and
 *                               read; procfs returns ESRCH, not a line
 *   no ')' / fields short    -- not a stat line for a live task
 * and the condition VMS reports for a target that no longer exists is
 * SS$_NONEXPR (%SYSTEM-W-NONEXPR, "process does not exist"; the value is
 * oracle-pinned to 2280 on VAX1, see ssdef.h). VMS has no "the executive
 * could not measure it" status, and inventing one -- or reporting a
 * plausible zero -- is the illegal third answer.
 *
 * On success *out holds the figure and SS$_NORMAL is returned; on
 * failure *out is NOT written and the caller must not report the item.
 */
static uint32_t jpi_cputim(uint32_t linux_pid, uint32_t *out)
{
    if (linux_pid == (uint32_t)getpid()) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        *out = (uint32_t)(
            (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 100 +
            (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 10000);
        return SS$_NORMAL;
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/stat", (unsigned)linux_pid);
    FILE *f = fopen(path, "r");
    if (!f) return SS$_NONEXPR;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return SS$_NONEXPR;
    buf[n] = '\0';

    /* The comm field is parenthesised and may contain spaces, so field
     * splitting has to start after the LAST ')'. utime/stime are the
     * 12th and 13th fields after that point. */
    char *p = strrchr(buf, ')');
    if (!p) return SS$_NONEXPR;
    p++;

    /* strtok_r, never strtok: this runs inside sys$getjpi, a PUBLIC sys$
     * entry point, and strtok() keeps its cursor in a single static that
     * belongs to whoever called us. Clobbering a caller's in-flight
     * tokenisation from inside a system service is a defect on its own,
     * independent of anything the tests reach. */
    unsigned long long utime = 0, stime = 0;
    int field = 0;
    int have_both = 0;
    char *save = NULL;
    for (char *tok = strtok_r(p, " ", &save); tok;
         tok = strtok_r(NULL, " ", &save)) {
        field++;                        /* field 1 here == stat field 3 */
        if (field == 12) utime = strtoull(tok, NULL, 10);
        else if (field == 13) {
            stime = strtoull(tok, NULL, 10);
            have_both = 1;
            break;
        }
    }
    if (!have_both) return SS$_NONEXPR;

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    *out = (uint32_t)(((utime + stime) * 100ULL) / (unsigned long long)hz);
    return SS$_NORMAL;
}

/*
 * sys$getjpi - Get Job/Process Information.
 *
 * A READER OF THE EXECUTIVE PROCESS TABLE (vms-8019, CLAUDE.md Rule 11).
 *
 * This service used to ignore both pidadr and prcnam entirely and answer
 * every question out of the CALLER's own per-process PCB -- so $GETJPI on
 * another process returned the asker's own name, uic and cpu time, and
 * resolving a process BY NAME was not implemented at all. That is the
 * defect this item exists to remove: a process name that only its owner
 * can see is not a VMS process name, it is a variable.
 *
 * The target is now resolved in the executive (src/kernel/vms_proctab.c,
 * behind /dev/vms) and every item that the executive's row carries is
 * answered FROM that row:
 *   JPI$_PID      - the row's VMS process ID
 *   JPI$_PRCNAM   - the row's process name, as set by $SETPRN / $CREPRC
 *   JPI$_UIC      - the row's UIC, derived by the executive from the
 *                   task's real credentials (a process cannot declare it)
 *   JPI$_CPUTIM   - see jpi_cputim() above
 *   JPI$_USERNAME - the row's authenticated username (vms-2b8), empty
 *                   until an identity is stamped; see the item's comment
 *
 * Selection follows the VMS argument rules: prcnam names a process,
 * otherwise a non-zero *pidadr names one, otherwise the caller. There is
 * NO fallback to local state when the executive cannot resolve the
 * target -- the honest answer is SS$_NONEXPR, which is what VMS returns
 * for a process that does not exist.
 */
uint32_t sys$getjpi(uint32_t efn, const uint32_t *pidadr,
                    void *prcnam_arg,
                    void *itmlst_arg,
                    void *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm) {
    (void)efn; (void)iosb; (void)astadr; (void)astprm;

    /* prcnam/itmlst are void* in the VSI prototype (see starlet.h) so callers
     * may pass any descriptor / item-list flavour (ILE3, item_list_3, ...);
     * ILE3 and item_list_3 share an identical layout. */
    const struct dsc$descriptor_s *prcnam =
        (const struct dsc$descriptor_s *)prcnam_arg;
    const struct item_list_3 *itmlst =
        (const struct item_list_3 *)itmlst_arg;

    if (!itmlst) return SS$_BADPARAM;

    struct vms_procinfo info;
    uint32_t status;

    if (prcnam && prcnam->dsc$a_pointer && prcnam->dsc$w_length > 0) {
        /* The key travels untruncated: VMS_PRCNAM_XFER is deliberately
         * larger than any legal process name so that an oversized name
         * is REJECTED by the executive (SS$_IVLOGNAM) instead of being
         * clipped into a valid one that resolves a different process.
         * See the VMS_PRCNAM_XFER comment in src/kernel/vms_ioctl.h. */
        char key[VMS_PRCNAM_XFER];
        dsc$strncpy(key, prcnam, sizeof(key));
        status = vms_kif_getjpi_prcnam(key, &info);
    } else if (pidadr && *pidadr != 0) {
        status = vms_kif_getjpi_pid(*pidadr, &info);
    } else {
        status = vms_kif_getjpi_self(&info);
    }

    if (!(status & 1))
        return status;

    /*
     * NO PCB IS CONSULTED. Every item below is answered from the row the
     * executive resolved. The caller's own vms_pcb_get() used to supply
     * the name, the username and the UIC -- values the process had
     * written about itself -- which is why $GETJPI could only ever
     * describe its caller. Reintroducing a PCB read here reintroduces
     * that facade (CLAUDE.md Rule 11).
     */
    for (const struct item_list_3 *item = itmlst;
         item->buflen != 0 || item->item_code != 0; item++) {
        switch (item->item_code) {
            case JPI$_PID:
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = info.vms_pid;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;

            case JPI$_PRCNAM:
                /*
                 * The row's name, verbatim -- including the empty string
                 * for a process that has never been named.
                 *
                 * This used to invent "_%08X" from the pid when the PCB
                 * carried no name. That invented name was exactly the
                 * facade this item exists to delete: nothing else could
                 * resolve it (the executive skips unnamed rows in
                 * find_by_name), so $GETJPI reported a name to its owner
                 * that no other process could see.
                 *
                 * UNPINNED, AND OWNED BY vms-6a7 (which is blocked by
                 * this item). Deleting a wrong answer did not produce a
                 * right one: the empty name is a PLACEHOLDER, chosen
                 * without the oracle, not a match. The question vms-6a7
                 * must put to the oracle, verbatim: "What does OpenVMS
                 * VAX 7.3 report for JPI$_PRCNAM of a process created
                 * with no process name, and does the executive assign one
                 * at creation?" Until then OVMX reports no name rather
                 * than one only its owner believes in. Do not settle it
                 * here -- see the matching comment in
                 * src/vmsdcl/dcl_cmd_show.c cmd_show_system().
                 */
                if (item->bufaddr) {
                    uint16_t len = (uint16_t)strlen(info.prcnam);
                    if (len > item->buflen) len = item->buflen;
                    memcpy(item->bufaddr, info.prcnam, len);
                    if (item->retlen) *item->retlen = len;
                }
                break;

            case JPI$_USERNAME: {
                /*
                 * THE ROW'S USERNAME, AND NOTHING ELSE.
                 *
                 * vms-2b8 put an authenticated username in the
                 * executive's row (struct vms_procinfo.username, stamped
                 * by vms_kif_setident after SYSUAF authentication and
                 * refused to any caller that would be widening its own
                 * identity). That makes this a straight read, from the
                 * only place a VMS username has ever lived.
                 *
                 * TWO SOURCES WERE DELETED HERE, not kept as fallbacks:
                 *  - pcb->username, a value the process wrote about
                 *    itself, so $GETJPI reported to a process what that
                 *    process had already told it (and, before this item,
                 *    reported it for OTHER processes too);
                 *  - getpwuid(UIC member), a passwd-database derivation
                 *    this item's earlier round introduced when the row
                 *    had no username. It looked reasonable and it was
                 *    the illegal third answer (Rule 10): OVMX's runtime
                 *    ships no passwd database at all, so in the product
                 *    it always failed and always returned the literal
                 *    "UNKNOWN" -- a made-up name reported with
                 *    SS$_NORMAL.
                 *
                 * A row with no identity stamped reports an EMPTY name
                 * with a zero return length. That is what the executive
                 * holds, so that is what is reported; a name is not
                 * invented to fill the field.
                 */
                uint16_t len = (uint16_t)strlen(info.username);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, info.username, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case JPI$_UIC:
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = info.uic;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;

            case JPI$_CPUTIM: {
                /*
                 * THE WHOLE CALL FAILS IF THE TARGET HAS GONE.
                 *
                 * jpi_cputim() only fails when the process the executive
                 * resolved no longer exists (see its comment), and
                 * SS$_NONEXPR describes the TARGET, not this item -- the
                 * items already written above describe a process that has
                 * since ceased to exist, so reporting SS$_NORMAL over the
                 * top of them would hand the caller a stale row and call
                 * it current. Nothing is written for this item and no
                 * return length is set: a caller must never receive a
                 * measured-looking zero for an unmeasurable process.
                 */
                uint32_t cputim = 0;
                uint32_t cpustat = jpi_cputim(info.linux_pid, &cputim);
                if (!(cpustat & 1))
                    return cpustat;
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = cputim;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;
            }

            default:
                break;
        }
    }

    return SS$_NORMAL;
}

/*
 * sys$getjpiw - Get Job/Process Information (synchronous).
 * Same as sys$getjpi since our implementation is already synchronous.
 */
uint32_t sys$getjpiw(uint32_t efn, const uint32_t *pidadr,
                     void *prcnam,
                     void *itmlst,
                     void *iosb,
                     void (*astadr)(uint32_t), uint32_t astprm) {
    return sys$getjpi(efn, pidadr, prcnam, itmlst, iosb, astadr, astprm);
}

/* AST drain, shared with sys$setast (vms$$ internal-helper convention). */
extern void vms$$deliver_pending_asts(void);

/*
 * sys$hiber - Hibernate the current process, INTERRUPTIBLE BY AST DELIVERY
 * (vms-feb).
 *
 * The wait, the wake state and the AST-arrival notification are all the
 * EXECUTIVE's now, not a bare pause(): vms_kif_hiber() blocks in /dev/vms until
 * a $WAKE is pending for this process OR an AST becomes deliverable to it (an
 * entry another process queued into this process's AST queue -- a mailbox
 * write-attention write, a lock completion/blocking AST, or a $DCLAST). That is
 * what makes a process B action -- writing the mailbox this process armed a
 * write-attention AST on -- actually interrupt this process's $HIBER, run the
 * AST, and (if the AST issues a $WAKE) return from $HIBER. It is the
 * $HIBER/$WAKE + AST pattern MMK's send_cmd_and_wait needs (spine #4, vms-b23);
 * without it a $HIBER waiting on such an AST would deadlock.
 *
 * VMS semantics (VSI System Services Reference, $HIBER): an AST that does NOT
 * issue a $WAKE runs and the process CONTINUES to hibernate; only a $WAKE ends
 * it (and a $WAKE that preceded the $HIBER makes it fall straight through). The
 * loop reproduces exactly that: each executive return drains and runs the
 * deliverable ASTs in THIS process (the routine address is a code address here
 * -- an AST is dispatched in the process it was queued to), and $HIBER returns
 * only when the release was a $WAKE (woken == 1), whether that $WAKE arrived on
 * its own or was issued by one of the ASTs just run.
 */
uint32_t sys$hiber(void) {
    for (;;) {
        int woken = 0;
        uint32_t st = vms_kif_hiber(&woken);  /* blocks: $WAKE pending or AST ready */
        /* Honest failure, never a busy-spin (CLAUDE.md Rule 9 / INV-6): if the
         * executive did not answer (odd = success), return its status rather
         * than looping forever. With no /dev/vms there is no hibernate state to
         * wait on -- fail, do not fake a wait. */
        if (!(st & 1))
            return st;
        vms$$deliver_pending_asts();     /* run deliverable ASTs; one may $WAKE */
        if (woken)
            return SS$_NORMAL;
        /* An AST (not a $WAKE) released the wait: re-hibernate, per VMS. */
    }
}

/*
 * sys$wake - Wake a hibernating process ($WAKE), executive-resident (vms-feb).
 *
 * Sets the target's sticky wake bit in the executive and wakes it if it is
 * hibernating in $HIBER. With no pidadr the target is the CALLING process --
 * the self-directed $WAKE a write-attention AST issues to end its own process's
 * $HIBER (the MMK case). A non-NULL pidadr names a VMS PID, resolved by the
 * executive (SS$_NONEXPR if it names no process, SS$_NOPRIV without GROUP/WORLD
 * for another process's target) -- the same VMS-PID targeting the executive
 * does for $DELPRC/$GETJPI, not the raw Linux pid the old SIGCONT shim used.
 *
 * prcnam-based targeting is not resolved (the executive's $WAKE takes a VMS
 * PID); a caller that names its target by string is redirected to itself, the
 * same limitation the other prcnam-taking process services carry.
 */
uint32_t sys$wake(const uint32_t *pidadr,
                  const struct dsc$descriptor_s *prcnam) {
    (void)prcnam;
    return vms_kif_wake(pidadr ? *pidadr : 0);
}

/*
 * creprc_write_all / creprc_read_all -- the $CREPRC creation handshake's
 * transfers, made SIGNAL-PROOF.
 *
 * WHY THESE EXIST (the defect they delete): a bare read() on the pipe
 * returns -1/EINTR whenever a signal is delivered to the CALLER through a
 * handler installed without SA_RESTART. That is not an exotic condition --
 * src/vmsdcl/dcl_main.c installs DCL's interactive SIGINT and SIGQUIT
 * handlers with sa_flags = 0, and DCL is the process that calls $CREPRC
 * (RUN, RUN/DETACHED). A Ctrl-C landing in the handshake window made the
 * short-read branch fire with the child PERFECTLY HEALTHY: $CREPRC reported
 * OVMX$_PRCLOST -- "no VMS process was created, nothing was ever entered in
 * the table" -- for a process that WAS created, IS in the executive's table
 * and IS enumerable by SHOW SYSTEM, and then reaped it. Measured on a
 * NATIVE host against this branch's own LIBVMS$SHR.EXE and recorded on
 * vms-8019: 788 of 4000 calls under a DCL-shaped handler, 0 of 4000 without
 * one. (Under QEMU the same window is a fraction of a millisecond wide,
 * which is why tests/qemu/test_syssvc_procnam.c's P13 holds the forked
 * child still with ptrace instead of trying to catch it.)
 *
 * The signal disposition of the caller is not a fact about the child, so it
 * must not be able to change what $CREPRC says about the child. With these
 * loops in place a short read really does mean "the child died before
 * reporting", which is the only condition OVMX$_PRCLOST is documented to
 * name, and is what makes the unconditional waitpid() below correct rather
 * than an indefinite block on a live process.
 *
 * The child's write needs the same treatment for the same reason with the
 * roles swapped: a child that reported successfully but was interrupted
 * mid-write still exec's its image, and the parent would see a short read
 * and declare a running process lost.
 *
 * A genuine EOF (read returns 0) and a genuine error other than EINTR both
 * stop the loop and are reported as a short transfer.
 */
static ssize_t creprc_write_all(int fd, const void *buf, size_t len)
{
    const char *p = (const char *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t w = write(fd, p + done, len - done);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (w == 0) break;
        done += (size_t)w;
    }
    return (ssize_t)done;
}

static ssize_t creprc_read_all(int fd, void *buf, size_t len)
{
    char *p = (char *)buf;
    size_t done = 0;
    while (done < len) {
        ssize_t r = read(fd, p + done, len - done);
        if (r < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (r == 0) break;      /* genuine EOF: the writer is gone */
        done += (size_t)r;
    }
    return (ssize_t)done;
}

/*
 * sys$creprc - Create a process.
 *
 * Two kinds of process, selected by the stsflg argument:
 *
 *   default        a SUBPROCESS. The creator stays its parent and is
 *                  expected to wait on it.
 *
 *   PRC$M_DETACH   a DETACHED process. Not part of the creator's job
 *                  tree, owns no controlling terminal, and outlives the
 *                  creator -- the creator cannot wait on it at all.
 *                  This is what RUN/DETACHED and the system startup
 *                  procedures create for services.
 *
 * Uses fork()+exec() to create a new process running the specified image.
 * Inherits VMS context (privileges, UIC, username, quotas) from the
 * parent PCB when parameters are zero/NULL. I/O redirection is set up
 * from the input/output/error descriptors.
 * The new process PID is returned in *pidadr if non-NULL.
 *
 * Reference: OpenVMS System Services Reference Manual ($CREPRC).
 */
uint32_t sys$creprc(uint32_t *pidadr, const struct dsc$descriptor_s *image,
                    const struct dsc$descriptor_s *input,
                    const struct dsc$descriptor_s *output,
                    const struct dsc$descriptor_s *error,
                    const void *prvadr, const void *quota,
                    const struct dsc$descriptor_s *prcnam,
                    uint32_t baspri, uint32_t uic, uint32_t mbxunt,
                    uint32_t stsflg) {
    (void)quota; (void)baspri; (void)mbxunt;

    if (!image || !image->dsc$a_pointer) return SS$_BADPARAM;

    const int detached = (stsflg & PRC$M_DETACH) != 0;

    char img_path[512];
    dsc$strncpy(img_path, image, sizeof(img_path));

    /* ATOMIC FLIP (vms-5f0): a genuinely-new process ($CREPRC/RUN-DETACHED/
     * SPAWN) is execve'd by the Linux kernel, which maps the MAIN image's
     * PT_LOAD and opens its PT_INTERP by POSIX path. With the /vms passthrough
     * retired, a SYS$SYSTEM image is execve'd from the boot-staging tmpfs
     * PID 1 filled off the ODS-2 volume THROUGH the executive ACP. Self-
     * guarding: rewrite only when the staged copy is actually present, so a
     * non-staged or non-Linux path is left unchanged and fails honestly. */
    {
        char staged[512];
        if (ovmx_boot_stage_exec_path(img_path, staged, sizeof(staged)) &&
            access(staged, X_OK) == 0)
            snprintf(img_path, sizeof(img_path), "%s", staged);
    }

    struct vms_pcb *parent_pcb = vms_pcb_get();

    /*
     * INHERITANCE BASE -- the CREATOR's authentic EXECUTIVE identity
     * (vms-d31d).
     *
     * $CREPRC gives the created process the creator's privileges, UIC and
     * user name by default; a caller-supplied prvadr/uic overrides them
     * (OpenVMS System Services Reference, $CREPRC). Those masks must reach
     * the child's EXECUTIVE row -- the row the Files-11 reference monitor
     * (vmsfs_acp.c acp_check_access) and $GETJPI read -- not merely the
     * child's process-local PCB. Until this item they reached only the PCB:
     * the child registered with a capable()-derived identity (UIC from
     * Linux credentials, privileges = VMS_PRV_M_ENFORCED, which carries
     * neither SYSPRV nor BYPASS). On x86_64 that was masked because root
     * maps to UIC group 0, which the SYSTEM protection category covers
     * (group <= MAXSYSGROUP); on VAX, with no such mapping, a detached
     * LOGINOUT created by SYSTEM [1,4] fell through to the WORLD nibble of
     * SYS$SYSTEM:SYSUAF.DAT and was refused its own authorization read.
     *
     * The creator reads its OWN executive identity here (authentic -- a
     * process cannot forge the row the executive holds for it), and the
     * child stamps the resulting identity after it registers, via
     * vms_kif_setident() below.
     */
    struct vms_procinfo creator_info;
    memset(&creator_info, 0, sizeof(creator_info));
    int have_creator = (vms_kif_getjpi_self(&creator_info) & 1) != 0;

    /* Determine child UIC: explicit uic, else the creator's. */
    uint32_t child_uic = uic;
    if (child_uic == 0)
        child_uic = have_creator ? creator_info.uic
                                 : (parent_pcb ? parent_pcb->uic : 0);

    /*
     * Determine child privileges.
     *
     * With prvadr the child gets the requested set, but a creator may grant
     * only privileges it is itself AUTHORIZED to hold, UNLESS it holds
     * SETPRV -- SETPRV is what authorizes exceeding the authorized mask
     * ($CREPRC/AUTHORIZE semantics; the same rule vms_ioctl_setident
     * enforces). Without prvadr the child inherits the creator's current
     * privileges. The executive re-checks this when the child stamps the
     * identity: a child without SETPRV cannot stamp a superset of its own
     * authorized mask, so a clamp missed here fails honestly rather than
     * fabricating privilege (INV-6).
     */
    uint64_t child_privs;
    if (prvadr) {
        uint64_t req = *(const uint64_t *)prvadr;
        if (have_creator && !(creator_info.perm_privs & PRV$M_SETPRV))
            child_privs = req & creator_info.perm_privs;
        else
            child_privs = req;
    } else if (have_creator) {
        child_privs = creator_info.cur_privs;
    } else if (parent_pcb) {
        child_privs = parent_pcb->cur_privs;
    } else {
        child_privs = 0;
    }

    /*
     * The user name the child's executive identity carries -- the creator's,
     * so a detached process created by SYSTEM is SYSTEM in the executive's
     * table, not a nameless capable()-derived row. Empty when the creator
     * itself has no executive name: the child then keeps its registration
     * identity rather than being stamped with an invented name (Rule 10).
     */
    char child_username[VMS_USERNAME_SIZE] = {0};
    if (have_creator && creator_info.username[0])
        strncpy(child_username, creator_info.username, sizeof(child_username) - 1);
    else if (parent_pcb && parent_pcb->username[0])
        strncpy(child_username, parent_pcb->username, sizeof(child_username) - 1);

    /*
     * Process name.
     *
     * Held in an inbound transfer buffer, not a VMS_PRCNAM_SIZE field:
     * an oversized name must reach the EXECUTIVE intact so the executive
     * rejects it (SS$_IVLOGNAM), exactly as VMS does. Clipping it here
     * would create a process under a name the caller never asked for.
     */
    char child_prcnam[VMS_PRCNAM_XFER] = {0};
    if (prcnam && prcnam->dsc$a_pointer && prcnam->dsc$w_length > 0)
        dsc$strncpy(child_prcnam, prcnam, sizeof(child_prcnam));

    /*
     * CREATION HANDSHAKE -- OVMX design choice, matching VMS's OBSERVABLE
     * semantics (CLAUDE.md Rule 8: labelled as OVMX's own mechanism).
     *
     * On VMS the EXECUTIVE creates the process: it assigns the process ID
     * that $CREPRC hands back in pidadr, and it names the process as it
     * creates it, so a name clash is reported to the CREATOR:
     *   Oracle (VAX1, OpenVMS VAX V7.3, recorded on this item): a third
     *   detached process taking a PROCESS_NAME already held in the same
     *   UIC group is refused with %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM.
     *
     * OVMX creates the process with fork(), and only the child can enter
     * itself in the executive's table (the entry is keyed by ITS tgid).
     * So the child registers with the executive, performs $SETPRN if a
     * name was asked for, and reports BOTH the status AND the process ID
     * the executive assigned it back over a pipe. $CREPRC returns that
     * status, and that process ID, to its caller. The pipe is O_CLOEXEC,
     * so it costs the activated image nothing.
     *
     * WHY pidadr CANNOT BE THE fork() RETURN (vms-2b8 changed this): the
     * executive now ASSIGNS VMS process IDs from its own generator
     * (src/kernel/vms_module.c assign_vms_pid) instead of adopting
     * getpid(). Handing the caller a Linux pid would hand it a number
     * $GETJPI cannot resolve -- a process ID that names no process, which
     * is precisely the "reports success while sharing nothing" shape this
     * item exists to delete. The handshake therefore runs for EVERY
     * created process, named or not: a VMS process has a PCB from the
     * moment it exists, and pidadr must mean the same thing in both
     * cases.
     *
     * Naming happens BEFORE the exec and before the I/O redirection: the
     * executive entry is keyed by the pid, which execve() does not
     * change, so the name survives image activation with no userspace
     * carrier of any kind. That is the whole point -- the rejected
     * VMS_PRCNAM environment-variable "fix" carried a name the image
     * could only tell itself.
     */
    struct creprc_report {
        uint32_t status;
        uint32_t vms_pid;
    };

    if (pidadr) *pidadr = 0;

    int namefd[2] = { -1, -1 };
    if (pipe(namefd) < 0)
        return SS$_INSFMEM;
    /* Not pipe2(O_CLOEXEC): that needs _GNU_SOURCE, and this file is
     * built against both glibc and musl. */
    fcntl(namefd[0], F_SETFD, FD_CLOEXEC);
    fcntl(namefd[1], F_SETFD, FD_CLOEXEC);

    pid_t pid = fork();
    if (pid < 0) {
        close(namefd[0]);
        close(namefd[1]);
        return SS$_INSFMEM;
    }

    if (pid == 0) {
        /* Child: enter the executive's process table -- under the
         * requested name, if one was requested -- before anything else
         * can fail, and tell the creator how it went and what process ID
         * the executive gave it. An unnamed process is expressed by never
         * calling $SETPRN, never by naming it something invented. */
        struct creprc_report rep = { 0, 0 };
        struct vms_procinfo self_info;

        close(namefd[0]);

        /*
         * PRC$M_DETACH: BECOME A DETACHED PROCESS BEFORE REGISTERING.
         *
         * A VMS detached process is not in the creator's job tree and
         * has no controlling terminal, so the creator cannot wait on it
         * and it survives the creator exiting. Under fork() that is two
         * steps and both must happen HERE, in the created task, before
         * it enters the executive's table:
         *
         *   setsid()  leaves the creator's session and controlling
         *             terminal, so a hangup on the creator's terminal
         *             does not reach the service.
         *   fork()    the process that actually runs the image is the
         *             GRANDchild; this intermediate exits immediately,
         *             so the grandchild is reparented away from the
         *             creator and waitpid() from the creator fails with
         *             ECHILD. That is the observable difference between
         *             a detached process and a subprocess.
         *
         * The grandchild inherits namefd[1] and is therefore the task
         * that reports back, so $CREPRC's pidadr is still the process ID
         * the executive assigned to the process that RUNS THE IMAGE --
         * never the intermediate's. The creation handshake is unchanged
         * and unconditional: a detached process is a VMS process, so it
         * has a PCB and a process ID from the moment it exists.
         *
         * The intermediate closes its copy of the write end before
         * exiting, so if the grandchild dies before reporting, the
         * creator's read still sees EOF (and answers OVMX$_PRCLOST)
         * rather than blocking on a descriptor nothing will ever write.
         */
        if (detached) {
            setsid();
            pid_t svc = fork();
            if (svc < 0)
                _exit(1);           /* creator reads EOF -> OVMX$_PRCLOST */
            if (svc > 0) {
                close(namefd[1]);
                _exit(0);           /* reparent the grandchild away */
            }
        }
        rep.status = child_prcnam[0] ? vms_kif_setprn(child_prcnam)
                                     : SS$_NORMAL;
        if (rep.status & 1) {
            uint32_t gst = vms_kif_getjpi_self(&self_info);
            if (gst & 1)
                rep.vms_pid = self_info.vms_pid;
            else
                rep.status = gst;
        }
        /*
         * STAMP THE INHERITED EXECUTIVE IDENTITY (vms-d31d).
         *
         * The child now has an executive row (setprn or getjpi_self
         * registered it, with a capable()-derived identity). Replace that
         * identity with the one $CREPRC computed from the creator's row and
         * any prvadr/uic override, so the child's EXECUTIVE UIC and
         * privilege masks -- the ones vmsfs_acp.c's acp_check_access and
         * $GETJPI read -- are the creator's (or the requested set), not the
         * registration default. This is what makes a detached LOGINOUT
         * created by SYSTEM [1,4] hold SYSTEM's UIC and privileges and read
         * its own World-denied SYSUAF, on VAX and Alpha as on x86_64,
         * WITHOUT leaning on the root->UIC-group-0 mapping.
         *
         * Only stamped when there is a name to stamp; the executive refuses
         * a caller that lacks the authority to establish the identity
         * (SS$_NOPRIV -- a child without SETPRV cannot grant itself a
         * superset of its authorized mask), and that refusal becomes
         * $CREPRC's status. It is never papered over with a fabricated
         * success (INV-6, CLAUDE.md Rule 9).
         */
        if ((rep.status & 1) && child_username[0]) {
            uint32_t ist = vms_kif_setident(child_username, child_uic,
                                            child_privs);
            if (!(ist & 1))
                rep.status = ist;
        }
        /* Retried on EINTR and on a short write: an interrupted report
         * from a child that is about to exec its image would reach the
         * creator as a short read, i.e. as OVMX$_PRCLOST for a running
         * process. */
        ssize_t w = creprc_write_all(namefd[1], &rep, sizeof(rep));
        (void)w;
        close(namefd[1]);
        /* A report the creator never received describes a process the
         * creator does not know exists. The creator reads a short transfer
         * as OVMX$_PRCLOST -- "nothing was created" -- so the child must
         * not go on to activate the image and make that statement false.
         * Only a genuine write error can end the loop short, and this
         * keeps "short transfer" and "no process" the same fact rather
         * than two facts that can disagree. */
        if (!(rep.status & 1) || w != (ssize_t)sizeof(rep))
            _exit(1);

        /* Child: Initialize PCB with inherited context */
        struct vms_pcb *child_pcb = vms_pcb_init(child_privs);
        if (child_pcb) {
            const char *username = child_username[0] ? child_username
                                 : (parent_pcb ? parent_pcb->username
                                               : "UNKNOWN");
            vms_pcb_set_identity((uint32_t)getpid(), child_uic,
                                 username, child_prcnam);
            if (parent_pcb && parent_pcb->default_dir[0])
                vms_pcb_set_default_dir(parent_pcb->default_dir);
            /* Inherit quotas from parent */
            if (parent_pcb)
                memcpy(child_pcb->quotas, parent_pcb->quotas,
                       sizeof(child_pcb->quotas));
        }

        /*
         * Set up I/O redirection.
         *
         * A stream the caller did not supply is INHERITED for a
         * subprocess and attached to the null device for a DETACHED
         * process. That is not a default this code picked out of the
         * air: "no controlling terminal" is the defining property of a
         * detached process, and a detached process that kept the
         * creator's console fds would be holding the creator's terminal
         * open -- contradicting the property in the one place it is
         * observable. Mapping VMS's null device onto /dev/null here is
         * an OVMX implementation detail (CLAUDE.md Rule 8), not a
         * claimed byte-level VMS behaviour.
         */
        if (input && input->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, input, sizeof(path));
            int fd = open(path, O_RDONLY);
            if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
        } else if (detached) {
            int fd = open("/dev/null", O_RDONLY);
            if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
        }
        if (output && output->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, output, sizeof(path));
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
        } else if (detached) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
        }
        if (error && error->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, error, sizeof(path));
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, STDERR_FILENO); close(fd); }
        } else if (detached) {
            int fd = open("/dev/null", O_WRONLY);
            if (fd >= 0) { dup2(fd, STDERR_FILENO); close(fd); }
        }

        execl(img_path, img_path, (char *)NULL);
        _exit(1);  /* exec failed */
    }

    /* Parent process */
    close(namefd[1]);
    struct creprc_report rep = { 0, 0 };
    /* Retried on EINTR: see creprc_read_all. A signal caught by the
     * CALLER must not be able to change what $CREPRC says about the
     * CHILD, and it is what makes r < sizeof(rep) mean "the child died
     * before reporting" -- the precondition of both the OVMX$_PRCLOST
     * return and the unconditional waitpid() below. */
    ssize_t r = creprc_read_all(namefd[0], &rep, sizeof(rep));
    close(namefd[0]);

    /*
     * DETACHED: `pid` is the INTERMEDIATE, which has already exited, and
     * it is the only task the creator may ever wait for. Reaping it is
     * unconditional -- it is not the process that was created, so it is a
     * zombie on BOTH the success and the failure path. After this the
     * creator has no children left from this call, which is the whole
     * point: waitpid() for the service fails with ECHILD.
     *
     * The service itself is never reaped here and must not be. If the
     * GRANDchild failed to register it exited on its own and init reaped
     * it; if it registered, it is running the image and is no longer
     * related to this process at all.
     */
    if (detached) {
        int mstatus;
        while (waitpid(pid, &mstatus, 0) < 0 && errno == EINTR)
            ;
    }

    if (r != (ssize_t)sizeof(rep) || !(rep.status & 1)) {
        /*
         * EITHER WAY, THE CHILD IS DEAD AND MUST BE REAPED HERE. Only
         * the name-refusal case used to be reaped, so the short-read
         * case leaked a zombie for a process the caller was
         * simultaneously told had been created.
         *
         * THAT CLAIM IS TRUE ONLY BECAUSE OF creprc_read_all(): a bare
         * read() also came back short when the CALLER caught a signal
         * under a handler without SA_RESTART, and this branch then ran
         * against a live, correctly created process -- either killing it
         * (the close() above turns its report into SIGPIPE) or blocking
         * the waitpid() below for the whole life of the image it had
         * just started, depending only on which of the two reached the
         * pipe first. Both halves of this branch -- the status and the
         * reap -- depend on a short transfer meaning the child died.
         * The child likewise refuses to activate its image if its own
         * report did not get through, so the two cannot disagree.
         *
         *  - r == sizeof(rep) with a failure status: the executive
         *    refused the name (SS$_DUPLNAM within the UIC group, or
         *    SS$_IVLOGNAM for a malformed one) and the child exited
         *    itself. The caller gets the executive's OWN status.
         *
         *  - anything else: the child never got as far as reporting.
         *    NO VMS PROCESS EXISTS -- the executive's table is keyed by
         *    the child's tgid, so a child that died before registering
         *    was never in it. The caller gets OVMX$_PRCLOST, an
         *    OVMX-defined condition value (customer-defined bit set),
         *    because OpenVMS has no condition for this: its executive
         *    creates the process, so a process ID always exists by the
         *    time $CREPRC returns. See the OVMX$_PRCLOST comment in
         *    src/libvms/include/ovmx_status.h for why an OVMX-defined
         *    code is legal here and why reporting it as a VMS status
         *    would be a lie about VMS.
         *
         * WHAT THIS REPLACED: SS$_NORMAL with *pidadr left at zero --
         * success paired with a process ID that names no process, a
         * combination VMS does not produce.
         *
         * DETACHED: the reap already happened above, and `pid` named the
         * intermediate rather than the created task in the first place.
         * Waiting again here would either return ECHILD immediately or,
         * worse, reap an unrelated child of the caller.
         */
        if (!detached) {
            int wstatus;
            while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR)
                ;
        }
        return (r == (ssize_t)sizeof(rep)) ? rep.status : OVMX$_PRCLOST;
    }

    /*
     * The ONLY success return in this function, and it is structurally
     * unreachable without a process ID the executive assigned: rep was
     * read whole and rep.status is a success, so rep.vms_pid is the
     * value the child read back out of its own executive row. There is
     * no path here that reports success with a zero process ID.
     */
    if (pidadr)
        *pidadr = rep.vms_pid;
    return SS$_NORMAL;
}

/*
 * sys$delprc - Delete (terminate) a process (vms-1a8).
 *
 * A READER (to resolve the target) and ACTOR (to terminate it) against the
 * EXECUTIVE process table, replacing the OVMX-USERSPACE shape documented at
 * the top of this file: the old sys$delprc treated its pidadr argument as a
 * bare Linux pid number and discarded prcnam outright, so it had never gone
 * near the executive and could not implement $DELPRC's documented interface
 * at all -- there was no path here from a VMS process NAME to anything this
 * function understood. That is what left DCL's STOP command (cmd_stop,
 * src/vmsdcl/dcl_cmd_process.c) unable to do anything but self-exit: it had
 * no real service under it to call.
 *
 * The target is resolved exactly as sys$getjpi resolves it above: prcnam
 * names a process WITHIN THE CALLER'S OWN UIC GROUP (vms_kif_getjpi_prcnam,
 * src/kernel/vms_proctab.c find_by_name), otherwise a non-zero *pidadr names
 * one BY VMS PROCESS ID in ANY group (vms_kif_getjpi_pid), otherwise the
 * caller. A target the executive does not carry comes back as the
 * executive's own SS$_NONEXPR -- never a raw kill() against a Linux pid
 * number nothing in the executive ever named.
 *
 * PRIVILEGE. OpenVMS DCL Dictionary, STOP command (process-name parameter /
 * IDENTIFICATION qualifier form; https://www.mrynet.com/FTP/
 * operatingsystems/VMS/docs/ssb71/9996/9996p062.htm, "OpenVMS DCL
 * Dictionary"): the process-name parameter is scoped to "the same group
 * number in its UIC as the current process"; stopping another process in
 * your own group requires GROUP privilege, stopping one outside it requires
 * WORLD, and /IDENTIFICATION=pid is "an alternative to the process-name
 * parameter" for reaching a process outside the caller's group. Deleting
 * the caller's OWN process takes no privilege at all (there is nothing to
 * authorize).
 *
 * The WORLD half of that rule is already enforced by the RESOLUTION above,
 * not by a separate check here: a cross-group pid target cannot even be
 * READ without WORLD (vms_proc_may_read() in vms_proctab.c refuses it with
 * SS$_NOPRIV before this function ever sees a row), and the process-name
 * selector never searches outside the caller's group in the first place --
 * matching the Dictionary's own "process-name is same-group-only, use
 * /IDENTIFICATION to reach further" division of labour. So a target that
 * reaches the code below has ALREADY proven it is either the caller's own
 * process, a same-group process, or a cross-group process the caller holds
 * WORLD to even see -- and the WORLD case needs no further gate. Only the
 * same-group "another process" case is still ungated at that point, and
 * that is what the explicit check below closes.
 *
 * THIS IS A DIFFERENT FINDING FROM vms_proc_may_read()'s, DELIBERATELY.
 * That function's own comment records the oracle-measured, counter-to-
 * expectation result that a same-group $GETJPI READ needs NO privilege at
 * all. Reusing that verdict for TERMINATION here would be exactly the
 * confusion CLAUDE.md Rule 10 warns against: two different operations, and
 * the Dictionary is explicit they carry different requirements for the
 * same-group case (freely readable; GROUP-gated to delete). The GROUP/WORLD
 * halves of THIS check are grounded in the Dictionary text quoted above,
 * not in a fresh oracle session -- unlike vms_proc_may_read()'s READ rule,
 * this DELETE rule has not yet been measured against the reference lab; a
 * lab confirmation pass is follow-up work, not blocking (Rule 10 permits
 * citing the public Dictionary directly; nothing here is invented).
 */
uint32_t sys$delprc(const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam) {
    struct vms_procinfo target, self_info;
    uint32_t status;

    if (prcnam && prcnam->dsc$a_pointer && prcnam->dsc$w_length > 0) {
        char key[VMS_PRCNAM_XFER];
        dsc$strncpy(key, prcnam, sizeof(key));
        status = vms_kif_getjpi_prcnam(key, &target);
    } else if (pidadr && *pidadr != 0) {
        status = vms_kif_getjpi_pid(*pidadr, &target);
    } else {
        status = vms_kif_getjpi_self(&target);
    }
    if (!(status & 1))
        return status;

    status = vms_kif_getjpi_self(&self_info);
    if (!(status & 1))
        return status;

    if (target.vms_pid != self_info.vms_pid) {
        int same_group = ((target.uic >> 16) == (self_info.uic >> 16));
        /* WORLD is the BROADER privilege ("affect any process, any group")
         * and so authorizes a same-group delete too, same as it authorizes
         * everything GROUP does; GROUP alone does not reach outside the
         * caller's own group. A same-group target is therefore authorized
         * by EITHER bit, a cross-group one by WORLD alone -- caught live by
         * tests/qemu/test_syssvc_delprc.c P1/P2, whose caller holds WORLD
         * (the executive's default privileged-registration grant,
         * VMS_PRV_M_ENFORCED) but not GROUP (granted to nobody by default;
         * nothing enforced it before this item). Requiring GROUP outright
         * for the same-group case -- this function's first version --
         * refused that caller's own same-group STOP with SS$_NOPRIV. */
        uint64_t authorized = self_info.cur_privs &
            (same_group ? (PRV$M_GROUP | PRV$M_WORLD) : PRV$M_WORLD);
        if (!authorized)
            return SS$_NOPRIV;
    }

    /*
     * THE TERMINATION ITSELF REMAINS SIGTERM-BY-LINUX-PID (OVMX design
     * choice, Rule 8): OVMX has no executive-side "delete this PCB"
     * primitive distinct from the process actually dying, the same way
     * sys$exit() above is _exit() rather than a call into the executive.
     * What vms-1a8 changes is WHOSE Linux pid this is -- the row the
     * executive resolved and authorized above, not an unchecked caller
     * argument. A race where the target exits between resolution and this
     * kill() is reported the same way VMS reports "not there any more":
     * SS$_NONEXPR.
     */
    if (kill((pid_t)target.linux_pid, SIGTERM) < 0)
        return SS$_NONEXPR;
    return SS$_NORMAL;
}

/*
 * sys$dclexh - Declare exit handler.
 *
 * Registers an exit handler in the PCB that will be called by sys$exit
 * before the process terminates. Handlers are called in LIFO order.
 *
 * The desblk layout (VMS convention):
 *   desblk[0] = forward link (managed by system)
 *   desblk[1] = handler address
 *   desblk[2] = argument count
 *   desblk[3] = address of status longword
 */
uint32_t sys$dclexh(void *desblk) {
    if (!desblk) return SS$_BADPARAM;

    struct vms_pcb *pcb = vms_pcb_get();
    if (!pcb) return SS$_BADPARAM;

    if (pcb->exit_handler_count >= PCB_MAX_EXIT_HANDLERS)
        return SS$_EXQUOTA;

    pcb->exit_handlers[pcb->exit_handler_count++] =
        (struct pcb_exit_handler *)desblk;

    return SS$_NORMAL;
}

/*
 * sys$forcex - Force image exit on another process.
 *
 * Sends SIGUSR1 to the target process to force it to exit.
 * If both pidadr and prcnam are NULL, force exit on current process
 * (equivalent to calling sys$exit with the provided code).
 */
uint32_t sys$forcex(const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam,
                    uint32_t code) {
    (void)prcnam;

    /* If no target specified, force exit on current process */
    if (!pidadr && !prcnam) {
        return sys$exit(code);
    }

    pid_t pid;
    if (pidadr) {
        pid = (pid_t)*pidadr;
    } else {
        pid = getpid();
    }

    /* Send SIGUSR1 to force the target process to exit */
    if (kill(pid, SIGUSR1) < 0) return SS$_NONEXPR;
    return SS$_NORMAL;
}

/*
 * sys$suspnd - Suspend a process (canonical VMS name).
 *
 * Sends SIGSTOP to the target process. If pidadr is NULL,
 * suspend the current process.
 *
 * The VMS system service name is sys$suspnd (no trailing 'e').
 * sys$suspend is provided as a backwards-compatibility alias.
 */
uint32_t sys$suspnd(const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam) {
    (void)prcnam;

    pid_t pid;
    if (pidadr) {
        pid = (pid_t)*pidadr;
    } else {
        pid = getpid();
    }

    if (kill(pid, SIGSTOP) < 0) return SS$_NONEXPR;
    return SS$_NORMAL;
}

/*
 * sys$suspend - Backwards-compatibility alias for sys$suspnd.
 *
 * OVMX originally implemented the suspend service under this name.
 * Retained so that any code calling sys$suspend continues to work.
 */
uint32_t sys$suspend(const uint32_t *pidadr,
                     const struct dsc$descriptor_s *prcnam) {
    return sys$suspnd(pidadr, prcnam);
}

/*
 * sys$resume - Resume a suspended process.
 *
 * Sends SIGCONT to the target process to resume it from suspension.
 */
uint32_t sys$resume(const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam) {
    (void)prcnam;

    pid_t pid;
    if (pidadr) {
        pid = (pid_t)*pidadr;
    } else {
        pid = getpid();
    }

    if (kill(pid, SIGCONT) < 0) return SS$_NONEXPR;
    return SS$_NORMAL;
}

/*
 * sys$setpri - Set process priority.
 *
 * Maps VMS priority (0-31) to Linux nice values (-20 to 19).
 * VMS priority 31 (highest) = Linux nice -20 (highest)
 * VMS priority 0 (lowest) = Linux nice 19 (lowest)
 *
 * Currently only supports changing the priority of the current process.
 * If pidadr/prcnam specify another process, they are ignored and we
 * operate on the current process (this simplifies the implementation
 * while still satisfying most use cases).
 */
uint32_t sys$setpri(const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam,
                    uint32_t pri,
                    uint32_t *prvpri) {
    (void)pidadr;
    (void)prcnam;

    /* Clamp VMS priority to valid range (0-31) */
    if (pri > 31) pri = 31;

    /* Get current priority if caller wants it */
    if (prvpri) {
        errno = 0;
        int current_nice = getpriority(PRIO_PROCESS, 0);
        if (errno != 0) current_nice = 0;

        /* Convert Linux nice (-20 to 19) back to VMS priority (31 to 0) */
        *prvpri = (uint32_t)(19 - current_nice);
        if (*prvpri > 31) *prvpri = 31;
    }

    /* Map VMS priority to Linux nice value:
     * VMS 31 -> nice -20 (highest priority)
     * VMS 0  -> nice 19 (lowest priority)
     */
    int nice_value = 19 - (int)pri;

    /* Set priority using setpriority (operates on current process) */
    if (setpriority(PRIO_PROCESS, 0, nice_value) < 0) {
        return SS$_NOPRIV;  /* Usually fails due to lack of privilege */
    }

    return SS$_NORMAL;
}

/*
 * sys$cancel - Cancel pending I/O on a channel.
 *
 * This is a stub that always returns success because our current I/O
 * model is synchronous. All sys$qio operations complete immediately
 * before returning, so there are no pending operations to cancel.
 *
 * When asynchronous I/O is implemented (via io_uring or AIO), this
 * service will need to:
 *   1. Locate any pending I/O request for the specified channel
 *   2. Cancel the operation (io_uring_prep_cancel or aio_cancel)
 *   3. Complete the I/O with SS$_CANCEL status
 */
uint32_t sys$cancel(uint16_t chan) {
    (void)chan;

    /* No-op: synchronous I/O model has no pending operations */
    return SS$_NORMAL;
}
