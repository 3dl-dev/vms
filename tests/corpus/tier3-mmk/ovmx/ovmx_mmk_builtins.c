/*
 * ovmx_mmk_builtins.c - OVMX (vms-ec70) implementations of the VMS/DEC-C
 *                       built-ins and RTL entry points MMK references that the
 *                       OVMX toolchain/RTL does not provide as intrinsics.
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) ***
 *
 *  _INSQUE / _REMQUE  - DEC C generates these for the VAX/x86 INSQUE/REMQUE
 *                       instructions (self-relative doubly-linked queue insert
 *                       / remove).  MMK's queue macros (mmk.h queue_insert /
 *                       queue_remove) call them on the non-Alpha path.  The
 *                       semantics are public (OpenVMS MACRO-32 / architecture
 *                       manuals); implemented here on the standard {flink,blink}
 *                       entry layout every MMK queue structure begins with.
 *
 *  sp_once            - "run one command, collect its output" (parse_descrip.c
 *                       uses it for command-substitution during parsing).  This
 *                       needs the same DCL-subprocess drive that build_target's
 *                       command execution needs (see ovmx_mmk_sp.c); it is NOT
 *                       yet wired, so it returns an honest failure rather than
 *                       faking output.  A description file that uses command
 *                       substitution therefore fails cleanly (deferred gap).
 *
 *  lib$find_image_symbol - dynamic image symbol lookup; MMK uses it only for an
 *                       optional dynamically-loaded feature, off the spine path.
 *                       Honest "not found" so the optional feature is disabled,
 *                       never faked.
 */
#include <stdint.h>
#include <ssdef.h>

#ifndef SS$_UNSUPPORTED
#define SS$_UNSUPPORTED 0x00000924
#endif
#ifndef LIB$_KEYNOTFOU
#define LIB$_KEYNOTFOU  0x00158244   /* even (failure): key not found */
#endif

struct ovmx_q { struct ovmx_q *flink, *blink; };

/* _INSQUE(entry, pred): insert `entry` into the queue immediately after `pred`. */
void _INSQUE(void *entry, void *pred)
{
    struct ovmx_q *e = (struct ovmx_q *)entry;
    struct ovmx_q *p = (struct ovmx_q *)pred;
    e->flink        = p->flink;
    e->blink        = p;
    p->flink->blink = e;
    p->flink        = e;
}

/* _REMQUE(entry, addr): unlink `entry` from its queue; store its address at *addr. */
void _REMQUE(void *entry, void *addr)
{
    struct ovmx_q *e = (struct ovmx_q *)entry;
    e->blink->flink = e->flink;
    e->flink->blink = e->blink;
    if (addr) *(void **)addr = entry;
}

unsigned int sp_once(void *cmd, void *actrtn, void *result)
{
    (void)cmd; (void)actrtn; (void)result;
    return SS$_UNSUPPORTED;   /* command-substitution drive not wired (deferred) */
}

unsigned int lib$find_image_symbol(void *image, void *symbol, void *symval, ...)
{
    (void)image; (void)symbol; (void)symval;
    return LIB$_KEYNOTFOU;
}
