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

/* Imported from starlet.h via forward declarations */
extern uint32_t sys$getjpiw(uint32_t efn, const uint32_t *pidadr,
                             void *prcnam,
                             void *itmlst,
                             void *iosb,
                             void (*astadr)(uint32_t), uint32_t astprm);
extern uint32_t sys$getsyiw(uint32_t efn, const uint32_t *csidadr,
                              const struct dsc$descriptor_s *nodename,
                              const struct item_list_3 *itmlst,
                              void *iosb,
                              void (*astadr)(uint32_t), uint32_t astprm);
extern uint32_t sys$getdviw(uint32_t efn, uint16_t chan,
                             struct dsc$descriptor_s *devnam,
                             void *itmlst, void *iosb,
                             void (*astadr)(uint32_t), uint32_t astprm,
                             uint32_t nullarg);

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
 *   prcnam      - Subprocess name (accepted, not yet applied: needs $CREPRC
 *                 executive registration -- prereq for MMK's named subprocess)
 *   pid         - Receives subprocess PID (or NULL)
 *   status      - Receives the subprocess completion status (or NULL)
 *   efn         - Event flag to set on completion (accepted, not yet wired)
 *   astadr      - Completion AST routine (accepted, not yet wired)
 *   astprm      - Completion AST parameter (accepted, not yet wired)
 *   prompt      - Prompt string (only meaningful for interactive; ignored)
 *   cli_name    - CLI name (accepted; OVMX's one CLI is DCL)
 *   table_name  - CLI table name (accepted; OVMX's one CLI is DCL)
 *
 * Return: SS$_NORMAL when the subprocess was created (the completion status
 * lands in *status); an error status when it could not be created.
 */

/* Child pre-exec sentinels: distinct exit codes so the parent can tell "the
 * CLI image never ran" (a spawn failure) from a command that ran and failed.
 * Chosen high to avoid colliding with DCL's own 0/1 image exit. */
#define OVMX_SPAWN_IOERR    250   /* SYS$INPUT/SYS$OUTPUT redirection failed */
#define OVMX_SPAWN_EXECERR  251   /* execl() of the CLI image failed         */

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
    (void)prcnam; (void)efn; (void)astadr; (void)astprm;
    (void)prompt; (void)cli_name; (void)table_name;

    const uint32_t spawn_flags = flags ? *flags : 0;
    const int nowait = (spawn_flags & CLI$M_NOWAIT) != 0;

    /*
     * Resolve the DCL CLI image through the VMS filespec translator so a
     * redefined SYS$SYSTEM (e.g. an alternate system root) is honored, the
     * same resolution PROVISION.EXE and JOB_CONTROL use. This is userspace
     * (device table + logical names); it needs no executive.
     */
    char dcl_path[1024];
    if (vmsfs_to_linux_path(VMS_DCL_PATH, dcl_path, sizeof(dcl_path)) != 1)
        return SS$_NOSUCHFILE;

    /*
     * The CLI image must be a real, executable regular file. If it is not,
     * FAIL HONESTLY -- do not fall back to any other program. A spawn that
     * cannot find its command interpreter created no subprocess.
     */
    struct stat st;
    if (stat(dcl_path, &st) != 0 || !S_ISREG(st.st_mode) ||
        access(dcl_path, X_OK) != 0)
        return SS$_NOSUCHFILE;

    char cmd[4096] = "";
    const int have_cmd = command && command->dsc$a_pointer &&
                         command->dsc$w_length > 0;
    if (have_cmd)
        dsc$strncpy(cmd, command, sizeof(cmd));

    char in_path[1024]  = "";
    char out_path[1024] = "";
    const int have_in  = input_file  && input_file->dsc$a_pointer &&
                         input_file->dsc$w_length  > 0;
    const int have_out = output_file && output_file->dsc$a_pointer &&
                         output_file->dsc$w_length > 0;
    if (have_in)  spawn_resolve_spec(input_file,  in_path,  sizeof(in_path));
    if (have_out) spawn_resolve_spec(output_file, out_path, sizeof(out_path));

    pid_t child = fork();
    if (child < 0) return SS$_INSFMEM;

    if (child == 0) {
        /* Child: SYS$INPUT / SYS$OUTPUT redirection, then BECOME DCL. */
        if (have_in && !freopen(in_path, "r", stdin))
            _exit(OVMX_SPAWN_IOERR);
        if (have_out && !freopen(out_path, "w", stdout))
            _exit(OVMX_SPAWN_IOERR);

        if (have_cmd)
            /* DCL executes the one command and exits (dcl_main.c -c mode). */
            execl(dcl_path, "DCL", "-c", cmd, (char *)NULL);
        else
            /* No command: interactive DCL reading SYS$INPUT. */
            execl(dcl_path, "DCL", (char *)NULL);

        _exit(OVMX_SPAWN_EXECERR);  /* only reached if execl() failed */
    }

    /* Parent */
    if (pid) *pid = (uint32_t)child;

    if (nowait) {
        /*
         * CLI$M_NOWAIT: the subprocess was created; return at once. The
         * completion status is not known yet, and the efn/AST completion
         * NOTIFICATION path (LIB$SPAWN's event flag / AST arguments) is
         * prereq C (the write-attention AST, vms-9003) -- accepted here but
         * not yet delivered, so *status is left unwritten rather than filled
         * with a value that has not happened.
         */
        return SS$_NORMAL;
    }

    /* Wait mode: HIBERNATE until the subprocess completes (VMS default). */
    int wstatus;
    while (waitpid(child, &wstatus, 0) < 0 && errno == EINTR)
        ;

    if (WIFEXITED(wstatus)) {
        int ec = WEXITSTATUS(wstatus);
        if (ec == OVMX_SPAWN_IOERR || ec == OVMX_SPAWN_EXECERR) {
            /* The CLI image never ran the command -> spawn failed. */
            if (status) *status = SS$_ABORT;
            return SS$_NOSUCHFILE;
        }
        /*
         * DCL's -c mode collapses $STATUS to a shell 0/1 (dcl_main.c returns
         * `(status & 1) ? 0 : 1`), so the completion status recoverable HERE
         * is success/failure, not the subprocess's full VMS $STATUS. Full
         * $STATUS fidelity is the persistent-subprocess + mailbox EOM-marker
         * protocol MMK uses (build_target.c "MMK____status="), which rides on
         * prereqs B/C -- see the DEFERRED note below.
         */
        if (status) *status = (ec == 0) ? SS$_NORMAL : SS$_ABORT;
        return SS$_NORMAL;
    }

    /* Killed by a signal: the subprocess was created but did not complete. */
    if (status) *status = SS$_ABORT;
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
