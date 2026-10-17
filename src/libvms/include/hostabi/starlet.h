/*
 * hostabi/starlet.h - <starlet.h> as a DEC C program built FOR the OVMX/Alpha
 * host sees it under __NEW_STARLET (vms-5303e): the system services by their
 * VMS names and the VMS argument forms, so a 32-bit program's descriptors and
 * quadwords mean what they mean on OpenVMS. See hostabi/rms.h for why this
 * directory sits ahead of the sysroot.
 *
 * Time services: SYS$ASCTIM takes the VMS string descriptor (vms/starlet.h);
 * sys$gettim writes the caller's quadword, here typed as DEC C types it (a
 * struct _generic_64 *) -- the same entry point OVMX's own uint64_t * prototype
 * names, the argument being one address either way.
 */
#ifndef __HOSTABI_STARLET_H
#define __HOSTABI_STARLET_H
#include <stdint.h>
#include "../gen64def.h"
#include "../vms/descrip.h"
#include "../vms/starlet.h"

unsigned int sys$gettim(struct _generic_64 *timadr);

#define sys$asctim   SYS$ASCTIM
#define sys$bintim   SYS$BINTIM
#define sys$assign   SYS$ASSIGN
#define sys$dassgn   SYS$DASSGN
#define sys$trnlnm   SYS$TRNLNM
#define sys$crelnm   SYS$CRELNM
#define sys$qio      SYS$QIO
#define sys$qiow     SYS$QIOW
#endif /* __HOSTABI_STARLET_H */
