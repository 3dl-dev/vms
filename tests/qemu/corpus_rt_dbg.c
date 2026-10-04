/* TEMPORARY diagnostic (vms-44a): prints what the executive/RMS answer in the corpus guest. */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include "starlet.h"
#include "descrip.h"
#include "rms.h"
#include "ssdef.h"
#include "sysuaf.h"
#include "vms_kif.h"
#include "vms/logical.h"
#include "uaidef.h"
#include "jpidef.h"
#include "lib$routines.h"

static void tr(const char *n)
{
    char eq[256];
    int r = vms_kif_lnm_translate(VMS_LNM_TBL_SYSTEM, n, 0, eq, sizeof eq, NULL, NULL, NULL);
    printf("DBG tr %s r=%d '%s'\n", n, r, r > 0 ? eq : "");
}

static void asg(const char *n)
{
    struct dsc$descriptor_s d = { (unsigned short)strlen(n), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)n };
    uint16_t ch = 0;
    uint32_t st = sys$assign(&d, &ch, 0, 0, 0);
    printf("DBG assign %s st=%u ch=%u\n", n, st, ch);
    if (st & 1) sys$dassgn(ch);
}

int main(void)
{
    sysuaf_record_t rec;
    tr("SYS$SYSDEVICE"); tr("SYS$SYSROOT"); tr("SYS$SYSTEM"); tr("SYS$COMMON");
    printf("DBG lookup DEFAULT=%d SYSTEM=%d\n", sysuaf_lookup("DEFAULT", &rec), sysuaf_lookup("SYSTEM", &rec));
    {
        struct FAB fab = cc$rms_fab;
        const char *fn = "SYS$SYSTEM:SYSUAF.DAT";
        fab.fab$l_fna = (char *)fn; fab.fab$b_fns = (uint8_t)strlen(fn);
        fab.fab$b_fac = FAB$M_GET;
        uint32_t st = sys$open(&fab, 0, 0);
        printf("DBG open %s st=%u stv=%u\n", fn, st, fab.fab$l_stv);
        if (st & 1) sys$close(&fab, 0, 0);
    }
    asg("SYS$SYSDEVICE"); asg("SYS$SYSDEVICE:"); asg("VDA300:"); asg("VDA0:"); asg("TT:");
    {
        struct FAB fab = cc$rms_fab;
        const char *fn = "DBGEXT.TMP";
        fab.fab$l_fna = (char *)fn; fab.fab$b_fns = (uint8_t)strlen(fn);
        fab.fab$b_fac = FAB$M_GET | FAB$M_PUT;
        fab.fab$w_mrs = 512; fab.fab$b_rfm = FAB$C_FIX;
        uint32_t st = sys$create(&fab, 0, 0);
        printf("DBG create st=%u stv=%u sts=%u\n", st, fab.fab$l_stv, fab.fab$l_sts);
        if (st & 1) {
            fab.fab$l_alq = 100;
            st = sys$extend(&fab, 0, 0);
            printf("DBG extend st=%u stv=%u\n", st, fab.fab$l_stv);
            sys$close(&fab, 0, 0);
        }
    }
    {
        static char dev[1+31], dir[1+63]; static unsigned flags;
        static ILE3 it[] = { {63, UAI$_DEFDIR, dir, NULL}, {31, UAI$_DEFDEV, dev, NULL}, {4, UAI$_FLAGS, &flags, NULL}, {0,0,NULL,NULL} };
        const char *u[] = { "DEFAULT", "SYSTEM" };
        for (int k = 0; k < 2; k++) {
            struct dsc$descriptor_s d = { (unsigned short)strlen(u[k]), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)u[k] };
            uint32_t st = sys$getuai(0, 0, &d, it, 0, 0, 0);
            printf("DBG getuai %s st=%u\n", u[k], st);
        }
        char un[16]; struct dsc$descriptor_s ud = { sizeof(un)-1, DSC$K_DTYPE_T, DSC$K_CLASS_S, un };
        memset(un, ' ', sizeof un);
        uint32_t code = JPI$_USERNAME;
        uint32_t st = lib$getjpi(&code, 0, 0, 0, &ud, &ud.dsc$w_length);
        printf("DBG getjpi username st=%u '%.*s'\n", st, ud.dsc$w_length, un);
    }
    return 1;
}
