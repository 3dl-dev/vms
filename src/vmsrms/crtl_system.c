/*
 * crtl_system.c - DEC C system() for OVMX (vms-65a), part of the RMS-backed
 * (pass-2) DECC$SHR next to crtl_features.c.
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
 */
#include <string.h>

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
