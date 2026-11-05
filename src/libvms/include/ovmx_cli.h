/*
 * ovmx_cli.h - the CLI callback channel (rd vms-cded).
 *
 * On OpenVMS, LIB$SET_SYMBOL / LIB$GET_SYMBOL / LIB$DELETE_SYMBOL called from
 * an image run under DCL act on DCL's OWN symbol tables: the routines call back
 * into the command language interpreter that activated the image, so a symbol
 * an image sets is there when the image exits (OpenVMS RTL Library (LIB$)
 * Manual, LIB$SET_SYMBOL: "the CLI ... must be DCL").
 *
 * OVMX reaches DCL the two ways an image can be activated:
 *   - in-process (imgact_activate in DCL's own process): DCL registers a
 *     handler with lib$$set_cli_handler(); the image's LIBVMS$SHR is DCL's
 *     resident one, so the call goes straight to DCL's symbol code;
 *   - fork()+execve() (a CONTINUE task sharing DCL's VMS PID): DCL creates
 *     two temporary mailboxes, a REQUEST and a REPLY mailbox, and names them
 *     to the image by OVMX$CLI_MBX ("MBAn:,MBAm:") in its environment. The
 *     image $ASSIGNs them and writes one request message, then reads one
 *     reply (rd vms-6d9). The request mailbox is also the image task's
 *     TERMINATION MAILBOX ($CREPRC mbxunt), so DCL's whole wait for the image
 *     is one blocking mailbox read: a CLI request, the executive's
 *     termination message, or the executive's CTRL/Y message (struct
 *     vms_exec_mbxmsg) -- no host descriptor, no poll. With no executive
 *     there is no mailbox: a named CLI that cannot be reached answers
 *     LIB$_NOCLI.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): the wire layout is OVMX's own; VMS
 * documents the behaviour (the symbol lands in DCL's table), not a format.
 */
#ifndef OVMX_CLI_H
#define OVMX_CLI_H

#include <stdint.h>

#define OVMX_CLI_MBX_ENV    "OVMX$CLI_MBX"

#define OVMX_CLI_OP_SET     1
#define OVMX_CLI_OP_GET     2
#define OVMX_CLI_OP_DELETE  3
/* LIB$DO_COMMAND: `value` is the command (<= 255 chars). The CLI keeps it and
 * executes it once the image has exited (the caller then calls SYS$EXIT). */
#define OVMX_CLI_OP_DO_COMMAND 4

#define OVMX_CLI_NAME_MAX   255
#define OVMX_CLI_VALUE_MAX  1024

/* table: 1 = local (LIB$K_CLI_LOCAL_SYM), 2 = global (LIB$K_CLI_GLOBAL_SYM);
 * 0 on a GET = "local, then global" (the reply says which). */
struct ovmx_cli_req {
    uint32_t op;
    uint32_t table;
    uint16_t name_len;
    uint16_t value_len;
    char     name[OVMX_CLI_NAME_MAX + 1];
    char     value[OVMX_CLI_VALUE_MAX];
};

struct ovmx_cli_rsp {
    uint32_t status;            /* SS$_NORMAL or LIB$_NOSUCHSYM etc. */
    uint32_t table;             /* GET: the table the symbol was found in */
    uint16_t value_len;
    uint16_t pad;
    char     value[OVMX_CLI_VALUE_MAX];
};

/* The in-process CLI: fills *rsp for *req. */
typedef void (*ovmx_cli_handler_t)(const struct ovmx_cli_req *req,
                                   struct ovmx_cli_rsp *rsp);

/* Registered by DCL (LIBVMS$SHR); NULL clears it. */
void lib$$set_cli_handler(ovmx_cli_handler_t handler);

#endif /* OVMX_CLI_H */
