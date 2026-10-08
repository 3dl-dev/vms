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
 *   - fork()+execve() (a REGISTER_CONTINUE child sharing DCL's VMS PID): DCL
 *     hands the child one end of a socketpair, named by OVMX$CLI_FD in its
 *     environment, and services requests on the other end while it waits for
 *     the image -- the request/response below, one SOCK_SEQPACKET datagram
 *     each way.
 *
 * OVMX DESIGN CHOICE (CLAUDE.md Rule 8): the wire layout is OVMX's own; VMS
 * documents the behaviour (the symbol lands in DCL's table), not a format.
 */
#ifndef OVMX_CLI_H
#define OVMX_CLI_H

#include <stdint.h>

#define OVMX_CLI_FD_ENV     "OVMX$CLI_FD"

#define OVMX_CLI_OP_SET     1
#define OVMX_CLI_OP_GET     2
#define OVMX_CLI_OP_DELETE  3

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
