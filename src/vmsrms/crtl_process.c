/*
 * crtl_process.c - the DEC C process-environment calls of the RMS-backed (pass-2)
 * DECC$SHR, next to crtl_features.c: system() and getcwd() (vms-65a).
 *
 * getcwd(buf, size, vms_flag): the process's default directory, which the
 * executive keeps ($SETDDIR / SET DEFAULT) and RMS completes relative file
 * specifications in. With vms_flag it is returned as the VMS "DEV:[DIR]"; without,
 * in the DEC C Unix form "/dev/dir". (musl's getcwd answered the substrate's "/",
 * which says nothing about where an image's relative files are.)
 *
 * system():

 *
 * DEC C: the string is a DCL command run in a subprocess by the command language
 * interpreter, through LIB$SPAWN, and the value is the VMS condition value of the
 * command (odd = success). The GCC port's linker wrapper (vms-ld.c) writes
 * system("$ link ...") and tests bit 0. A leading "$" is the DCL prompt and is
 * dropped. system(NULL) says a command interpreter exists.
 *
 * LIB$SPAWN (LIBVMS$SHR, libvms/rtl/lib_misc.c) creates the subprocess through
 * $CREPRC and feeds DCL the command; this pass of DECC$SHR is linked --use
 * LIBVMS$SHR so the call is a real cross-image import, loaded with every C RTL
 * image. The musl system() the archive also carries (a clone with CLONE_VFORK,
 * which the executive refuses) stays an internal symbol: decc$system is rebound
 * in place to ovmx_crtl_system by mk_decc_shr.sh.
 *
 * exit() and __posix_exit() (vms-599): the value an image hands exit() is its VMS
 * completion status, which DCL reads as $STATUS. Measured on OpenVMS Alpha V8.4
 * (tests/lab/captures/decc-exit-alpha-20261021, MACRO-32 calling the C RTL entry
 * points): DECC$EXIT turns 0 into %X00000001 (SS$_NORMAL) and passes every other
 * value through unchanged (1, 2, 3, %X1000002C). DECC$__POSIX_EXIT (what exit()
 * is under _POSIX_EXIT) keeps the low eight bits, turns 0 into %X00000001, and any
 * other n into a C-facility message: %X0035A001 + (n << 3), with 1 made an error
 * whose message is inhibited, %X1035A00A (2 -> %X0035A011, 255 -> %X0035A7F9,
 * 256 -> %X00000001). musl's exit() ran the atexit handlers and flushed stdio, then
 * exit_group()ed a POSIX code, and nothing recorded the status: DCL saw exit(1),
 * a success, as "exited with error status %X00000001". Both entry points record
 * the status, then run musl's exit; the C RTL file layer's system-call hook
 * (crtl_rms_fd.c) sees the final exit_group and ends the image with SYS$EXIT of
 * that status, after the handlers and the flush, as the DEC C RTL does.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "rms/crtl_filespec.h"

/* a class-S string descriptor, as LIB$SPAWN takes its command */
struct crtl_sys_dsc { unsigned short len; unsigned char dtype, cls; const char *ptr; };

extern unsigned int lib$spawn(const struct crtl_sys_dsc *command, const void *input,
                              const void *output, const unsigned int *flags,
                              const void *prcnam, unsigned int *pid, unsigned int *status,
                              const unsigned int *efn, void *astadr, void *astprm,
                              const void *prompt, const void *cli, const void *table);

int ovmx_crtl_system(const char *cmd)
{
    if (!cmd)
        return 1;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    if (*cmd == '$')
        cmd++;
    while (*cmd == ' ' || *cmd == '\t')
        cmd++;
    struct crtl_sys_dsc d = { (unsigned short)strlen(cmd), 14 /* DSC$K_DTYPE_T */, 1 /* DSC$K_CLASS_S */, cmd };
    unsigned int status = 0x2Cu;                    /* SS$_ABORT unless the command reports */
    unsigned int st = lib$spawn(&d, 0, 0, 0, 0, 0, &status, 0, 0, 0, 0, 0, 0);
    if (!(st & 1u))
        return (int)st;                             /* the subprocess was not created */
    return (int)status;
}

/* $SETDDIR, read-only: the default directory as a class-S descriptor pair */
struct crtl_dsc { unsigned short len; unsigned char dtype, cls; char *ptr; };
extern unsigned int sys$setddir(const struct crtl_dsc *new_dir, unsigned short *old_len,
                                struct crtl_dsc *old_dir);

char *ovmx_crtl_getcwd(char *buf, unsigned long size, int vms_flag)
{
    char dflt[256], out[512];
    struct crtl_dsc d = { sizeof dflt - 1, 14, 1, dflt };
    unsigned short n = 0;
    if (!(sys$setddir(0, &n, &d) & 1) || n == 0 || n >= sizeof dflt)
        n = 0;
    dflt[n] = '\0';
    if (n == 0)
        strcpy(dflt, "SYS$DISK:[000000]");   /* no default recorded: the system disk's top */
    if (vms_flag) {
        strcpy(out, dflt);
    } else if (ovmx_crtl_vms_to_unix(dflt, out, sizeof out) < 0) {
        errno = ERANGE;
        return 0;
    }
    size_t need = strlen(out) + 1;
    if (!buf) {
        buf = malloc(size ? size : need);
        if (!buf) {
            errno = ENOMEM;
            return 0;
        }
        if (size && size < need) {
            free(buf);
            errno = ERANGE;
            return 0;
        }
    } else if (size < need) {
        errno = ERANGE;
        return 0;
    }
    memcpy(buf, out, need);
    return buf;
}

/* ------------------------------------------------------- exit (vms-599) ---- */

extern unsigned int sys$exit(unsigned int code);            /* LIBVMS$SHR */
extern void (*__ovmx_crtl_exit_record)(void);               /* crtl_rms_fd.c */

static unsigned int crtl_exit_cond;

/* Called from the system-call hook at the image's final exit_group: end the
 * image with SYS$EXIT of the recorded status (it runs the exit handlers, records
 * the status with the executive and terminates). The cell is cleared first, so
 * SYS$EXIT's own exit_group passes straight through the hook. */
static void crtl_exit_record(void)
{
    __ovmx_crtl_exit_record = 0;
    (void)sys$exit(crtl_exit_cond);
}

static void crtl_exit_with(unsigned int cond)
{
    crtl_exit_cond = cond;
    __ovmx_crtl_exit_record = crtl_exit_record;
    exit((cond & 1u) ? 0 : 1);                     /* musl: atexit, stdio flush */
}

/* DEC C exit(): 0 is SS$_NORMAL, any other value is the status itself. */
void ovmx_crtl_exit(int status)
{
    crtl_exit_with(status == 0 ? 1u : (unsigned int)status);
}

/* DEC C __posix_exit() (exit() under _POSIX_EXIT): a POSIX code 0-255 as a
 * C-facility condition value (see the measurement above). */
void ovmx_crtl_posix_exit(int status)
{
    unsigned int n = (unsigned int)status & 255u;
    unsigned int cond;
    if (n == 0)
        cond = 1u;
    else if (n == 1)
        cond = 0x1035A00Au;
    else
        cond = 0x0035A001u + (n << 3);
    crtl_exit_with(cond);
}
