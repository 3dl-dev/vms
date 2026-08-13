/*
 * ovmx_mmk_sp.c - OVMX (vms-ec70) subprocess-manager companion for MMK.
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) + DEFERRED-GAP FLAG ***
 *
 * MMK drives the compiler/linker by feeding resolved DCL command lines to a
 * PERSISTENT DCL SUBPROCESS over a VMS MAILBOX, using a write-attention AST +
 * $HIBER/$WAKE to know when each command finished and to capture $STATUS
 * (build_target.c's send_cmd_and_wait / echo_ast protocol; the end-of-command
 * markers MMK___WRITE "MMK____status=...").  Faithfully reproducing that needs
 * OVMX executive support for lib$spawn-of-DCL + mailboxes + write-attention
 * ASTs, which is NOT yet wired.  Rather than FAKE command execution (which would
 * be an INV-6 / Rule 9 facade — reporting success while nothing ran), this
 * companion is HONEST about the boundary:
 *
 *   - MMK's /NOACTION (dry-run) mode — which by design parses the description
 *     file, resolves all rules/symbols/dependencies, and PRINTS the exact
 *     commands it WOULD run without executing anything — is fully supported.
 *     The self-host spine's parse proof runs in this mode (see run_mmk_native.sh
 *     / test_mmk_parse_descrip).
 *   - A REAL build (executing commands) returns an honest failure status so MMK
 *     reports "cannot execute" and stops — it never pretends a build happened.
 *
 * Wiring the real DCL-subprocess drive (turning /NOACTION into a real build that
 * invokes TCC.EXE / LINK.EXE / LIBRARIAN.EXE) is the remaining spine-#4 work.
 *
 * *** vms-b23 UPDATE — the mailbox+AST drive is BLOCKED, not "just unwired". ***
 *
 * Attempting to wire build_target.c's send_cmd_and_wait to the three named
 * facilities (vms-98c lib$spawn, vms-e0b mailbox IPC, vms-9003 write-attention
 * AST) surfaced THREE further executive/DCL gaps the prereqs proved a PRIMITIVE
 * for but never proved IN COMPOSITION.  MMK needs: a persistent DCL over two
 * mailboxes + an ASYNC write-attention AST that interrupts $HIBER + a
 * NON-BLOCKING mailbox drain.  OVMX today has none of those three:
 *
 *   1. AST delivery is a poll-drain on sys$setast(1) (sys_ast.c), never an
 *      interrupt of a hibernating process (sys_process.c sys$hiber == pause();
 *      vms_ast.c names signal delivery a "future enhancement") -> command_complete
 *      is never set, the wait deadlocks.
 *   2. Mailbox reads always block; IO$M_NOW is dropped (sys_qio.c qio_mailbox_op,
 *      vms_kif.c vms_kif_mbx_read) -> echo_ast's non-blocking drain cannot end.
 *   3. DCL reads/writes fd-based stdio (dcl_main.c) but OVMX mailboxes have NO fd
 *      (sys_mailbox.c fd = -1) -> a spawned DCL cannot use a mailbox as
 *      SYS$INPUT/SYS$OUTPUT without a bridge.
 *
 * The full analysis, the VMS-faithful design (A, blocked on the above executive
 * work) and the achievable interim synchronous-.COM-batch drive (B, host-testable,
 * a disclosed transport deviation the operator owns per Rule 5) are in
 * docs/design-mmk-exec-drive-ovmx.md.  Until that decision lands this companion
 * stays HONEST (SS$_UNSUPPORTED outside /NOACTION) rather than fake a build over
 * facilities the executive cannot yet deliver (Rule 9 / INV-6).
 */
#include <stdint.h>
#include <stdio.h>
#include <ssdef.h>

typedef void *SPHANDLE;

/* MMK global: nonzero when running /NOACTION (dry run — nothing is executed). */
extern int noaction;

#ifndef SS$_UNSUPPORTED
#define SS$_UNSUPPORTED 0x00000924   /* even (failure) */
#endif

/* A single non-null sentinel handle: in dry-run there is no real subprocess,
 * but MMK only checks the handle for non-zero (spctx == 0 => "not yet open"). */
static int ovmx_sp_sentinel;

unsigned int sp_open(SPHANDLE *ctxpp, void *inicmd,
                     unsigned int (*rcvast)(void *), void *rcvastprm)
{
    (void)inicmd; (void)rcvast; (void)rcvastprm;
    if (!noaction) {
        /* Honest: we cannot spawn a real DCL subprocess yet. */
        fprintf(stderr,
            "%%MMK-E-NOSPAWN, OVMX build does not yet wire the DCL-subprocess "
            "drive; re-run with /ACTION=NOACTION to see the build plan "
            "(vms-b23: blocked on async AST delivery + non-blocking mailbox read "
            "+ DCL-over-mailbox; see docs/design-mmk-exec-drive-ovmx.md)\n");
        return SS$_UNSUPPORTED;
    }
    if (ctxpp) *ctxpp = &ovmx_sp_sentinel;
    return SS$_NORMAL;
}

/* In dry-run the only sends are the subprocess-setup commands (SET NOON, symbol
 * definitions); accepting them has no observable effect because /NOACTION runs
 * nothing.  Never reached for a real build (sp_open already failed above). */
unsigned int sp_send(SPHANDLE *ctxpp, void *cmdstr)
{
    (void)ctxpp; (void)cmdstr;
    return noaction ? SS$_NORMAL : SS$_UNSUPPORTED;
}

unsigned int sp_receive(SPHANDLE *ctxpp, void *rcvstr, int *rcvlen)
{
    (void)ctxpp; (void)rcvstr;
    if (rcvlen) *rcvlen = 0;
    return SS$_NORMAL;
}

unsigned int sp_close(SPHANDLE *ctxpp)          { (void)ctxpp; return SS$_NORMAL; }
unsigned int sp_show_subprocess(SPHANDLE ctx)   { (void)ctx;   return SS$_NORMAL; }

/* NB: set_ctrlt_ast / clear_ctrlt_ast are provided by MMK's own misc.c. */
