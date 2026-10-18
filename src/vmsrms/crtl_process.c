/*
 * crtl_process.c - the DEC C process-environment calls of the RMS-backed (pass-2)
 * DECC$SHR, next to crtl_features.c: system() and getcwd() (dflt-65a).
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
 * command (odd = success). The GCC port's linker wrapper (dflt-ld.c) writes
 * system("$ link ...") and tests bit 0. A leading "$" is the DCL prompt and is
 * dropped. system(NULL) says a command interpreter exists.
 *
 * LIB$SPAWN (LIBVMS$SHR, libvms/rtl/lib_misc.c) creates the subprocess through
 * $CREPRC and feeds DCL the command; this pass of DECC$SHR is linked --use
 * LIBVMS$SHR so the call is a real cross-image import, loaded with every C RTL
 * image. The musl system() the archive also carries (a clone with CLONE_VFORK,
 * which the executive refuses) stays an internal symbol: decc$system is rebound
 * in place to ovmx_crtl_system by mk_decc_shr.sh.
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
