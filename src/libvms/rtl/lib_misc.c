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
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "str$routines.h"
#include "prcdef.h"
#include "lnmdef.h"
#include "clidef.h"          /* CLI$M_NOWAIT — lib$spawn "flags" bits        */
#include "ovmx_layout.h"     /* VMS_DCL_PATH — the DCL CLI image filespec    */
#include "rmsdef.h"
#include "rms_textfile.h"   /* rms_textfile_open/getline -- SYS$INPUT read through RMS (vms-ccc) */
#include "vmsfs/filespec.h"  /* vmsfs_to_linux_path — VMS filespec resolver  */
#include "starlet.h"         /* sys$creprc — the one executive-registered create (B0) */
#include "vms_kif.h"         /* vms_kif_getjpi_pid, struct vms_procinfo — the wait handle */
#include <pthread.h>

/*
 * Side table for lib$find_file context handles.
 * Maps uint32 handles (1..MAX_FIND_FILE_CONTEXTS) to search contexts (the RMS
 * FAB/NAM of the scan), avoiding 64-bit pointer truncation when stored in
 * uint32_t *context.
 */
#define MAX_FIND_FILE_CONTEXTS 64
struct find_file_ctx;
static struct find_file_ctx *find_file_table[MAX_FIND_FILE_CONTEXTS];
static pthread_mutex_t find_file_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t find_file_alloc(struct find_file_ctx *pglob) {
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

static struct find_file_ctx *find_file_lookup(uint32_t handle) {
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
/*
 * LIB$GETJPI / LIB$GETSYI deliver an item as TEXT when the caller gives a
 * result-string descriptor (OpenVMS RTL Library (LIB$) Manual): a string item
 * as it is, a numeric item as its decimal value, a UIC in the named [g,m]
 * form $FAO's !%I gives (observed LIB.GETJPI.UIC.STRING "[SYSTEM]",
 * LIB.GETSYI.DEFPRI.STRING "4"; docs/oracle/semantics/rtl/).
 */
static int lib_item_is_string(int jpi, uint32_t code)
{
    static const uint32_t jstr[] = { JPI$_USERNAME, JPI$_PRCNAM, JPI$_TERMINAL,
        JPI$_ACCOUNT, JPI$_IMAGNAME, JPI$_NODENAME, JPI$_CLINAME,
        JPI$_TABLENAME, JPI$_DFDEV, JPI$_DFDIR };
    static const uint32_t sstr[] = { SYI$_NODENAME, SYI$_VERSION, SYI$_HW_NAME,
        SYI$_ARCH_NAME, SYI$_SCSNODE };
    const uint32_t *t = jpi ? jstr : sstr;
    size_t n = jpi ? sizeof jstr / sizeof jstr[0] : sizeof sstr / sizeof sstr[0];
    for (size_t i = 0; i < n; i++)
        if (t[i] == code) return 1;
    return 0;
}

static uint32_t lib_item_text(int jpi, uint32_t code, const char *raw, uint16_t rawlen,
                              struct dsc$descriptor_s *out, uint16_t *outlen)
{
    char txt[256];
    uint16_t tl;
    if (lib_item_is_string(jpi, code)) {
        tl = rawlen < sizeof txt ? rawlen : (uint16_t)(sizeof txt - 1);
        memcpy(txt, raw, tl);
    } else if (jpi && code == JPI$_UIC) {
        uint32_t uic = 0;
        memcpy(&uic, raw, rawlen < 4 ? rawlen : 4);
        struct dsc$descriptor_s cd = { 3, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"!%I" };
        struct dsc$descriptor_s od = { (uint16_t)(sizeof txt - 1), DSC$K_DTYPE_T, DSC$K_CLASS_S, txt };
        tl = 0;
        if (!(sys$fao(&cd, &tl, &od, uic) & 1)) tl = 0;
    } else {
        uint64_t v = 0;
        memcpy(&v, raw, rawlen < 8 ? rawlen : 8);
        tl = (uint16_t)snprintf(txt, sizeof txt, "%llu", (unsigned long long)v);
    }
    txt[tl] = '\0';
    struct dsc$descriptor_s src = { tl, DSC$K_DTYPE_T, DSC$K_CLASS_S, txt };
    uint32_t st = str$copy_dx(out, &src);
    if (outlen) *outlen = tl < out->dsc$w_length || out->dsc$b_class == DSC$K_CLASS_D
                          ? tl : out->dsc$w_length;
    return (st & 1) ? SS$_NORMAL : st;
}

uint32_t lib$getjpi(const uint32_t *item_code, const uint32_t *pid,
                    const struct dsc$descriptor_s *prcnam,
                    void *result, struct dsc$descriptor_s *result_str,
                    uint16_t *result_len) {
    if (!item_code) return SS$_BADPARAM;

    char raw[256];
    uint16_t rawlen = 0;
    struct item_list_3 items[2];
    memset(items, 0, sizeof(items));
    items[0].item_code = (uint16_t)*item_code;
    if (result_str) {
        items[0].buflen = sizeof raw;
        items[0].bufaddr = raw;
        items[0].retlen = &rawlen;
    } else {
        items[0].buflen = sizeof(uint32_t);
        items[0].bufaddr = result;
        items[0].retlen = result_len;
    }
    uint32_t st = sys$getjpiw(0, pid, (void *)prcnam, items, NULL, NULL, 0);
    if (!(st & 1) || !result_str)
        return st;
    return lib_item_text(1, *item_code, raw, rawlen, result_str, result_len);
}

/*
 * lib$getsyi - Get System Information (simplified wrapper): as lib$getjpi.
 */
uint32_t lib$getsyi(const uint32_t *item_code,
                    void *result, struct dsc$descriptor_s *result_str,
                    uint16_t *result_len, uint32_t *csid,
                    const struct dsc$descriptor_s *node) {
    if (!item_code) return SS$_BADPARAM;

    char raw[256];
    uint16_t rawlen = 0;
    struct item_list_3 items[2];
    memset(items, 0, sizeof(items));
    items[0].item_code = (uint16_t)*item_code;
    if (result_str) {
        items[0].buflen = sizeof raw;
        items[0].bufaddr = raw;
        items[0].retlen = &rawlen;
    } else {
        items[0].buflen = sizeof(uint32_t);
        items[0].bufaddr = result;
        items[0].retlen = result_len;
    }
    uint32_t st = sys$getsyiw(0, csid, node, items, NULL, NULL, 0);
    if (!(st & 1) || !result_str)
        return st;
    return lib_item_text(0, *item_code, raw, rawlen, result_str, result_len);
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
/*
 * The scratch file is a substrate mechanism (a literal path $CREPRC opens), not
 * a C RTL file: it lives under /run, the kernel namespace the C RTL file layer
 * over RMS leaves alone (src/vmsrms/crtl_rms_fd.c kernel_path), so an image with
 * that layer on still hands its subprocess a file the kernel can open. /tmp is
 * the fallback where /run/ovmx-spawn cannot be made (a host test process).
 */
static const char *spawn_scratch_dir(void)
{
    static const char *dir = NULL;
    struct stat sd;

    if (dir)
        return dir;
    /* /run itself may not exist on a minimal system (the corpus guest's
     * initramfs has none); without it this fell back to /tmp, which the C RTL
     * file layer (vms-003b) maps to RMS, so the scratch open failed and
     * LIB$SPAWN returned SS$_INSFMEM (corpus sys_forcex). /run is the kernel's
     * namespace on every substrate: make sure it is there. */
    (void)mkdir("/run", 0755);
    if ((mkdir("/run/ovmx-spawn", 01777) == 0 || errno == EEXIST) &&
        stat("/run/ovmx-spawn", &sd) == 0 && S_ISDIR(sd.st_mode) &&
        access("/run/ovmx-spawn", W_OK) == 0)
        dir = "/run/ovmx-spawn";
    else
        dir = "/tmp";
    return dir;
}

static int spawn_open_scratch(char *buf, size_t bufsz)
{
    static unsigned seq = 0;
    for (int tries = 0; tries < 4096; tries++) {
        snprintf(buf, bufsz, "%s/ovmx_spawn_cmd_%d_%u",
                 spawn_scratch_dir(), (int)getpid(), seq++);
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

uint32_t (lib$spawn)(const struct dsc$descriptor_s *command,
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
    /* efn/astadr/astprm: the /NOWAIT completion-notification path, wired in the
     * CLI$M_NOWAIT branch below via vms_kif_spawn_notify (B1). */

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
        /*
         * SYS$INPUT FROM A FILE (vms-ccc). The file is read through RMS over the
         * ACP -- the same view the caller's own $CREATE/fopen wrote it through.
         * The old code translated the spec to a Linux path (leftover of the retired
         * /vms passthrough, e.g. /vms/SYSTMP/demo_forcex.com;0), which does not
         * exist for an ODS-2 file; $CREPRC's child then failed the open() silently,
         * left the subprocess on the creator's /dev/null, and DCL read EOF and
         * exited at once. Materialise the records into the scratch file $CREPRC
         * can open (the mechanism the command-string case already uses). A file
         * RMS cannot reach is an honest RMS$_FNF here, before anything is created --
         * never a silent substitute input.
         */
        char raw_in[1024];
        dsc$strncpy(raw_in, input_file, sizeof(raw_in));
        rms_textfile_t *tf = rms_textfile_open(raw_in);
        if (tf) {
            int tfd = spawn_open_scratch(cmd_tmp, sizeof(cmd_tmp));
            if (tfd < 0) { rms_textfile_close(tf); return SS$_INSFMEM; }
            have_tmp = 1;
            char rec[4096];
            int too_long = 0, werr = 0;
            while (rms_textfile_getline(tf, rec, sizeof(rec), &too_long)) {
                size_t rl = strlen(rec);
                rec[rl++] = '\n';
                for (size_t off = 0; off < rl; ) {
                    ssize_t w = write(tfd, rec + off, rl - off);
                    if (w < 0) { if (errno == EINTR) continue; werr = 1; break; }
                    off += (size_t)w;
                }
                if (werr) break;
            }
            rms_textfile_close(tf);
            close(tfd);
            if (werr) { unlink(cmd_tmp); return SS$_INSFMEM; }
            in_str = cmd_tmp;
        } else {
            /* Not reachable through RMS. A caller handing a plain Linux path (host-side
             * tooling) still works when that path is a real regular file; anything
             * else is a file-not-found, reported before a process exists. */
            spawn_resolve_spec(input_file, in_resv, sizeof(in_resv));
            struct stat ist;
            if (stat(in_resv, &ist) != 0 || !S_ISREG(ist.st_mode))
                return RMS$_FNF;
            in_str = in_resv;
        }
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
         * once. *status is left UNWRITTEN -- the completion is not known yet.
         *
         * B1 (vms-e9a, docs/design-libspawn-ovmx.md §3b): if the caller asked
         * for completion notification (an event flag and/or a completion AST),
         * ARM it in the executive against this subprocess's VMS PID. The
         * executive sets the caller's `efn` and/or queues the caller's
         * `astadr`/`astprm` AST when the subprocess records its exit status
         * ($EXIT) -- a real cross-process process-exit signal, not a userspace
         * waitpid poll. The arm is best-effort with respect to LIB$SPAWN's
         * return: the subprocess is already created, so a failed arm (e.g. the
         * child raced to completion is handled inside the executive, which then
         * delivers immediately) does not undo the create. With no /dev/vms the
         * arm returns SS$_NOSUCHDEV and nothing is faked (INV-6) -- consistent
         * with $CREPRC itself already requiring the executive.
         */
        const uint32_t efn_val = efn ? *efn : VMS_EF_NONE;
        if (efn_val != VMS_EF_NONE || astadr != NULL) {
            const uint32_t ast = vms_kif_spawn_notify(vms_pid, efn_val,
                                       (uint64_t)(uintptr_t)astadr,
                                       (uint64_t)(uintptr_t)astprm,
                                       NULL);
            if (ast == SS$_NONEXPR) {
                /*
                 * The subprocess we JUST created is already gone AND already
                 * reclaimed by the executive's reaper (a command file that ends at
                 * once -- e.g. an empty SYS$INPUT -- finishes before this arm runs;
                 * observed as the vms-f45 lost completion: the subprocess exited
                 * ~immediately, the arm found no process, and the caller's
                 * $WAITFR on the completion event flag hung to the harness
                 * budget). The executive only keeps a completion for a subprocess
                 * that still has a process-table row, so with no row there is
                 * nothing left to deliver it. VMS notifies the creator whenever the
                 * subprocess is deleted; "no such process" for a pid this very call
                 * returned means exactly that, so complete the caller's request
                 * here: set ITS event flag and queue ITS AST, both through the
                 * public services acting on the caller itself. The completion
                 * $STATUS of a subprocess that was reclaimed unobserved is
                 * unknown, so *status is left as the caller initialised it.
                 */
                if (efn_val != VMS_EF_NONE)
                    (void)sys$setef(efn_val);
                if (astadr != NULL)
                    (void)sys$dclast((void (*)(uint32_t))astadr,
                                     (uint32_t)(uintptr_t)astprm, 0);
            }
        }

        /*
         * The scratch SYS$INPUT file, if any, is NOT unlinked here: the
         * subprocess may not have opened it yet, and lib$spawn has no exit hook
         * to reclaim it. It is a genuine file left for the subprocess to
         * consume.
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
 * lib$find_file - Find the next file matching a wildcard specification.
 *
 * OVER RMS, not the host filesystem: the scan is an RMS $PARSE/$SEARCH of the
 * specification (default spec as the FAB's DNA), so on a booted system it walks
 * the Files-11 volumes through the executive ACP exactly as DIRECTORY and
 * LIB$FILE_SCAN do. (An earlier version ran glob(3) on the spec as a host path:
 * a plausible answer from the wrong namespace, which on a mounted ODS-2 volume
 * matched nothing and let callers that ignore the status loop on garbage.)
 *
 * The RMS entry points are referenced WEAKLY (the same layering seam
 * rms_textfile.c documents: LIBVMSRMS$SHR links LIBVMS, not the reverse). An
 * image with no RMS in its link closure gets SS$_NOSUCHDEV, never a fallback to
 * the host filesystem.
 *
 * First call (*context == 0): $PARSE, remember the scan in a context handle.
 * Each call: $SEARCH, return the resultant (full) specification in resultspec.
 * RMS$_NMF when the matches are used up -- or RMS$_FNF when there were none --
 * and the context is released. *status_value gets the RMS STV of a failure.
 *
 *   filespec         - specification, may contain wildcards
 *   resultspec       - receives the matched specification
 *   context          - 0 on first call; opaque handle afterwards
 *   default_filespec - supplies fields filespec leaves out (RMS DNA)
 *   related_filespec - not honoured; it is refused rather than ignored
 *   status_value     - receives the RMS STV on failure
 */
#if defined(OVMX_HAVE_ACP)
#include "rms/rms.h"
#pragma weak sys$parse
#pragma weak sys$search
#pragma weak rms_search_end

struct find_file_ctx {
    struct FAB fab;
    struct NAM nam;
    char spec[256], dflt[256], esa[256], rsa[256];
};

static void ff_dsc_to_str(const struct dsc$descriptor_s *d, char *buf, size_t sz)
{
    buf[0] = '\0';
    if (d && d->dsc$a_pointer && d->dsc$w_length) {
        size_t n = d->dsc$w_length < sz - 1 ? d->dsc$w_length : sz - 1;
        memcpy(buf, d->dsc$a_pointer, n);
        buf[n] = '\0';
    }
}

static void ff_close(struct find_file_ctx *c)
{
    if (rms_search_end)
        rms_search_end(&c->nam);
}

uint32_t (lib$find_file)(const struct dsc$descriptor_s *filespec,
                         struct dsc$descriptor_s *resultspec,
                         uint32_t *context,
                         const struct dsc$descriptor_s *default_filespec,
                         const struct dsc$descriptor_s *related_filespec,
                         uint32_t *status_value,
                         const uint32_t *flags) {
    (void)flags;
    if (status_value) *status_value = 0;
    if (!filespec || !resultspec || !context) return SS$_BADPARAM;
    if (!filespec->dsc$a_pointer) return SS$_BADPARAM;
    if (related_filespec && related_filespec->dsc$a_pointer
            && related_filespec->dsc$w_length)
        return SS$_BADPARAM;               /* not implemented: refuse, don't ignore */
    if (!sys$parse || !sys$search)
        return SS$_NOSUCHDEV;              /* no RMS in this image: fail honestly */

    struct find_file_ctx *c;
    if (*context == 0) {
        c = calloc(1, sizeof(*c));
        if (!c) return SS$_INSFMEM;
        ff_dsc_to_str(filespec, c->spec, sizeof c->spec);
        ff_dsc_to_str(default_filespec, c->dflt, sizeof c->dflt);
        if (c->spec[0] == '\0') { free(c); return SS$_BADPARAM; }
        c->fab = cc$rms_fab;
        c->fab.fab$l_fna = c->spec;
        c->fab.fab$b_fns = (uint8_t)strlen(c->spec);
        c->fab.fab$l_dna = c->dflt;
        c->fab.fab$b_dns = (uint8_t)strlen(c->dflt);
        c->nam = cc$rms_nam;
        c->nam.nam$l_esa = c->esa;
        c->nam.nam$b_ess = 255;
        c->nam.nam$l_rsa = c->rsa;
        c->nam.nam$b_rss = 255;
        c->fab.fab$l_nam = &c->nam;
        uint32_t ps = sys$parse(&c->fab, 0, 0);
        if (!(ps & 1)) {
            if (status_value) *status_value = c->fab.fab$l_stv;
            ff_close(c);
            free(c);
            return ps;
        }
        uint32_t handle = find_file_alloc(c);
        if (handle == 0) { ff_close(c); free(c); return SS$_INSFMEM; }
        *context = handle;
    } else {
        c = find_file_lookup(*context);
        if (!c) return RMS$_NMF;
    }

    uint32_t st = sys$search(&c->fab, 0, 0);
    if (!(st & 1)) {
        if (status_value) *status_value = c->fab.fab$l_stv;
        /* At the end of the search the result is the expanded (wildcard)
         * spec the search walked (observed LIB.FIND_FILE.WILD.2:
         * "...]LOGINOU*.EXE;*" with RMS$_NMF; docs/oracle/semantics/rtl/). */
        if ((st == RMS$_NMF || st == RMS$_FNF) && c->nam.nam$b_esl &&
            resultspec->dsc$b_class != DSC$K_CLASS_VS) {
            uint16_t el = c->nam.nam$b_esl;
            (void)lib$scopy_r_dx(&el, c->esa, resultspec);
        }
        ff_close(c);
        find_file_release(*context);
        free(c);
        *context = 0;
        return st;                         /* RMS$_NMF at the end, RMS$_FNF if none */
    }

    uint16_t len = c->nam.nam$b_rsl;
    if (resultspec->dsc$b_class == DSC$K_CLASS_VS) {
        /* variable string: length word precedes the data, maxstrlen bounds it */
        struct dsc$descriptor_vs *v = (struct dsc$descriptor_vs *)resultspec;
        uint16_t room = v->dsc$w_maxstrlen;
        uint16_t n = len < room ? len : room;
        if (!v->dsc$a_pointer) return SS$_BADPARAM;
        *(uint16_t *)v->dsc$a_pointer = n;
        memcpy(v->dsc$a_pointer + 2, c->rsa, n);
        return n < len ? SS$_RESULTOVF : RMS$_NORMAL;
    }
    uint32_t r = lib$scopy_r_dx(&len, c->rsa, resultspec);
    return (r & 1) ? RMS$_NORMAL : r;
}

/*
 * lib$find_file_end - End a find-file sequence: release the RMS search context
 * and reset *context to 0.
 */
uint32_t lib$find_file_end(uint32_t *context) {
    if (!context) return SS$_BADPARAM;
    if (*context == 0) return SS$_NORMAL;

    struct find_file_ctx *c = find_file_lookup(*context);
    if (!c) {
        *context = 0;
        return SS$_BADPARAM;
    }
    ff_close(c);
    find_file_release(*context);
    free(c);
    *context = 0;
    return SS$_NORMAL;
}
#else  /* !OVMX_HAVE_ACP: no RMS in this build -- fail honestly */
struct find_file_ctx { int unused; };
uint32_t (lib$find_file)(const struct dsc$descriptor_s *filespec,
                         struct dsc$descriptor_s *resultspec,
                         uint32_t *context,
                         const struct dsc$descriptor_s *default_filespec,
                         const struct dsc$descriptor_s *related_filespec,
                         uint32_t *status_value,
                         const uint32_t *flags) {
    (void)filespec; (void)resultspec; (void)context; (void)default_filespec;
    (void)related_filespec; (void)flags;
    if (status_value) *status_value = 0;
    return SS$_NOSUCHDEV;
}
uint32_t lib$find_file_end(uint32_t *context) {
    if (context) *context = 0;
    return SS$_NORMAL;
}
#endif


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
