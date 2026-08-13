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
 * invokes TCC.EXE / LINK.EXE / LIBRARIAN.EXE) is the remaining spine-#4 work,
 * filed as a follow-up (see the vms-ec70 PR body).
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
            "(vms-ec70 deferred gap)\n");
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
