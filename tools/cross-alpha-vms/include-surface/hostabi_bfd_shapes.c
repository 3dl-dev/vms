/*
 * hostabi_bfd_shapes.c - the DEC C shapes binutils' bfd/vms-misc.c uses
 * (vms-5303e): #include <rms.h> / <starlet.h> / <gen64def.h> under
 * __NEW_STARLET, struct FAB from cc$rms_fab with fab$l_ctx and FAB$M_ESC,
 * sys$open / sys$modify / sys$close by their lower-case spellings,
 * SYS$ASCTIM on a hand-built {size, pointer} descriptor, and sys$gettim on a
 * struct _generic_64. (decc$to_vms / <unixlib.h> needs the target libc headers,
 * which this bare-compiler proof does not have; the host build links it for real.)
 * Compiled with src/libvms/include/hostabi ahead of the
 * sysroot, which is how the host binutils build resolves <rms.h>.
 */
#define __NEW_STARLET
#include <rms.h>
#include <gen64def.h>
#include <starlet.h>
#define RME$C_SETRFM 0x00000001

static void convert_to_var(char *vms_filename)
{
    struct FAB fab = cc$rms_fab;

    fab.fab$l_fna = vms_filename;
    fab.fab$b_fns = __builtin_strlen(vms_filename);
    fab.fab$b_fac = FAB$M_PUT;
    fab.fab$l_fop = FAB$M_ESC;
    fab.fab$l_ctx = RME$C_SETRFM;
    sys$open(&fab);
    fab.fab$b_rfm = FAB$C_VAR;
    sys$modify(&fab);
    sys$close(&fab);
}

int hostabi_cb(char *filename, int type)
{
    (void)type;
    convert_to_var(filename);
    return 1;
}

unsigned char *hostabi_timestring(unsigned char *tbuf)
{
    struct { int Size; unsigned char *Ptr; } Descriptor;
    Descriptor.Size = 17;
    Descriptor.Ptr = tbuf;
    SYS$ASCTIM(0, &Descriptor, 0, 0);
    return tbuf;
}

void hostabi_get_time(unsigned int *hi, unsigned int *lo)
{
    struct _generic_64 t;

    sys$gettim(&t);
    *lo = t.gen64$q_quadword;
    *hi = t.gen64$q_quadword >> 32;
}
