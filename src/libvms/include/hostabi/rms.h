/*
 * hostabi/rms.h - <rms.h> as a DEC C program built FOR the OVMX/Alpha host
 * sees it (vms-5303e). On OpenVMS <rms.h> is the VMS-layout RMS control-block
 * header: struct FAB/NAM/RAB, the cc$rms_ initializers, and the RMS services
 * under their VMS names (SYS$OPEN ...), which DEC C's default /NAMES=UPPERCASE
 * lets a program also spell sys$open. binutils' bfd/vms-misc.c is such a
 * program: it includes <rms.h> and calls sys$open / sys$modify / sys$close.
 *
 * OVMX's own <rms.h> (src/libvms/include/rms.h) is the native-code header for
 * OVMX's internal FAB, a different layout that the VMS-ABI RMS entry points
 * (src/vmsrms/rms_vmsabi.c) do not take. A program compiled for the host puts
 * this directory ahead of the sysroot on its include path (-I) and so gets the
 * VMS layout under the name the port source includes -- the source is unchanged.
 */
#ifndef __HOSTABI_RMS_H
#define __HOSTABI_RMS_H
#include "../vms/rms.h"

#define sys$parse       SYS$PARSE
#define sys$search      SYS$SEARCH
#define sys$open        SYS$OPEN
#define sys$create      SYS$CREATE
#define sys$erase       SYS$ERASE
#define sys$close       SYS$CLOSE
#define sys$modify      SYS$MODIFY
#define sys$connect     SYS$CONNECT
#define sys$disconnect  SYS$DISCONNECT
#define sys$get         SYS$GET
#define sys$put         SYS$PUT
#define sys$find        SYS$FIND
#define sys$update      SYS$UPDATE
#define sys$delete      SYS$DELETE
#define sys$rewind      SYS$REWIND
#define sys$flush       SYS$FLUSH

/* RME$C_ is the escape-function code space carried in fab$l_ctx under
 * FAB$M_ESC; bfd/vms-misc.c defines the one value it uses itself. */
#endif /* __HOSTABI_RMS_H */
