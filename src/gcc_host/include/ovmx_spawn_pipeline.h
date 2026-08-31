/*
 * ovmx_spawn_pipeline.h - multi-stage subprocess pipeline over the executive
 *                         (vms-e9a B2, docs/design-libspawn-ovmx.md §3c/§5)
 *
 * The reusable process-creation orchestration the GCC driver's VMS-host layer
 * needs: run a chain of image stages (the cpp -> cc1 -> as -> ld/collect2
 * pipeline), each stage's SYS$OUTPUT feeding the next stage's SYS$INPUT through
 * an RMS scratch file, with the DRIVER waiting on each stage's /NOWAIT
 * completion and checking its $STATUS before launching the next -- entirely over
 * the OVMX executive, never a Unix pipe(2)/waitpid(2) pipeline underneath.
 *
 * This is deliberately NOT in src/libvms or src/vmsdcl (design §5): it is the
 * GCC-lane's own authored VMS-host layer, a CONSUMER of the B0 creation
 * primitive ($CREPRC) and the B1 completion facility (VMS_IOCTL_SPAWN_NOTIFY).
 * The mechanism is generic -- any driver that must chain image stages by
 * completion status can call ovmx_spawn_pipeline(); the GCC driver is its first
 * consumer, and until the alpha-dec-vms port is buildable (vms-fd1, blocked) the
 * representative proof is tests/qemu/test_syssvc_spawn_pipeline.c.
 *
 * Per §3c the transport is the DEFAULT sequential temp-file handoff (VMS has no
 * anonymous-pipe primitive matching Unix pipe(2)); the concurrent mailbox-pipe
 * fan-out (-pipe behaviour) is B3, deferred.
 *
 * Rule 9 / INV-6: every step runs against a real /dev/vms. With no executive the
 * first $CREPRC fails honestly (SS$_NOSUCHDEV surfaced) and the pipeline stops
 * -- there is no userspace fork/pipe fallback that would report success.
 */

#ifndef OVMX_SPAWN_PIPELINE_H
#define OVMX_SPAWN_PIPELINE_H

#include <stdint.h>

/*
 * One pipeline stage: the image to activate, plus its executive process name.
 * SYS$INPUT / SYS$OUTPUT are wired by the driver (the previous stage's scratch
 * file in, the next stage's scratch file out), so they are NOT part of the
 * stage description -- a stage does not choose its own place in the chain.
 */
struct ovmx_pipe_stage {
    const char *image;    /* image filespec $CREPRC activates (e.g. cpp/cc1/as/ld) */
    const char *prcnam;   /* subprocess name applied via $CREPRC prcnam (or NULL) */
};

/*
 * Result of a pipeline run. `stages_run` is how many stages actually completed
 * (== nstages on full success). `failed_index` is the 0-based index of the
 * stage whose $STATUS was a failure (even) condition value, or -1 on full
 * success; `failed_status` carries that stage's actual $STATUS condition value.
 * `create_status` carries the $CREPRC status when a stage could not even be
 * created (e.g. SS$_NOSUCHDEV with no executive, SS$_NOSUCHFILE for a missing
 * image), SS$_NORMAL otherwise.
 */
struct ovmx_pipe_result {
    int      stages_run;
    int      failed_index;
    uint32_t failed_status;
    uint32_t create_status;
};

/*
 * Run stages[0..nstages-1] as a sequential subprocess pipeline over the
 * executive.
 *
 *   initial_input  - Linux/RMS path fed to stage 0 as its SYS$INPUT (NULL ->
 *                    stage 0 inherits the caller's SYS$INPUT).
 *   final_output   - path stage (nstages-1) writes as its SYS$OUTPUT. The
 *                    intermediate stage handoffs use scratch files the driver
 *                    creates and (on success) deletes.
 *   scratch_dir    - directory for the intermediate scratch files (NULL ->
 *                    "/tmp"). Each is created, fed forward, and unlinked.
 *   result         - filled with the outcome (may be NULL).
 *
 * For each stage the driver: $CREPRC's it /NOWAIT with SYS$INPUT/SYS$OUTPUT
 * redirected; arms VMS_IOCTL_SPAWN_NOTIFY (B1) on the child's VMS PID with a
 * completion event flag; $WAITFR's that flag until the child records its exit;
 * reads the child's $STATUS by VMS PID (vms_kif_getexit_pid); and only if that
 * $STATUS is a success (odd) launches the next stage on this stage's output.
 *
 * Returns SS$_NORMAL iff every stage was created AND completed with a success
 * $STATUS. Returns the failing stage's $STATUS if a stage completed with a
 * failure condition (the pipeline stops there, later stages are not launched),
 * or the $CREPRC error if a stage could not be created.
 */
uint32_t ovmx_spawn_pipeline(const struct ovmx_pipe_stage *stages, int nstages,
                             const char *initial_input, const char *final_output,
                             const char *scratch_dir,
                             struct ovmx_pipe_result *result);

#endif /* OVMX_SPAWN_PIPELINE_H */
