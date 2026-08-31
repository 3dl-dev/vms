/*
 * ovmx_spawn_pipeline.c - multi-stage subprocess pipeline over the executive
 *                         (vms-e9a B2, docs/design-libspawn-ovmx.md §3c/§5)
 *
 * See ovmx_spawn_pipeline.h for the contract. This is the GCC-lane's authored
 * VMS-host process-creation hook: it chains image stages (cpp -> cc1 -> as ->
 * ld) by their executive completion status, using ONLY executive facilities --
 * $CREPRC to create each stage /NOWAIT (B0), VMS_IOCTL_SPAWN_NOTIFY to be told
 * when a stage records its exit (B1), and the $GETJPI/$STATUS surface to read
 * that stage's condition value -- with a real RMS scratch file, not a Unix
 * pipe, carrying each stage's output forward (§3c default transport).
 *
 * No userspace waitpid drives the pipeline: the driver blocks in $WAITFR on the
 * B1 completion event flag, exactly as a /NOWAIT LIB$SPAWN caller would. A
 * waitpid() DOES appear once per stage AFTER completion -- but only to reap the
 * subprocess's zombie (the driver is its Linux parent); it is post-completion
 * bookkeeping, never the signal the pipeline advances on. It must run only after
 * the stage's $STATUS has been read, because the executive row that carries that
 * $STATUS survives exactly as long as the zombie is un-reaped.
 */

#include "ovmx_spawn_pipeline.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/wait.h>

#include "ssdef.h"
#include "descrip.h"
#include "starlet.h"
#include "vms_kif.h"

/*
 * Completion event flag for the /NOWAIT stage notifications. A local flag in
 * cluster 1 (32..63), matching the B1 kernel-executive proof
 * (tests/qemu/test_kmod_spawn_notify.c) -- away from the low reserved flags and
 * the common cluster-0 flags a driven image might itself use. The SAME flag is
 * reused across stages: it is cleared before each $CREPRC so a prior stage's set
 * state can never satisfy the next stage's $WAITFR.
 */
#define OVMX_PIPE_EFN   41u

/* Build a CLASS_S text descriptor over a C string (NULL/empty -> NULL pointer,
 * so the caller can pass it straight to $CREPRC as "inherit / none"). */
static struct dsc$descriptor_s pipe_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = s ? (uint16_t)strlen(s) : 0;
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (s && *s) ? (char *)s : NULL;
    return d;
}

uint32_t ovmx_spawn_pipeline(const struct ovmx_pipe_stage *stages, int nstages,
                             const char *initial_input, const char *final_output,
                             const char *scratch_dir,
                             struct ovmx_pipe_result *result)
{
    struct ovmx_pipe_result res;
    memset(&res, 0, sizeof(res));
    res.failed_index  = -1;
    res.create_status = SS$_NORMAL;

    if (!stages || nstages <= 0) {
        res.create_status = SS$_BADPARAM;
        if (result) *result = res;
        return SS$_BADPARAM;
    }
    if (!scratch_dir || !*scratch_dir)
        scratch_dir = "/tmp";

    /* Intermediate scratch files: one per stage boundary, i.e. the SYS$OUTPUT of
     * stage i that becomes the SYS$INPUT of stage i+1, for i in 0..nstages-2. */
    char scratch[16][256];
    int  have_scratch[16] = { 0 };
    const int max_scratch = (int)(sizeof(scratch) / sizeof(scratch[0]));
    if (nstages - 1 > max_scratch) {
        res.create_status = SS$_BADPARAM;   /* more stages than the driver chains */
        if (result) *result = res;
        return SS$_BADPARAM;
    }

    uint32_t final_status = SS$_NORMAL;

    for (int i = 0; i < nstages; i++) {
        const char *in_path  = (i == 0) ? initial_input : scratch[i - 1];
        char *out_path;

        if (i == nstages - 1) {
            out_path = (char *)final_output;
        } else {
            snprintf(scratch[i], sizeof(scratch[i]),
                     "%s/ovmx_pipe_%d_%d.tmp", scratch_dir, (int)getpid(), i);
            have_scratch[i] = 1;
            out_path = scratch[i];
        }

        struct dsc$descriptor_s img_d = pipe_dsc(stages[i].image);
        struct dsc$descriptor_s in_d  = pipe_dsc(in_path);
        struct dsc$descriptor_s out_d = pipe_dsc(out_path);
        struct dsc$descriptor_s prc_d = pipe_dsc(stages[i].prcnam);

        /* Clear any prior stage's completion state before creating this one, so
         * this stage's $WAITFR below cannot be satisfied by a stale set flag. */
        (void)vms_kif_clref(OVMX_PIPE_EFN);

        /* B0: create the stage /NOWAIT over the executive-registered primitive.
         * $CREPRC returns the child's fresh executive VMS PID and does NOT wait
         * for it -- the child is running the image while we arm and wait. */
        uint32_t vms_pid = 0;
        uint32_t cst = sys$creprc(&vms_pid, &img_d,
                                  in_d.dsc$a_pointer  ? &in_d  : NULL,
                                  out_d.dsc$a_pointer ? &out_d : NULL,
                                  NULL,                    /* SYS$ERROR: inherit */
                                  NULL, NULL,              /* prv/quota: inherit */
                                  prc_d.dsc$a_pointer ? &prc_d : NULL,
                                  0, 0, 0, 0);             /* stsflg 0 -> SUBPROCESS */
        if (!(cst & 1)) {
            /* Could not create the stage -- honest $CREPRC status (SS$_NOSUCHDEV
             * with no executive, SS$_NOSUCHFILE for a missing image, ...). No
             * userspace fork/pipe fallback (INV-6): the pipeline stops. */
            res.create_status = cst;
            final_status = cst;
            break;
        }

        /* B1: arm the executive to set OVMX_PIPE_EFN when this stage records its
         * exit. astadr 0 -> event-flag-only completion (the driver waits on the
         * flag; it needs no AST). If the child already exited (raced us between
         * $CREPRC and here), the executive delivers immediately: completed==1,
         * the flag is already set, and the $WAITFR below returns at once. */
        int completed = 0;
        uint32_t ast = vms_kif_spawn_notify(vms_pid, OVMX_PIPE_EFN, 0, 0,
                                            &completed);
        if (!(ast & 1)) {
            res.create_status = ast;   /* completion could not be armed */
            final_status = ast;
            break;
        }

        /* Wait for completion via the event flag -- NOT waitpid. Blocks until the
         * stage image records its exit ($EXIT / VMS_IOCTL_SETEXIT). */
        (void)vms_kif_waitfr(OVMX_PIPE_EFN);

        /* Read the stage's actual completion $STATUS by its VMS PID, BEFORE the
         * subprocess is reaped (its executive row survives while its Linux task
         * is an un-reaped zombie). This is the signal the pipeline advances on. */
        uint32_t cond = 0;
        int has_exited = 0;
        uint32_t gst = vms_kif_getexit_pid(vms_pid, &cond, &has_exited);

        /* Resolve the backing Linux pid (row still alive), then reap the zombie.
         * Post-completion cleanup only -- the pipeline already knows the stage
         * finished from the event flag above. */
        struct vms_procinfo info;
        memset(&info, 0, sizeof(info));
        if ((vms_kif_getjpi_pid(vms_pid, &info) & 1) && info.linux_pid) {
            int wstatus;
            while (waitpid((pid_t)info.linux_pid, &wstatus, 0) < 0 && errno == EINTR)
                ;
        }

        res.stages_run++;

        /* Decide the stage outcome. A getexit that could not read a recorded
         * status (no executive row, refused, or nothing recorded) is treated as a
         * stage failure carrying that status -- never as a silent success. */
        uint32_t stage_status;
        if (!(gst & 1) || !has_exited)
            stage_status = (gst & 1) ? SS$_ABORT : gst;
        else
            stage_status = cond;

        if (!(stage_status & 1)) {
            /* Stage failed: STOP the pipeline, later stages are NOT launched. */
            res.failed_index  = i;
            res.failed_status = stage_status;
            final_status = stage_status;
            break;
        }
        /* Success: this stage's SYS$OUTPUT scratch file is stage i+1's input. */
    }

    /* Reclaim the intermediate scratch files (the caller owns final_output). */
    for (int i = 0; i < max_scratch; i++)
        if (have_scratch[i])
            unlink(scratch[i]);

    if (result) *result = res;
    return final_status;
}
