/*
 * vms_abi_headers.c - the VMS-layout STARLET headers (vms-022) compile as a
 * DEC C client includes them, and the shapes GCC's own VMS-host code uses
 * (gcc/vmsdbgout.cc vms_file_stats_name) are expressible: FAB/NAM with
 * cc$rms_ initializers and nam$w_did, a 32-bit FIB descriptor, FAT field
 * access, string descriptors. Every offset/size assertion lives in the
 * headers themselves; compiling this TU at both pointer sizes, as C and as
 * C++, fires them all.
 */
#define __NEW_STARLET 1
#include <vms/rms.h>
#include <vms/fibdef.h>
#include <vms/stsdef.h>
#include <vms/iodef.h>
#include <vms/fatdef.h>
#include <vms/descrip.h>

struct vstring { short length; char string[NAM$C_MAXRSS + 1]; };

int vms_abi_shapes(const char *spec, unsigned short *did_out, FAT *recattr)
{
    struct FAB fab;
    struct NAM nam;
    FIBDEF fib;
    struct vstring file, device;
    struct dsc$descriptor_s devicedsc = { NAM$C_MAXRSS, DSC$K_DTYPE_T, DSC$K_CLASS_S, device.string };
    struct dsc$descriptor_s filedsc = { NAM$C_MAXRSS, DSC$K_DTYPE_T, DSC$K_CLASS_S, file.string };
    unsigned short chan = 0;
    int status;

    fab = cc$rms_fab;
    nam = cc$rms_nam;
    nam.nam$l_esa = file.string;
    nam.nam$b_ess = NAM$C_MAXRSS;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = 0;
    fab.fab$l_nam = &nam;
    status = SYS$PARSE(&fab, 0, 0);
    if ((status & STS$M_SUCCESS) != 1)
        return status;
    status = SYS$SEARCH(&fab, 0, 0);
    devicedsc.dsc$w_length = nam.nam$b_dev;
    status = SYS$ASSIGN(&devicedsc, &chan, 0, 0, 0);
    fib.fib$w_did[0] = nam.nam$w_did[0];
    fib.fib$w_did[1] = nam.nam$w_did[1];
    fib.fib$w_did[2] = nam.nam$w_did[2];
    did_out[0] = fib.fib$w_did[0];
    status = SYS$QIOW(0, chan, IO$_ACCESS | IO$M_ACCESS, 0, 0, 0, &fib, &filedsc, 0, 0, 0, 0);
    (void)SYS$DASSGN(chan);
    return (int)(recattr->fat$w_efblkl + recattr->fat$v_rtype + recattr->fat$w_ffbyte);
}
