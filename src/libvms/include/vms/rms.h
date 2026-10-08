/*
 * vms/rms.h - the RMS control blocks and services in the VMS layout
 * (vms-022): FAB, NAM, RAB, their cc$rms_ initializers, and the RMS services
 * by their upper-case (VMS-ABI) names, which take these layouts
 * (src/vmsrms/rms_vmsabi.c). Declared here are the services that exist:
 * SYS$PARSE and SYS$SEARCH; the rest follow on vms-692.
 */
#ifndef __VMS_RMS_H
#define __VMS_RMS_H
#include "fabdef.h"
#include "namdef.h"
#include "rabdef.h"

#define cc$rms_fab ((struct fabdef){ .fab$b_bid = FAB$C_BID, .fab$b_bln = FAB$C_BLN, \
        .fab$l_fop = 0, .fab$b_fac = FAB$M_GET, .fab$b_org = FAB$C_SEQ, \
        .fab$b_rat = 0, .fab$b_rfm = FAB$C_VAR })
#define cc$rms_nam ((struct namdef){ .nam$b_bid = NAM$C_BID, .nam$b_bln = NAM$C_BLN })
#define cc$rms_rab ((struct rabdef){ .rab$b_bid = RAB$C_BID, .rab$b_bln = RAB$C_BLN, \
        .rab$b_rac = RAB$C_SEQ })

__VMS_ABI_EXTERN_C_BEGIN

int SYS$PARSE(void *fab, ...);
int SYS$SEARCH(void *fab, ...);

__VMS_ABI_EXTERN_C_END

#endif /* __VMS_RMS_H */
