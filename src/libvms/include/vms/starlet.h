/*
 * vms/starlet.h - system services by their upper-case (VMS-ABI) names
 * (vms-022). These entry points take the VMS argument forms: 32-bit string
 * descriptors (vms/descrip.h), 32-bit item lists and quadword I/O status
 * blocks, whatever the caller's pointer size. OVMX's own code uses the
 * lower-case sys$ API (src/libvms/include/starlet.h) and is unaffected.
 */
#ifndef __VMS_STARLET_H
#define __VMS_STARLET_H
#include "vms_abi.h"

__VMS_ABI_EXTERN_C_BEGIN
int SYS$ASSIGN(void *devnam, unsigned short *chan, ...);
int SYS$DASSGN(unsigned short chan);
int SYS$QIO(unsigned int efn, unsigned short chan, unsigned int func, void *iosb, ...);
int SYS$QIOW(unsigned int efn, unsigned short chan, unsigned int func, void *iosb, ...);
int SYS$TRNLNM(unsigned int *attr, void *tabnam, void *lognam, unsigned char *acmode, void *itmlst);
int SYS$CRELNM(unsigned int *attr, void *tabnam, void *lognam, unsigned char *acmode, void *itmlst);
__VMS_ABI_EXTERN_C_END

#endif /* __VMS_STARLET_H */
