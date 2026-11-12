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
 *  __PAL_INSQUEL / __PAL_REMQUEL - the Alpha forms of the same operations,
 *                       which MMK's queue macros use when __ALPHA is defined
 *                       (the alpha-dec-vms port compiler).  Longword queues:
 *                       the links are 32-bit, as are the pointers of an image
 *                       built with the compiler's default pointer size.  Results
 *                       per the Alpha architecture's INSQUEL/REMQUEL:
 *                       INSQUEL 1 if the queue was empty before the insert, else 0;
 *                       REMQUEL -1 if the queue was empty (nothing removed),
 *                       0 if the removal emptied it, else 1.
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

#if defined(__ALPHA) || defined(__alpha)
int __PAL_INSQUEL(void *predecessor, void *new_entry)
{
    struct ovmx_q *p = (struct ovmx_q *)predecessor;
    int was_empty = (p->flink == p);
    _INSQUE(new_entry, predecessor);
    return was_empty;
}

int __PAL_REMQUEL(void *entry, void *removed_entry)
{
    struct ovmx_q *e = (struct ovmx_q *)entry;
    if (e->flink == e)
        return -1;                     /* entry is an empty header: nothing removed */
    int emptied = (e->flink == e->blink);
    _REMQUE(entry, removed_entry);
    return emptied ? 0 : 1;
}
#endif

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
