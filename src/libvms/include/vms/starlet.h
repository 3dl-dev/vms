/*
 * vms/starlet.h - system services by their upper-case (VMS-ABI) names
 * (vms-38b). They take the VMS argument forms -- 32-bit (or DSC64) string
 * descriptors (vms/descrip.h), ILE3 (or ILEB_64) item lists -- whatever the
 * caller's pointer size (src/libvms/syssvc/sys_vmsabi.c). OVMX's own code uses
 * the lower-case sys$ API (src/libvms/include/starlet.h). Declared here are
 * the services that exist on the VMS ABI so far.
 */
#ifndef __VMS_STARLET_H
#define __VMS_STARLET_H
#include "vms_abi.h"

__VMS_ABI_EXTERN_C_BEGIN
int SYS$ASSIGN(void *devnam, unsigned short *chan, ...);
int SYS$DASSGN(unsigned short chan);
int SYS$TRNLNM(unsigned int *attr, void *tabnam, void *lognam, unsigned char *acmode, void *itmlst);
int SYS$CRELNM(unsigned int *attr, void *tabnam, void *lognam, unsigned char *acmode, void *itmlst);
__VMS_ABI_EXTERN_C_END

#endif /* __VMS_STARLET_H */
