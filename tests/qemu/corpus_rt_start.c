/*
 * corpus_rt_start.c - what the corpus guest does BEFORE main() (vms-44a, R2.3).
 *
 * 1. Run as SYSTEM. The guest has no login: every program runs as the process
 *    STARTUP runs under, SYSTEM [1,4] with the executive's own full privilege mask
 *    (vms_kif_establish_system() asks the executive for exactly that; it refuses
 *    without CAP_SYS_ADMIN, which init.sh's root has). Programs that look their own
 *    user name up in SYSUAF ($GETUAI, $CREPRC) need a name that is in it.
 * 2. Give it SYSTEM's default directory, as LOGINOUT does at login: the SYSUAF
 *    record's default device + directory (SYS$SYSROOT:[SYSMGR]), stored with
 *    $SETDDIR in the executive. A program that names a file or a directory
 *    relative to its default ([.LOG], X.DAT) then resolves it on the mounted,
 *    writable system disk.
 * 3. The RMS force-bind anchor below.
 *
 * The process control block is NOT made here any more: libvms establishes it from
 * the executive's row on the first PCB-backed service (ovmx_pcb_ctx.h), as VMS gives
 * every image a process context. (This file used to stand in for that.)
 */
#include <string.h>
#include <stdint.h>
#include "vms_kif.h"
#include "descrip.h"
#include "uaidef.h"

/*
 * RMS FORCE-BIND ANCHOR. libvms reaches SYSUAF.DAT/RIGHTSLIST.DAT through RMS
 * WEAKLY (rtl/rms_textfile.c, rtl/sysuaf.c -- libvms must not depend on
 * libvmsrms), and a weak reference does not pull an archive member out of
 * libvmsrms.a: without a strong reference somewhere in the link the reader is
 * silently unlinked and $GETUAI/$CREPRC see "no such user". DCL, LOGINOUT and
 * PROVISION each carry such an anchor (src/vmslink/*_rms_bind.c); this is the
 * corpus image's.
 */
extern unsigned int sys$open(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$close(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$connect(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$get(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$put(void *, void (*)(void *), void (*)(void *));
extern unsigned int sys$create(void *, void (*)(void *), void (*)(void *));
extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

__attribute__((used, noinline))
unsigned int corpus_rt_rms_bind_never(void)
{
    static volatile int never = 0;      /* zero at run time; keeps the strong refs */
    if (!never)
        return 0;
    return sys$open(0, 0, 0)  | sys$close(0, 0, 0) | sys$connect(0, 0, 0)
         | sys$get(0, 0, 0)   | sys$put(0, 0, 0)   | sys$create(0, 0, 0)
         | ovmx_sysuaf_read_user(0, 0) | ovmx_sysuaf_read_uic(0, 0);
}

extern unsigned int sys$getuai(unsigned int, void *, void *, void *, void *, void *, void *);
extern unsigned int sys$setddir(void *, unsigned short *, void *);

/* A UAI$_DEFDEV/UAI$_DEFDIR value, either a counted string (length byte first) or
 * the bare text, appended to out. */
static void append_uai_text(char *out, size_t cap, const char *v, unsigned short len)
{
    size_t have = strlen(out);

    if (len > 1 && (unsigned char)v[0] == (unsigned)(len - 1)) {
        v++;
        len--;
    }
    while (len > 0 && (v[len - 1] == '\0' || v[len - 1] == ' '))
        len--;
    if (have + len >= cap)
        return;
    memcpy(out + have, v, len);
    out[have + len] = '\0';
}

__attribute__((constructor))
static void corpus_rt_run_as_system(void)
{
    static char user[] = "SYSTEM";
    struct dsc$descriptor_s ud = { sizeof(user) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, user };
    char dev[64] = "", dir[128] = "", ddir[192] = "";
    unsigned short dl = 0, rl = 0;
    struct { unsigned short len, code; void *buf; unsigned short *ret; } il[3] = {
        { sizeof(dev) - 1, UAI$_DEFDEV, dev, &dl },
        { sizeof(dir) - 1, UAI$_DEFDIR, dir, &rl },
        { 0, 0, 0, 0 },
    };

    (void)vms_kif_establish_system();
    if (!(sys$getuai(0, 0, &ud, il, 0, 0, 0) & 1))
        return;
    append_uai_text(ddir, sizeof(ddir), dev, dl);
    append_uai_text(ddir, sizeof(ddir), dir, rl);
    if (ddir[0]) {
        struct dsc$descriptor_s nd = { (unsigned short)strlen(ddir), DSC$K_DTYPE_T,
                                       DSC$K_CLASS_S, ddir };
        (void)sys$setddir(&nd, 0, 0);
    }
}
