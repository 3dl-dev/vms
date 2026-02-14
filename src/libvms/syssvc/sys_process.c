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
 * sys$getjpi - Get Job/Process Information.
 *
 * Returns information about a process via an item list. Supported items:
 *   JPI$_PID      - Process ID (getpid)
 *   JPI$_PRCNAM   - Process name (from PCB)
 *   JPI$_USERNAME - Username (from PCB or passwd database)
 *   JPI$_UIC      - User Identification Code (from PCB or [gid,uid])
 *   JPI$_CPUTIM   - CPU time in 10-millisecond units
 */
uint32_t sys$getjpi(uint32_t efn, const uint32_t *pidadr,
                    const struct dsc$descriptor_s *prcnam,
                    const struct item_list_3 *itmlst,
                    void *iosb,
                    void (*astadr)(uint32_t), uint32_t astprm) {
    (void)efn; (void)prcnam; (void)iosb; (void)astadr; (void)astprm;

    pid_t pid = pidadr ? (pid_t)*pidadr : getpid();

    if (!itmlst) return SS$_BADPARAM;

    struct vms_pcb *pcb = vms_pcb_get();

    for (const struct item_list_3 *item = itmlst;
         item->buflen != 0 || item->item_code != 0; item++) {
        switch (item->item_code) {
            case JPI$_PID:
                if (item->bufaddr && item->buflen >= sizeof(uint32_t))
                    *(uint32_t *)item->bufaddr = (uint32_t)pid;
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;

            case JPI$_PRCNAM:
                if (item->bufaddr) {
                    const char *name = "";
                    char default_name[16];

                    if (pcb && pcb->prcnam[0] != '\0') {
                        name = pcb->prcnam;
                    } else {
                        /* Generate a default process name from PID */
                        snprintf(default_name, sizeof(default_name),
                                 "_%08X", (uint32_t)pid);
                        name = default_name;
                    }

                    uint16_t len = (uint16_t)strlen(name);
                    if (len > item->buflen) len = item->buflen;
                    memcpy(item->bufaddr, name, len);
                    if (item->retlen) *item->retlen = len;
                }
                break;

            case JPI$_USERNAME: {
                const char *name = NULL;
                if (pcb && pcb->username[0] != '\0') {
                    name = pcb->username;
                } else {
                    struct passwd *pw = getpwuid(getuid());
                    name = pw ? pw->pw_name : "UNKNOWN";
                }
                uint16_t len = (uint16_t)strlen(name);
                if (len > item->buflen) len = item->buflen;
                if (item->bufaddr) memcpy(item->bufaddr, name, len);
                if (item->retlen) *item->retlen = len;
                break;
            }

            case JPI$_UIC:
                if (item->bufaddr && item->buflen >= sizeof(uint32_t)) {
                    if (pcb && pcb->uic != 0) {
                        *(uint32_t *)item->bufaddr = pcb->uic;
                    } else {
                        /* UIC: [group,member] packed as (gid << 16) | uid */
                        *(uint32_t *)item->bufaddr =
                            (uint32_t)((getgid() << 16) | getuid());
                    }
                }
                if (item->retlen) *item->retlen = sizeof(uint32_t);
                break;

            case JPI$_CPUTIM: {
                /* CPU time in 10ms units */
                struct rusage ru;
                getrusage(RUSAGE_SELF, &ru);
                uint32_t cputim = (uint32_t)(
                    (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 100 +
                    (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 10000);
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

    /* Process name */
    char child_prcnam[16] = {0};
    if (prcnam && prcnam->dsc$a_pointer)
        dsc$strncpy(child_prcnam, prcnam, sizeof(child_prcnam));

    pid_t pid = fork();
    if (pid < 0) return SS$_INSFMEM;

    if (pid == 0) {
        /* Child: Initialize PCB with inherited context */
        struct vms_pcb *child_pcb = vms_pcb_init(child_privs);
        if (child_pcb) {
            const char *username = parent_pcb ? parent_pcb->username : "UNKNOWN";
            if (!child_prcnam[0])
                snprintf(child_prcnam, sizeof(child_prcnam), "_%08X", getpid());
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
 * sys$suspend - Suspend a process.
 *
 * Sends SIGSTOP to the target process. If pidadr is NULL,
 * suspend the current process.
 */
uint32_t sys$suspend(const uint32_t *pidadr,
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
