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
#include <pwd.h>
#include <stdio.h>
#include "starlet.h"
#include "vms/pcb.h"
#include "vms_kif.h"

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
 * Returns 0 (and leaves the item length set) when /proc cannot answer.
 */
static uint32_t jpi_cputim(uint32_t linux_pid)
{
    if (linux_pid == (uint32_t)getpid()) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        return (uint32_t)(
            (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 100 +
            (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 10000);
    }

    char path[64];
    snprintf(path, sizeof(path), "/proc/%u/stat", (unsigned)linux_pid);
    FILE *f = fopen(path, "r");
    if (!f) return 0;

    char buf[1024];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (n == 0) return 0;
    buf[n] = '\0';

    /* The comm field is parenthesised and may contain spaces, so field
     * splitting has to start after the LAST ')'. utime/stime are the
     * 12th and 13th fields after that point. */
    char *p = strrchr(buf, ')');
    if (!p) return 0;
    p++;

    /* strtok_r, never strtok: this runs inside sys$getjpi, a PUBLIC sys$
     * entry point, and strtok() keeps its cursor in a single static that
     * belongs to whoever called us. Clobbering a caller's in-flight
     * tokenisation from inside a system service is a defect on its own,
     * independent of anything the tests reach. */
    unsigned long long utime = 0, stime = 0;
    int field = 0;
    char *save = NULL;
    for (char *tok = strtok_r(p, " ", &save); tok;
         tok = strtok_r(NULL, " ", &save)) {
        field++;                        /* field 1 here == stat field 3 */
        if (field == 12) utime = strtoull(tok, NULL, 10);
        else if (field == 13) { stime = strtoull(tok, NULL, 10); break; }
    }

    long hz = sysconf(_SC_CLK_TCK);
    if (hz <= 0) hz = 100;
    return (uint32_t)(((utime + stime) * 100ULL) / (unsigned long long)hz);
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
 *   JPI$_USERNAME - see the item's own comment below
 *
 * Selection follows the VMS argument rules: prcnam names a process,
 * otherwise a non-zero *pidadr names one, otherwise the caller. There is
 * NO fallback to local state when the executive cannot resolve the
 * target -- the honest answer is SS$_NONEXPR, which is what VMS returns
 * for a process that does not exist.
 */
uint32_t sys$getjpi(uint32_t efn, const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam,
                    const struct item_list_3 *itmlst,
                    void *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm) {
    (void)efn; (void)iosb; (void)astadr; (void)astprm;

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

    int is_self = (info.linux_pid == (uint32_t)getpid());
    struct vms_pcb *pcb = is_self ? vms_pcb_get() : NULL;

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
                 * that no other process could see. OVMX does not yet
                 * assign a default name at process creation the way VMS
                 * does; until it does, an unnamed process reports no
                 * name rather than one only it believes in.
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
                 * The executive's row does not carry a username yet
                 * (vms-2b8 is adding identity to struct vms_proc), so
                 * for another process the name is resolved through the
                 * passwd database from the UIC MEMBER the executive
                 * reported -- executive-supplied, credential-derived
                 * data, not anything the target process declared.
                 *
                 * For the caller itself the existing PCB path is kept
                 * unchanged. It is a self-declared value (the
                 * VMS_USERNAME facade) and it is NOT this item's to
                 * remove -- vms-2b8 owns it, and both branches touch
                 * the same struct.
                 */
                const char *name = NULL;
                if (is_self && pcb && pcb->username[0] != '\0') {
                    name = pcb->username;
                } else {
                    struct passwd *pw = getpwuid((uid_t)(info.uic & 0xFFFFu));
                    name = pw ? pw->pw_name : "UNKNOWN";
                }
                uint16_t len = (uint16_t)strlen(name);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, name, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case JPI$_UIC:
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = info.uic;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;

            case JPI$_CPUTIM: {
                uint32_t cputim = jpi_cputim(info.linux_pid);
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
                     const struct dsc$descriptor_s *prcnam,
                     const struct item_list_3 *itmlst,
                     void *iosb,
                     void (*astadr)(uint32_t), uint32_t astprm) {
    return sys$getjpi(efn, pidadr, prcnam, itmlst, iosb, astadr, astprm);
}

/*
 * sys$hiber - Hibernate the current process.
 *
 * Suspends the calling process until it is awakened by sys$wake
 * or a signal. On VMS this is a clean suspension; here we use
 * pause() which blocks until a signal is delivered.
 */
uint32_t sys$hiber(void) {
    pause();
    return SS$_NORMAL;
}

/*
 * sys$wake - Wake a hibernating process.
 *
 * Sends SIGCONT to the target process to resume it from hibernation.
 * If pidadr is NULL, wakes the current process.
 */
uint32_t sys$wake(const uint32_t *pidadr,
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
 * sys$creprc - Create a subprocess.
 *
 * Uses fork()+exec() to create a new process running the specified image.
 * Inherits VMS context (privileges, UIC, username, quotas) from the
 * parent PCB when parameters are zero/NULL. I/O redirection is set up
 * from the input/output/error descriptors.
 * The new process PID is returned in *pidadr if non-NULL.
 */
uint32_t sys$creprc(uint32_t *pidadr, const struct dsc$descriptor_s *image,
                    const struct dsc$descriptor_s *input,
                    const struct dsc$descriptor_s *output,
                    const struct dsc$descriptor_s *error,
                    const void *prvadr, const void *quota,
                    const struct dsc$descriptor_s *prcnam,
                    uint32_t baspri, uint32_t uic, uint32_t mbxunt,
                    uint32_t stsflg) {
    (void)quota; (void)baspri; (void)mbxunt; (void)stsflg;

    if (!image || !image->dsc$a_pointer) return SS$_BADPARAM;

    char img_path[512];
    dsc$strncpy(img_path, image, sizeof(img_path));

    struct vms_pcb *parent_pcb = vms_pcb_get();

    /* Determine child privileges */
    uint64_t child_privs;
    if (prvadr)
        child_privs = *(const uint64_t *)prvadr;
    else if (parent_pcb)
        child_privs = parent_pcb->cur_privs;
    else
        child_privs = 0;

    /* Determine child UIC */
    uint32_t child_uic = uic;
    if (child_uic == 0 && parent_pcb)
        child_uic = parent_pcb->uic;

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
     * NAMING HANDSHAKE -- OVMX design choice, matching VMS's OBSERVABLE
     * semantics (CLAUDE.md Rule 8: labelled as OVMX's own mechanism).
     *
     * On VMS the executive names the process AS IT CREATES IT, so a
     * clash is reported to the CREATOR:
     *   Oracle (VAX1, OpenVMS VAX V7.3, recorded on this item): a third
     *   detached process taking a PROCESS_NAME already held in the same
     *   UIC group is refused with %RUN-F-CREPRC / -SYSTEM-F-DUPLNAM.
     *
     * OVMX creates the process with fork(), and only the child can enter
     * itself in the executive's table (the entry is keyed by ITS tgid).
     * So the child performs $SETPRN before exec and reports the status
     * back over a pipe; $CREPRC returns that status to its caller. The
     * pipe is O_CLOEXEC, so it costs the activated image nothing.
     *
     * Naming happens BEFORE the exec and before the I/O redirection: the
     * executive entry is keyed by the pid, which execve() does not
     * change, so the name survives image activation with no userspace
     * carrier of any kind. That is the whole point -- the rejected
     * VMS_PRCNAM environment-variable "fix" carried a name the image
     * could only tell itself.
     */
    int namefd[2] = { -1, -1 };
    if (child_prcnam[0]) {
        if (pipe(namefd) < 0)
            return SS$_INSFMEM;
        /* Not pipe2(O_CLOEXEC): that needs _GNU_SOURCE, and this file is
         * built against both glibc and musl. */
        fcntl(namefd[0], F_SETFD, FD_CLOEXEC);
        fcntl(namefd[1], F_SETFD, FD_CLOEXEC);
    }

    pid_t pid = fork();
    if (pid < 0) {
        if (namefd[0] >= 0) { close(namefd[0]); close(namefd[1]); }
        return SS$_INSFMEM;
    }

    if (pid == 0) {
        /* Child: enter the executive's process table under the requested
         * name before anything else can fail, and tell the creator how it
         * went. An unnamed process is expressed by never calling $SETPRN,
         * never by naming it something invented. */
        if (child_prcnam[0]) {
            close(namefd[0]);
            uint32_t nst = vms_kif_setprn(child_prcnam);
            ssize_t w = write(namefd[1], &nst, sizeof(nst));
            (void)w;
            close(namefd[1]);
            if (!(nst & 1))
                _exit(1);
        }

        /* Child: Initialize PCB with inherited context */
        struct vms_pcb *child_pcb = vms_pcb_init(child_privs);
        if (child_pcb) {
            const char *username = parent_pcb ? parent_pcb->username : "UNKNOWN";
            vms_pcb_set_identity((uint32_t)getpid(), child_uic,
                                 username, child_prcnam);
            if (parent_pcb && parent_pcb->default_dir[0])
                vms_pcb_set_default_dir(parent_pcb->default_dir);
            /* Inherit quotas from parent */
            if (parent_pcb)
                memcpy(child_pcb->quotas, parent_pcb->quotas,
                       sizeof(child_pcb->quotas));
        }

        /* Set up I/O redirection */
        if (input && input->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, input, sizeof(path));
            int fd = open(path, O_RDONLY);
            if (fd >= 0) { dup2(fd, STDIN_FILENO); close(fd); }
        }
        if (output && output->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, output, sizeof(path));
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, STDOUT_FILENO); close(fd); }
        }
        if (error && error->dsc$a_pointer) {
            char path[256];
            dsc$strncpy(path, error, sizeof(path));
            int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
            if (fd >= 0) { dup2(fd, STDERR_FILENO); close(fd); }
        }

        execl(img_path, img_path, (char *)NULL);
        _exit(1);  /* exec failed */
    }

    /* Parent process */
    if (child_prcnam[0]) {
        close(namefd[1]);
        uint32_t nst = 0;
        ssize_t r = read(namefd[0], &nst, sizeof(nst));
        close(namefd[0]);

        if (r == (ssize_t)sizeof(nst) && !(nst & 1)) {
            /* The executive refused the name (SS$_DUPLNAM within the UIC
             * group, or SS$_IVLOGNAM for a malformed one). The child has
             * already exited; reap it so the caller is not left with a
             * zombie for a process that was never named, and hand the
             * caller the executive's own status. */
            int wstatus;
            while (waitpid(pid, &wstatus, 0) < 0 && errno == EINTR)
                ;
            return nst;
        }

        /*
         * Anything else means the child never got as far as reporting --
         * it died between fork() and the write. The PROCESS was still
         * created, which is what $CREPRC's contract is about, and there
         * is no VMS status for "the creator could not hear the child",
         * so no status is invented for it (Rule 10).
         */
    }

    if (pidadr) *pidadr = (uint32_t)pid;
    return SS$_NORMAL;
}

/*
 * sys$delprc - Delete (terminate) a process.
 *
 * Sends SIGTERM to the target process. If pidadr is NULL and prcnam
 * is NULL, terminates the current process.
 */
uint32_t sys$delprc(const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam) {
    (void)prcnam;
    pid_t pid;
    if (pidadr) {
        pid = (pid_t)*pidadr;
    } else {
        pid = getpid();
    }
    if (kill(pid, SIGTERM) < 0) return SS$_NONEXPR;
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
