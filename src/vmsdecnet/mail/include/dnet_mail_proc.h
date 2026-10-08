/*
 * dnet_mail_proc.h - the MAIL-11 NETWORK SERVER PROCESS (rd vms-47fd): how
 * NETACP serves an inbound object-27 connect.
 *
 * THE VMS SHAPE. A VMS node's MAIL object runs SYS$SYSTEM:MAIL_SERVER.EXE in a
 * network job under the MAIL object's account (MAIL$SERVER; oracle
 * docs/oracle/vax-ncp-show/MCR_NCP_SHOW_KNOWN_OBJECTS.txt) or, with none, the
 * executor's default nonprivileged account; with neither the connect is refused
 * (the oracle VAX's INVLOGIN sessions). The image writes any user's mail file
 * because it is installed with SYSPRV, not because its account is privileged.
 *
 * OVMX (LABELLED equivalent): NETACP $CREPRCs SYS$SYSTEM:MAIL_SERVER.EXE over
 * the FAL server-process mailbox link (dnet_fal_proc.h, dnet_netsrv_*) with the
 * UIC + default privileges of SYSUAF MAIL$SERVER, else of SYSUAF DEFAULT (OVMX
 * has no NCP executor NONPRIVILEGED USER; DEFAULT is the nonprivileged account
 * template it ships), plus SYSPRV -- the installed-image privilege, so system
 * protection admits it to a recipient's mail file while a protocol fault can
 * never act as SYSTEM. Neither account, or no MAIL_SERVER.EXE: the connect is
 * refused and nothing is acknowledged (INV-6).
 */
#ifndef DNET_MAIL_PROC_H
#define DNET_MAIL_PROC_H

#include <stdint.h>

#include "dnet_fal_proc.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DNET_MAILP_IMAGE_SPEC  "SYS$SYSTEM:MAIL_SERVER.EXE"
#define DNET_MAILP_PRCNAM_FMT  "MAIL_%u_%u"

/* Is SYS$SYSTEM:MAIL_SERVER.EXE runnable on this system disk? 1 / 0. */
int dnet_mail_proc_image_present(void);

/*
 * dnet_mail_proc_start - create the MAIL server for one accepted connect from
 * `remote_node` (node name, or decimal address) to `local_node`. Returns
 * SS$_NORMAL; SS$_INVLOGIN when SYSUAF holds neither MAIL$SERVER nor DEFAULT;
 * or the failing service's status (SS$_NOSUCHFILE: no image).
 */
uint32_t dnet_mail_proc_start(struct dnet_fal_proc *p, const char *remote_node,
                              const char *local_node);

#ifdef __cplusplus
}
#endif

#endif /* DNET_MAIL_PROC_H */
