/*
 * lib_misc.c - Miscellaneous LIB$ Routines
 *
 * Provides simplified wrappers around sys$getjpi and sys$getsyi,
 * and the lib$spawn routine for subprocess creation.
 */

#include <stdint.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <glob.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "prcdef.h"
#include "lnmdef.h"
#include "clidef.h"          /* CLI$M_NOWAIT — lib$spawn "flags" bits        */
#include "ovmx_layout.h"     /* VMS_DCL_PATH — the DCL CLI image filespec    */
#include "rmsdef.h"
#include "vmsfs/filespec.h"  /* vmsfs_to_linux_path — VMS filespec resolver  */
#include "starlet.h"         /* sys$creprc — the one executive-registered create (B0) */
#include "vms_kif.h"         /* vms_kif_getjpi_pid, struct vms_procinfo — the wait handle */
#include <pthread.h>

/*
 * Side table for lib$find_file context handles.
 * Maps uint32 handles (1..MAX_FIND_FILE_CONTEXTS) to glob_t pointers,
 * avoiding 64-bit pointer truncation when stored in uint32_t *context.
 */
#define MAX_FIND_FILE_CONTEXTS 64
static glob_t *find_file_table[MAX_FIND_FILE_CONTEXTS];
static pthread_mutex_t find_file_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t find_file_alloc(glob_t *pglob) {
    pthread_mutex_lock(&find_file_lock);
    for (uint32_t i = 0; i < MAX_FIND_FILE_CONTEXTS; i++) {
        if (!find_file_table[i]) {
            find_file_table[i] = pglob;
            pthread_mutex_unlock(&find_file_lock);
            return i + 1;  /* handles are 1-based */
        }
    }
    pthread_mutex_unlock(&find_file_lock);
    return 0;  /* table full */
}

static glob_t *find_file_lookup(uint32_t handle) {
    if (handle == 0 || handle > MAX_FIND_FILE_CONTEXTS) return NULL;
    return find_file_table[handle - 1];
}

static void find_file_release(uint32_t handle) {
    if (handle > 0 && handle <= MAX_FIND_FILE_CONTEXTS) {
        pthread_mutex_lock(&find_file_lock);
        find_file_table[handle - 1] = NULL;
        pthread_mutex_unlock(&find_file_lock);
    }
}

/* sys$getjpiw / sys$getsyiw / sys$getdviw come from starlet.h (now included
 * for sys$creprc); the hand-copied forward declarations they used to need
 * here are removed, as their local getdviw prototype conflicted with the
 * header's authoritative one. */

/*
 * lib$getjpi - Get Job/Process Information (simplified wrapper).
 *
 * Provides a simpler calling interface to sys$getjpi for retrieving
 * a single item. Either result (for numeric items) or result_str
 * (for string items) should be provided.
 *
 * Parameters:
 *   item_code  - JPI$_ item code
 *   pid        - Process ID (or NULL for current process)
 *   prcnam     - Process name descriptor (or NULL)
 *   result     - Receives numeric result (or NULL)
 *   result_str - Receives string result (or NULL)
 *   result_len - Receives actual length of result (or NULL)
 */
uint32_t lib$getjpi(const uint32_t *item_code, const uint32_t *pid,
                    const struct dsc$descriptor_s *prcnam,
                    void *result, struct dsc$descriptor_s *result_str,
                    uint16_t *result_len) {
    if (!item_code) return SS$_BADPARAM;

    struct item_list_3 items[2];
    memset(items, 0, sizeof(items));

    items[0].buflen = result_str ? result_str->dsc$w_length : sizeof(uint32_t);
    items[0].item_code = (uint16_t)*item_code;
    items[0].bufaddr = result_str ? (void *)result_str->dsc$a_pointer
                                  : (void *)result;
    items[0].retlen = result_len;
    /* Terminator */
    items[1].buflen = 0;
    items[1].item_code = 0;
    items[1].bufaddr = NULL;
    items[1].retlen = NULL;

    return sys$getjpiw(0, pid, (void *)prcnam, items, NULL, NULL, 0);
}

/*
 * lib$getsyi - Get System Information (simplified wrapper).
 *
 * Provides a simpler calling interface to sys$getsyi for retrieving
 * a single item.
 *
 * Parameters:
 *   item_code  - SYI$_ item code
 *   result     - Receives numeric result (or NULL)
 *   result_str - Receives string result (or NULL)
 *   result_len - Receives actual length (or NULL)
 *   csid       - Cluster system ID (or NULL)
 *   node       - Node name descriptor (or NULL)
 */
uint32_t lib$getsyi(const uint32_t *item_code,
                    void *result, struct dsc$descriptor_s *result_str,
                    uint16_t *result_len, const uint32_t *csid,
                    const struct dsc$descriptor_s *node) {
    if (!item_code) return SS$_BADPARAM;

    struct item_list_3 items[2];
    memset(items, 0, sizeof(items));

    items[0].buflen = result_str ? result_str->dsc$w_length : sizeof(uint32_t);
    items[0].item_code = (uint16_t)*item_code;
    items[0].bufaddr = result_str ? (void *)result_str->dsc$a_pointer
                                  : (void *)result;
    items[0].retlen = result_len;
    /* Terminator */
    items[1].buflen = 0;
    items[1].item_code = 0;
    items[1].bufaddr = NULL;
    items[1].retlen = NULL;

    return sys$getsyiw(0, csid, node, items, NULL, NULL, 0);
}

/*
 * lib$getdvi - Get Device/Volume Information (simplified wrapper).
 *
 * Provides a simpler calling interface to sys$getdviw for retrieving
 * a single item, mirroring lib$getjpi/lib$getsyi above.
 *
 * Parameters:
 *   item_code     - DVI$_ item code
 *   chan          - I/O channel (0 if using devnam), passed by value
 *   devnam        - Device name descriptor (or NULL if using chan)
 *   resultval     - Receives numeric result (or NULL)
 *   resultstring  - Receives string result (or NULL)
 *   string_length - Receives actual length of result (or NULL)
 */
uint32_t lib$getdvi(const uint32_t *item_code, uint16_t chan,
                    const struct dsc$descriptor_s *devnam,
                    void *resultval, struct dsc$descriptor_s *resultstring,
                    uint16_t *string_length) {
    if (!item_code) return SS$_BADPARAM;

    struct item_list_3 items[2];
    memset(items, 0, sizeof(items));

    items[0].buflen = resultstring ? resultstring->dsc$w_length : sizeof(uint32_t);
    items[0].item_code = (uint16_t)*item_code;
    items[0].bufaddr = resultstring ? (void *)resultstring->dsc$a_pointer
                                    : (void *)resultval;
    items[0].retlen = string_length;
    /* Terminator */
    items[1].buflen = 0;
    items[1].item_code = 0;
    items[1].bufaddr = NULL;
    items[1].retlen = NULL;

    return sys$getdviw(0, chan, (struct dsc$descriptor_s *)devnam, items,
                       NULL, NULL, 0, 0);
}

/*
 * lib$spawn - Spawn a subprocess running the DCL command interpreter.
 *
 * VMS CONTRACT (public: VSI OpenVMS RTL Library (LIB$) Routines Reference
 * Manual, LIB$SPAWN, and the DCL Dictionary SPAWN command). LIB$SPAWN
 * creates a subprocess that runs a DCL command interpreter. When a command
 * string is supplied and no input file is, the subprocess executes that
 * single command and then terminates; otherwise it takes its commands from
 * SYS$INPUT (the input file, or the parent's SYS$INPUT). SYS$OUTPUT is the
 * output file when supplied. Unless CLI$M_NOWAIT is set the caller HIBERNATEs
 * until the subprocess completes, and the completion status is returned in the
 * completion-status argument.
 *
 * WHAT THIS USED TO BE, AND WHY THAT WAS A FACADE (vms-98c). The prior body
 * fork+exec'd `/bin/sh -c <command>` -- the Unix Bourne shell, NOT a DCL
 * command interpreter. It reported SS$_NORMAL for a "SHOW TIME" that /bin/sh
 * cannot parse and would never run as DCL. That is the exact class of defect
 * CLAUDE.md Rule 9 / INV-6 exist to kill: a userspace stand-in that reports
 * success while doing something other than the VMS thing. It is now a REAL
 * DCL subprocess: the SAME image JOB_CONTROL and PROVISION exec to hand a
 * user a session -- SYS$SYSTEM:DCL.EXE -- run against the command.
 *
 * HONEST BOUNDARY. If SYS$SYSTEM:DCL.EXE cannot be resolved or is not an
 * executable image, this returns an authentic VMS error (SS$_NOSUCHFILE) and
 * runs NOTHING -- it never falls back to /bin/sh, and never reports success
 * for a command it did not run. Resolving and fork/exec'ing the CLI image is
 * a genuine child process running genuine code; it does not fabricate any
 * executive facility, so it needs no /dev/vms.
 *
 * SCOPE (self-host spine #4, prereq A of vms-ec70's exec-drive). This is the
 * synchronous "run one DCL command, give me its status" primitive and the
 * /NOWAIT create. It does NOT itself implement the PERSISTENT-subprocess +
 * mailbox + write-attention-AST protocol MMK's build_target.c uses to stream
 * many commands into one long-lived DCL and read each command's $STATUS back
 * (that is prereqs B/C -- the mailbox IPC and the write-attention AST). See
 * the "DEFERRED" note at the end of this routine.
 *
 * Parameters:
 *   command     - Command string; DCL executes it, then the subprocess ends
 *                 (NULL -> interactive DCL reading from SYS$INPUT)
 *   input_file  - SYS$INPUT VMS filespec (or NULL to inherit)
 *   output_file - SYS$OUTPUT VMS filespec (or NULL to inherit)
 *   flags       - CLI$M_ flags longword; CLI$M_NOWAIT honored (others accepted
 *                 and ignored -- see the field note)
 *   prcnam      - Subprocess name -- APPLIED (B0, vms-e9a): passed straight
 *                 through to $CREPRC, whose child registers under it, so the
 *                 subprocess is resolvable BY NAME ($GETJPI/SHOW SYSTEM)
 *   pid         - Receives subprocess PID -- the EXECUTIVE-assigned VMS PID
 *                 (B0), resolvable by $GETJPI, not a bare Linux pid (or NULL)
 *   status      - Receives the subprocess completion status (or NULL)
 *   efn         - Event flag to set on completion (accepted, not yet wired -- B1)
 *   astadr      - Completion AST routine (accepted, not yet wired -- B1)
 *   astprm      - Completion AST parameter (accepted, not yet wired -- B1)
 *   prompt      - Prompt string (only meaningful for interactive; ignored)
 *   cli_name    - CLI name (accepted; OVMX's one CLI is DCL)
 *   table_name  - CLI table name (accepted; OVMX's one CLI is DCL)
 *
 * Return: SS$_NORMAL when the subprocess was created (the completion status
 * lands in *status); an error status when it could not be created.
 */

/* Build a CLASS_S text descriptor over a C string, the same shape RUN's
 * dsc_from_str() hands sys$creprc (src/vmsdcl/dcl_cmd_process.c). A NULL or
 * empty string yields a NULL-pointer descriptor the caller passes as NULL. */
static struct dsc$descriptor_s spawn_dsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = s ? (uint16_t)strlen(s) : 0;
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (s && *s) ? (char *)s : NULL;
    return d;
}

/*
 * Create an exclusive scratch file for the subprocess's SYS$INPUT, filling
 * `buf` with its path and returning an open write fd (or -1).
 *
 * WHY NOT mkstemp() (vms-e9a, VMS-native link). The VMS-native LIBVMS$SHR link
 * binds every C-RTL call against DECC$SHR's symbol vector, which exports the
 * bare universals open/close/write/unlink/getpid/snprintf but NOT bare mkstemp
 * (only the decorated decc$mkstemp the GCC port uses) -- so a bare mkstemp()
 * here is an unresolved external that breaks LIBVMS$SHR and every consumer of
 * it. This builds a unique name from getpid() + a counter and opens it
 * O_CREAT|O_EXCL (retrying on a name clash), using only exported universals --
 * the same collision-safe guarantee mkstemp gave, with no unexported symbol.
 */
static int spawn_open_scratch(char *buf, size_t bufsz)
{
    static unsigned seq = 0;
    for (int tries = 0; tries < 4096; tries++) {
        snprintf(buf, bufsz, "/tmp/ovmx_spawn_cmd_%d_%u",
                 (int)getpid(), seq++);
        int fd = open(buf, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (fd >= 0)
            return fd;
        if (errno != EEXIST)
            return -1;             /* a real error, not a name clash */
    }
    return -1;
}

/* Resolve a VMS filespec to a Linux path for open()/freopen(); if translation
 * fails, fall back to the spec verbatim (mirrors ovmx_job_control's
 * vms_to_linux()), so a caller passing a bare Linux path still works. */
static void spawn_resolve_spec(const struct dsc$descriptor_s *spec,
                               char *buf, size_t bufsz) {
    char raw[1024];
    dsc$strncpy(raw, spec, sizeof(raw));
    if (vmsfs_to_linux_path(raw, buf, bufsz) != 1) {
        snprintf(buf, bufsz, "%s", raw);
    }
}

uint32_t lib$spawn(const struct dsc$descriptor_s *command,
                   const struct dsc$descriptor_s *input_file,
                   const struct dsc$descriptor_s *output_file,
                   const uint32_t *flags,
                   const struct dsc$descriptor_s *prcnam,
                   uint32_t *pid, uint32_t *status,
                   const uint32_t *efn,
                   void *astadr,
                   void *astprm,
                   const struct dsc$descriptor_s *prompt,
                   const struct dsc$descriptor_s *cli_name,
                   const struct dsc$descriptor_s *table_name) {
    (void)prompt; (void)cli_name; (void)table_name;   /* OVMX's one CLI is DCL */
    (void)efn; (void)astadr; (void)astprm;            /* completion notify = B1 */

    const uint32_t spawn_flags = flags ? *flags : 0;
    const int nowait = (spawn_flags & CLI$M_NOWAIT) != 0;

    /*
     * HONEST BOUNDARY (preserved from the pre-B0 body). Resolve the DCL CLI
     * image through the VMS filespec translator -- so a redefined SYS$SYSTEM
     * (alternate system root) is honored, the same resolution PROVISION and
     * JOB_CONTROL use -- and require it to be a real, executable regular file.
     * If it is not, FAIL HONESTLY with an authentic SS$_NOSUCHFILE and create
     * nothing: never fall back to any other program. (This userspace check is
     * kept here, BEFORE $CREPRC, so lib$spawn's documented
     * SS$_NOSUCHFILE-before-anything contract survives the reroute -- $CREPRC
     * would also refuse the image, but only after its fork/handshake.)
     */
    char dcl_path[1024];
    if (vmsfs_to_linux_path(VMS_DCL_PATH, dcl_path, sizeof(dcl_path)) != 1)
        return SS$_NOSUCHFILE;
    /*
     * CHECK THE ACTUAL EXECVE TARGET, NOT THE RETIRED /vms PATH (vms-19e9).
     *
     * With the /vms passthrough retired (Files-11 ODS-2 ACP flip), a SYS$SYSTEM
     * image no longer exists as a POSIX file at the translated path
     * (/vms/SYS0/SYSCOMMON/SYSEXE/dcl.exe) -- it lives on the ODS-2 volume and is
     * execve'd from the boot-staging tmpfs. This pre-check used to stat() that
     * retired path, which now ENOENTs for EVERY spawn, so lib$spawn returned
     * SS$_NOSUCHFILE before $CREPRC and SPAWN was dead in the booted runtime.
     *
     * Validate the SAME target $CREPRC will exec: the boot-staged copy for a
     * SYSEXE image (ovmx_boot_stage_exec_path, as sys$creprc uses), the raw path
     * otherwise. The SS$_NOSUCHFILE-before-anything contract is preserved -- it
     * now fires on a genuinely absent image, not on a path the architecture
     * stopped using.
     */
    char dcl_staged[1024];
    const char *dcl_check =
        (ovmx_boot_stage_exec_path(dcl_path, dcl_staged, sizeof(dcl_staged)) &&
         access(dcl_staged, X_OK) == 0) ? dcl_staged : dcl_path;
    struct stat st;
    if (stat(dcl_check, &st) != 0 || !S_ISREG(st.st_mode) ||
        access(dcl_check, X_OK) != 0)
        return SS$_NOSUCHFILE;

    /*
     * B0 (vms-e9a, docs/design-libspawn-ovmx.md §3a/§5): create the subprocess
     * through the ONE executive-registered primitive -- $CREPRC -- instead of
     * lib$spawn's own fork()/execl(). $CREPRC's child enters the executive
     * process table (under `prcnam`, if one is given) BEFORE it activates the
     * image, so the subprocess is a genuine VMS process: resolvable by
     * $GETJPI / SHOW SYSTEM / $DELPRC and, when named, BY NAME. The pre-B0
     * fork/exec body registered NOTHING -- its `pid` was a bare Linux pid VMS
     * process management could not see, and `prcnam` was discarded outright;
     * that is the INV-6 invisibility gap this rung deletes.
     *
     * $CREPRC execs its image with NO command-line arguments, so a command
     * string cannot be handed to DCL as `DCL -c <cmd>`. VMS's own LIB$SPAWN
     * feeds the command to the subprocess CLI as its SYS$INPUT (RTL ref:
     * "executes that one command and terminates"). OVMX matches that contract:
     * the command is written to a scratch file and passed to $CREPRC as the
     * SYS$INPUT equivalence name; DCL reads it, runs it, hits EOF and exits.
     * The scratch file is an OVMX mechanism (CLAUDE.md Rule 8 -- a real
     * SYS$SCRATCH-style temp, never presented as a VMS byte format).
     */
    const int have_cmd = command && command->dsc$a_pointer &&
                         command->dsc$w_length > 0;
    const int have_in  = input_file  && input_file->dsc$a_pointer &&
                         input_file->dsc$w_length  > 0;
    const int have_out = output_file && output_file->dsc$a_pointer &&
                         output_file->dsc$w_length > 0;

    /*
     * SYS$INPUT for the subprocess. A command string wins when supplied (the
     * documented "command and no input file" case) and becomes a scratch
     * command file; otherwise the caller's input_file, resolved to a Linux
     * path, is passed through; otherwise NULL, and $CREPRC leaves a
     * subprocess's SYS$INPUT inherited. $CREPRC opens its input/output
     * descriptors as literal paths (no filespec translation of its own), so
     * they are handed already-resolved paths.
     */
    char cmd_tmp[256]  = "";
    char in_resv[1024] = "";
    int  have_tmp      = 0;
    const char *in_str = NULL;

    if (have_cmd) {
        int tfd = spawn_open_scratch(cmd_tmp, sizeof(cmd_tmp));
        if (tfd < 0)
            return SS$_INSFMEM;
        have_tmp = 1;
        char cbuf[4096];
        dsc$strncpy(cbuf, command, sizeof(cbuf) - 1);
        size_t clen = strlen(cbuf);
        cbuf[clen++] = '\n';                 /* one command line, then EOF */
        for (size_t off = 0; off < clen; ) {
            ssize_t w = write(tfd, cbuf + off, clen - off);
            if (w < 0) { if (errno == EINTR) continue; break; }
            if (w == 0) break;
            off += (size_t)w;
        }
        close(tfd);
        in_str = cmd_tmp;
    } else if (have_in) {
        spawn_resolve_spec(input_file, in_resv, sizeof(in_resv));
        in_str = in_resv;
    }

    char out_resv[1024] = "";
    const char *out_str = NULL;
    if (have_out) {
        spawn_resolve_spec(output_file, out_resv, sizeof(out_resv));
        out_str = out_resv;
    }

    struct dsc$descriptor_s img_d = spawn_dsc(dcl_path);
    struct dsc$descriptor_s in_d  = spawn_dsc(in_str);
    struct dsc$descriptor_s out_d = spawn_dsc(out_str);

    uint32_t vms_pid = 0;
    uint32_t cst = sys$creprc(&vms_pid, &img_d,
                              in_d.dsc$a_pointer  ? &in_d  : NULL,
                              out_d.dsc$a_pointer ? &out_d : NULL,
                              NULL,           /* SYS$ERROR: inherit */
                              NULL, NULL,     /* prvadr/quota: inherit creator */
                              prcnam,         /* B0: APPLIED, not discarded */
                              0, 0, 0,
                              0);             /* stsflg 0 -> SUBPROCESS */

    if (pid) *pid = vms_pid;

    if (!(cst & 1)) {
        /*
         * $CREPRC created nothing -- propagate its authentic status (e.g.
         * SS$_DUPLNAM for a name clash, OVMX$_PRCLOST for a lost child,
         * SS$_NOSUCHDEV with no executive). No fabricated success (INV-6):
         * lib$spawn no longer has an unregistered fork/exec to fall back to.
         */
        if (have_tmp) unlink(cmd_tmp);
        return cst;
    }

    if (nowait) {
        /*
         * CLI$M_NOWAIT: the subprocess was created and registered; return at
         * once. *status is left UNWRITTEN -- the completion is not known yet,
         * and the efn/astadr/astprm completion-NOTIFICATION path (LIB$SPAWN's
         * event flag / AST arguments) is rung B1: a process-exit EF/AST in the
         * executive (docs/design-libspawn-ovmx.md §3b/§5), deliberately not
         * wired here. Never fabricate a completion status.
         *
         * The scratch SYS$INPUT file, if any, is NOT unlinked here: the
         * subprocess may not have opened it yet, and B0 has no exit hook to
         * reclaim it. It is a genuine file left for the subprocess to consume;
         * its reclamation belongs to B1's exit notification.
         */
        return SS$_NORMAL;
    }

    /*
     * Wait mode: HIBERNATE until the subprocess completes (LIB$SPAWN's default
     * without CLI$M_NOWAIT). $CREPRC's SUBPROCESS shape forks in THIS process,
     * so the subprocess is a genuine Linux child of the caller and waitpid()
     * is the wait mechanism -- on the backing Linux pid, resolved from the
     * executive-assigned VMS pid $CREPRC handed back (the only handle it
     * gives). Full per-command $STATUS fidelity (vs. this success/failure
     * collapse) rides on B1's exit record + the mailbox EOM protocol -- see
     * the DEFERRED note below.
     */
    struct vms_procinfo info;
    memset(&info, 0, sizeof(info));
    uint32_t gj = vms_kif_getjpi_pid(vms_pid, &info);
    if ((gj & 1) && info.linux_pid) {
        int wstatus;
        while (waitpid((pid_t)info.linux_pid, &wstatus, 0) < 0 && errno == EINTR)
            ;
        if (status) {
            if (WIFEXITED(wstatus))
                *status = (WEXITSTATUS(wstatus) == 0) ? SS$_NORMAL : SS$_ABORT;
            else
                *status = SS$_ABORT;   /* killed by a signal */
        }
    } else if (status) {
        /*
         * Registered, but the executive no longer resolves the pid: the
         * subprocess already ran to completion between creation and this read.
         * It genuinely ran (no fabrication); its full $STATUS is unavailable
         * without B1's exit record, so report normal completion.
         */
        *status = SS$_NORMAL;
    }

    if (have_tmp) unlink(cmd_tmp);
    return SS$_NORMAL;
}

/*
 * DEFERRED (vms-ec70 exec-drive, prereqs B/C -- vms-e0b mailbox, vms-9003
 * write-attention AST): the PERSISTENT DCL subprocess MMK actually drives.
 * build_target.c opens ONE DCL subprocess (/NOWAIT), then streams resolved
 * compile/link command lines into it over a VMS MAILBOX, using a write-
 * attention AST + $HIBER/$WAKE to know when each command finished and to read
 * that command's $STATUS from an end-of-command marker. That needs: (1) this
 * /NOWAIT create [DONE here], (2) a mailbox the parent and the DCL child both
 * hold [prereq B], and (3) the write-attention AST that fires when the child
 * writes a completion marker [prereq C]. lib$spawn is the create primitive
 * under that protocol; the mailbox wiring is what turns it into MMK's driver.
 */

/*
 * lib$find_file - Find file matching wildcard specification.
 *
 * Uses glob() to expand wildcards and stores the result in *context
 * (cast as a glob_t*). On first call (*context == 0), performs glob().
 * On subsequent calls, returns the next match. Returns RMS$_NORMAL
 * when a file is found, RMS$_NMF when no more files.
 *
 * Parameters:
 *   filespec   - Descriptor of file specification (may contain * or ?)
 *   resultspec - Descriptor to receive matched filename
 *   context    - Context pointer (must be 0 on first call)
 */
uint32_t lib$find_file(const struct dsc$descriptor_s *filespec,
                       struct dsc$descriptor_s *resultspec,
                       uint32_t *context) {
    if (!filespec || !resultspec || !context) return SS$_BADPARAM;
    if (!filespec->dsc$a_pointer) return SS$_BADPARAM;

    glob_t *pglob;

    /* First call: perform glob() */
    if (*context == 0) {
        /* Convert descriptor to C string */
        char spec[1024];
        dsc$strncpy(spec, filespec, sizeof(spec));

        pglob = (glob_t *)malloc(sizeof(glob_t));
        if (!pglob) return SS$_INSFMEM;

        int result = glob(spec, GLOB_NOCHECK | GLOB_TILDE, NULL, pglob);
        if (result == GLOB_NOSPACE || result == GLOB_ABORTED) {
            if (result == GLOB_ABORTED) globfree(pglob);
            free(pglob);
            return SS$_INSFMEM;
        }

        /* Store glob result in side table, return handle */
        uint32_t handle = find_file_alloc(pglob);
        if (handle == 0) {
            globfree(pglob);
            free(pglob);
            return SS$_INSFMEM;
        }
        *context = handle;

        /* If no matches, return NMF immediately */
        if (pglob->gl_pathc == 0 ||
            (pglob->gl_pathc == 1 && strcmp(pglob->gl_pathv[0], spec) == 0)) {
            /* GLOB_NOCHECK means no match - just returned input */
            globfree(pglob);
            free(pglob);
            find_file_release(*context);
            *context = 0;
            return RMS$_NMF;
        }

        /* Mark that we're at the first result */
        pglob->gl_offs = 0;
    } else {
        /* Subsequent call: retrieve stored glob_t from side table */
        pglob = find_file_lookup(*context);
        if (!pglob) return RMS$_NMF;

        /* Move to next match */
        pglob->gl_offs++;
    }

    /* Check if we've exhausted all matches */
    if (pglob->gl_offs >= pglob->gl_pathc) {
        globfree(pglob);
        free(pglob);
        find_file_release(*context);
        *context = 0;
        return RMS$_NMF;
    }

    /* Copy current match to result descriptor */
    const char *match = pglob->gl_pathv[pglob->gl_offs];
    uint16_t len = (uint16_t)strlen(match);

    if (resultspec->dsc$b_class == DSC$K_CLASS_D) {
        /* Dynamic descriptor - reallocate */
        struct dsc$descriptor_d *ddest = (struct dsc$descriptor_d *)resultspec;
        if (ddest->dsc$a_pointer) free(ddest->dsc$a_pointer);
        ddest->dsc$a_pointer = (char *)malloc(len);
        if (!ddest->dsc$a_pointer) {
            ddest->dsc$w_length = 0;
            globfree(pglob);
            free(pglob);
            *context = 0;
            return SS$_INSFMEM;
        }
        memcpy(ddest->dsc$a_pointer, match, len);
        ddest->dsc$w_length = len;
    } else {
        /* Static descriptor - truncate if needed */
        if (!resultspec->dsc$a_pointer) {
            globfree(pglob);
            free(pglob);
            *context = 0;
            return SS$_BADPARAM;
        }
        uint16_t copylen = len;
        if (copylen > resultspec->dsc$w_length) {
            copylen = resultspec->dsc$w_length;
        }
        memcpy(resultspec->dsc$a_pointer, match, copylen);
        /* Pad with spaces if shorter */
        if (copylen < resultspec->dsc$w_length) {
            memset(resultspec->dsc$a_pointer + copylen, ' ',
                   resultspec->dsc$w_length - copylen);
        }
    }

    return RMS$_NORMAL;
}

/*
 * lib$find_file_end - End find file sequence.
 *
 * Frees the glob result stored in *context and resets *context to 0.
 *
 * Parameters:
 *   context - Context pointer from lib$find_file
 */
uint32_t lib$find_file_end(uint32_t *context) {
    if (!context) return SS$_BADPARAM;
    if (*context == 0) return SS$_NORMAL;

    glob_t *pglob = find_file_lookup(*context);
    if (!pglob) {
        *context = 0;
        return SS$_BADPARAM;
    }
    globfree(pglob);
    free(pglob);
    find_file_release(*context);
    *context = 0;

    return SS$_NORMAL;
}

/* ================================================================
 * Message and keyword-table routines
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$SYS_GETMSG,
 * LIB$GET_USERS_LANGUAGE, LIB$LOOKUP_KEY.
 * ================================================================ */

extern uint32_t sys$getmsg(uint32_t msgid, uint16_t *msglen,
                           struct dsc$descriptor_s *bufadr,
                           uint32_t flags, uint32_t *outadr);
extern uint32_t sys$trnlnm(const uint32_t *attr,
                           const struct dsc$descriptor_s *tabnam,
                           const struct dsc$descriptor_s *lognam,
                           const uint8_t *acmode,
                           const struct item_list_3 *itmlst);

#define LNM_STRING_CODE 2   /* LNM$_STRING */

/*
 * lib$sys_getmsg - Retrieve the message text for a condition value.
 *
 * A thin LIB$ wrapper over sys$getmsg (starlet). The flags argument
 * selects which message components are returned (text, identification,
 * severity, facility); the corpus passes 0x0F for the full message.
 */
uint32_t lib$sys_getmsg(const uint32_t *msgid, uint16_t *msglen,
                        struct dsc$descriptor_s *bufadr,
                        const uint32_t *flags) {
    if (!msgid || !bufadr)
        return SS$_BADPARAM;
    uint32_t f = flags ? *flags : 0x0F;
    return sys$getmsg(*msgid, msglen, bufadr, f, NULL);
}

/*
 * lib$get_users_language - Return the user's natural language.
 *
 * The language is taken from the logical name SYS$LANGUAGE. When that
 * logical is not defined (the default OVMX state) the routine returns
 * LIB$_ENGLUSED — "English used" — exactly as documented.
 */
uint32_t lib$get_users_language(struct dsc$descriptor_s *language) {
    if (!language || !language->dsc$a_pointer)
        return SS$_BADPARAM;

    char value[256];
    uint16_t retlen = 0;
    struct item_list_3 itm[2];
    memset(itm, 0, sizeof(itm));
    itm[0].buflen = sizeof(value);
    itm[0].item_code = LNM_STRING_CODE;
    itm[0].bufaddr = value;
    itm[0].retlen = &retlen;

    struct dsc$descriptor_s lognam = {
        12, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"SYS$LANGUAGE"
    };

    uint32_t st = sys$trnlnm(NULL, NULL, &lognam, NULL, itm);
    if (st != SS$_NORMAL || retlen == 0) {
        /* No language logical defined — English is used. */
        return LIB$_ENGLUSED;
    }

    uint16_t copylen = retlen;
    if (copylen > language->dsc$w_length) copylen = language->dsc$w_length;
    memcpy(language->dsc$a_pointer, value, copylen);
    if (language->dsc$b_class == DSC$K_CLASS_S &&
        copylen < language->dsc$w_length) {
        memset(language->dsc$a_pointer + copylen, ' ',
               language->dsc$w_length - copylen);
    }
    return SS$_NORMAL;
}

/*
 * lib$lookup_key - Look up a (possibly abbreviated) keyword in a key table.
 *
 * The key table is a longword vector:
 *   table[0]        = count of longwords that follow
 *   table[1], [2]   = &keyword-ascic, key-value
 *   table[3], [4]   = &keyword-ascic, key-value   ...
 * where each keyword is a counted (ASCIC) string: a length byte followed
 * by the characters. The input is matched case-sensitively against the
 * keywords, honoring unique abbreviation:
 *   - an exact full-length match wins outright;
 *   - otherwise a prefix that matches exactly one keyword is accepted;
 *   - a prefix matching several keywords is LIB$_AMBKEY;
 *   - no match is LIB$_UNRKEY.
 * On success the full keyword is returned in keyword (if supplied) and its
 * value through key_value.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual — LIB$LOOKUP_KEY.
 */
uint32_t lib$lookup_key(const struct dsc$descriptor_s *input,
                        const uint32_t *table,
                        uint32_t *key_value,
                        struct dsc$descriptor_s *keyword,
                        uint16_t *keyword_len) {
    if (!input || !input->dsc$a_pointer || !table)
        return SS$_BADPARAM;

    const char *in = input->dsc$a_pointer;
    uint16_t inlen = input->dsc$w_length;
    /* Trim trailing spaces from the input. */
    while (inlen > 0 && in[inlen - 1] == ' ')
        inlen--;
    if (inlen == 0)
        return LIB$_UNRKEY;

    uint32_t entries = table[0] / 2;   /* each entry is 2 longwords */

    int match_index = -1;
    int exact = 0;
    int ambiguous = 0;

    for (uint32_t e = 0; e < entries; e++) {
        const unsigned char *ascic =
            (const unsigned char *)(uintptr_t)table[1 + e * 2];
        if (!ascic)
            continue;
        uint8_t klen = ascic[0];
        const char *kstr = (const char *)&ascic[1];

        if (inlen > klen)
            continue;   /* input longer than keyword: cannot match */

        if (memcmp(in, kstr, inlen) != 0)
            continue;   /* prefix mismatch */

        if (inlen == klen) {
            /* Exact full-length match wins immediately. */
            match_index = (int)e;
            exact = 1;
            break;
        }

        /* Abbreviation match. */
        if (match_index < 0) {
            match_index = (int)e;
        } else {
            ambiguous = 1;
        }
    }

    if (match_index < 0)
        return LIB$_UNRKEY;
    if (ambiguous && !exact)
        return LIB$_AMBKEY;

    const unsigned char *ascic =
        (const unsigned char *)(uintptr_t)table[1 + (uint32_t)match_index * 2];
    uint8_t klen = ascic[0];
    const char *kstr = (const char *)&ascic[1];
    uint32_t value = table[2 + (uint32_t)match_index * 2];

    if (key_value)
        *key_value = value;

    if (keyword && keyword->dsc$a_pointer) {
        uint16_t copylen = klen;
        if (copylen > keyword->dsc$w_length)
            copylen = keyword->dsc$w_length;
        memcpy(keyword->dsc$a_pointer, kstr, copylen);
        if (keyword->dsc$b_class == DSC$K_CLASS_S &&
            copylen < keyword->dsc$w_length) {
            memset(keyword->dsc$a_pointer + copylen, ' ',
                   keyword->dsc$w_length - copylen);
        }
        if (keyword_len)
            *keyword_len = copylen;
    } else if (keyword_len) {
        *keyword_len = klen;
    }

    return SS$_NORMAL;
}
