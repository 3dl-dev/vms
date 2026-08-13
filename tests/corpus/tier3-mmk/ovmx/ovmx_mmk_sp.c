/*
 * ovmx_mmk_sp.c - OVMX (vms-ec70 / vms-b23) subprocess-manager companion for
 * MMK: the VMS-faithful DCL-subprocess drive (design A).
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) ***
 *
 * MadGoat MMK drives the compiler/linker by feeding resolved DCL command lines
 * to a PERSISTENT DCL SUBPROCESS over two VMS MAILBOXES, using a write-attention
 * AST + $HIBER/$WAKE to learn when each command finished and to capture $STATUS
 * from an end-of-command marker (build_target.c's send_cmd_and_wait / echo_ast
 * protocol; the marker line  MMK___WRITE MMK___OUTPUT "MMK____status=",...).
 * MMK's own sp_mgr.c is the reference for this boundary (it is NOT compiled on
 * OVMX -- this file replaces it); every routine below is our own clean-room
 * reimplementation over the OVMX public sys$/lib$ API, derived from MMK's public
 * sp_mgr.c/build_target.c and the OpenVMS System Services / I/O User's Reference.
 * No VSI/HPE/DEC source or byte layout is used.
 *
 * *** vms-b23: design A is now UNBLOCKED. ***
 *
 * The three executive/DCL facilities the mailbox+AST drive composes all landed:
 *   - vms-98c   lib$spawn spawns a REAL DCL subprocess (/NOWAIT);
 *   - vms-e0b   mailbox IPC (sys$crembx + IO$_READVBLK/WRITEVBLK $QIO);
 *   - vms-9003  cross-process mailbox WRITE-ATTENTION AST
 *               (IO$_SETMODE|IO$M_WRTATTN);
 *   - vms-feb   asynchronous AST delivery -- a queued write-attention AST
 *               INTERRUPTS a hibernating process's $HIBER;
 *   - vms-5df   IO$M_NOW non-blocking mailbox read (empty -> SS$_ENDOFFILE);
 *   - vms-786   a spawned DCL reads SYS$INPUT / writes SYS$OUTPUT over a mailbox
 *               (src/vmsdcl/dcl_mbx.c).
 * (See docs/design-mmk-exec-drive-ovmx.md for the analysis these closed.)
 *
 * HOW THE PIECES COMPOSE (the shape build_target.c drives, unmodified):
 *
 *   sp_open   - $CREMBX two mailboxes and PUBLISH their names as the logical
 *               names SYS$INPUT (commands in) and SYS$OUTPUT (results out); arm a
 *               write-attention AST on the SYS$OUTPUT mailbox; lib$spawn a
 *               persistent DCL (CLI$M_NOWAIT).  The spawned DCL translates
 *               SYS$INPUT/SYS$OUTPUT to those mailboxes and binds them
 *               (dcl_mbx.c) -- exactly the rendezvous vms-786 proved.  Then feed
 *               MMK's initial (.FIRST-setup) command line.
 *   sp_send   - $QIO IO$_WRITEVBLK one command record into the SYS$INPUT mailbox;
 *               the DCL reads it as one command line.
 *   sp_receive- $QIO IO$_READVBLK|IO$M_NOW one result record from the SYS$OUTPUT
 *               mailbox; empty returns an error (SS$_ENDOFFILE) so MMK's echo_ast
 *               drain loop terminates instead of blocking (the vms-5df point).
 *   (the wait) - build_target.c's send_cmd_and_wait sp_sends the command + the
 *               three end-of-command-marker commands, then  do { sys$hiber(); }
 *               while (!command_complete).  When the DCL writes output, our
 *               write-attention trampoline fires (vms-9003), INTERRUPTING the
 *               $HIBER (vms-feb), and calls MMK's echo_ast, which drains with
 *               sp_receive, echoes output, and on the "MMK____status=<hex>"
 *               marker sets command_complete + sys$wake -- ending the wait with
 *               the command's real $STATUS.
 *   sp_close  - $FORCEX + $DELPRC the DCL and $DASSGN the mailboxes.
 *
 * SINGLE SUBPROCESS.  MMK keeps exactly ONE persistent DCL (build_target.c's
 * `static SPHANDLE spctx`).  We therefore hold the context in a single file
 * static and reach it from the write-attention AST directly: the executive
 * delivers a write-attention AST's parameter as a longword (P2), which cannot
 * carry a 64-bit context pointer, so the trampoline uses the file static rather
 * than the (truncated) AST parameter.  This mirrors MMK's own one-subprocess
 * assumption; it is an OVMX design choice (Rule 8).
 *
 * /NOACTION (dry run) is unchanged: MMK resolves the plan and PRINTS the
 * commands it WOULD run without executing anything, so there is no subprocess to
 * open -- sp_open returns a sentinel handle and the sends/receives are no-ops
 * (the toolchain-mmk-parse ctest runs entirely in this mode).
 *
 * HONEST FAILURE (Rule 9 / INV-6).  Reaching a mailbox goes through the
 * executive; with no /dev/vms $CREMBX fails SS$_NOSUCHDEV and sp_open returns
 * that status -- MMK signals it and stops.  Nothing is faked: no per-process
 * mailbox, no pretended build, no fabricated $STATUS.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <ssdef.h>
#include <descrip.h>
#include <starlet.h>
#include <iodef.h>
#include <iosbdef.h>
#include <str$routines.h>
#include <libclidef.h>
#include <vms/pcb.h>

typedef void *SPHANDLE;

/* Forward declarations (sp_open feeds the initial command via sp_send). */
unsigned int sp_open(SPHANDLE *ctxpp, void *inicmd,
                     unsigned int (*rcvast)(void *), void *rcvastprm);
unsigned int sp_send(SPHANDLE *ctxpp, void *cmdstr);
unsigned int sp_receive(SPHANDLE *ctxpp, void *rcvstr, int *rcvlen);
unsigned int sp_close(SPHANDLE *ctxpp);
unsigned int sp_show_subprocess(SPHANDLE ctx);

/* MMK global: nonzero when running /NOACTION (dry run -- nothing is executed). */
extern int noaction;

#ifndef SS$_UNSUPPORTED
#define SS$_UNSUPPORTED 0x00000924   /* even (failure) */
#endif

/* The two logical names the parent publishes for the spawned DCL to translate
 * and bind (dcl_mbx.c binds exactly these when they name a mailbox). */
#define SP_SYSINPUT   "SYS$INPUT"
#define SP_SYSOUTPUT  "SYS$OUTPUT"

/* One mailbox record cap -- matches the executive's per-ioctl limit and DCL's
 * command-line length (dcl_mbx.c DCL_MBX_BUF). */
#define SP_MBX_BUF 4096

/*
 * The single persistent-DCL context.  MMK holds exactly one subprocess, so a
 * file static is both sufficient and necessary (the write-attention AST reaches
 * it here rather than through the pointer-truncating AST parameter).
 */
struct SPB {
    uint16_t in_chan;    /* SYS$INPUT  command mailbox (parent writes) */
    uint16_t out_chan;   /* SYS$OUTPUT result  mailbox (parent reads)  */
    uint32_t pid;        /* the spawned DCL's process id */
    int      spawned;    /* a real DCL was created */
    unsigned int (*rcvast)(void *);  /* MMK's echo_ast */
    void        *astprm;             /* MMK's rcvast parameter (0) */
};

static struct SPB g_spb;
static int        ovmx_sp_sentinel;   /* non-null dry-run handle */

static struct dsc$descriptor_s sp_mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

/*
 * sp_wrtattn_trampoline - the write-attention AST the executive delivers when
 * the DCL writes its SYS$OUTPUT mailbox (vms-9003), which INTERRUPTS MMK's
 * $HIBER (vms-feb).  Re-arm the one-shot FIRST (so no subsequent cross-process
 * write is missed while echo_ast drains), then call MMK's receive AST (echo_ast)
 * which drains the mailbox and, on the end-of-command marker, wakes the main
 * line.  Delivered as void(uint32_t) -- the parameter is unused; the single
 * subprocess is g_spb.
 */
static void sp_wrtattn_trampoline(uint32_t prm)
{
    (void)prm;
    struct _iosb iosb;
    memset(&iosb, 0, sizeof(iosb));
    (void)sys$qiow(0, g_spb.out_chan, IO$_SETMODE | IO$M_WRTATTN, &iosb,
                   NULL, 0, (void *)sp_wrtattn_trampoline, 0, 0, 0, 0, 0);
    if (g_spb.rcvast)
        (void)g_spb.rcvast(g_spb.astprm);
}

/*
 * sp_open - open the persistent DCL subprocess (design A).  In /NOACTION there
 * is nothing to run, so hand back a sentinel handle and do not spawn.
 */
unsigned int sp_open(SPHANDLE *ctxpp, void *inicmd,
                     unsigned int (*rcvast)(void *), void *rcvastprm)
{
    if (noaction) {
        if (ctxpp) *ctxpp = &ovmx_sp_sentinel;
        return SS$_NORMAL;
    }

    /* Ensure this image has a per-process PCB: every sys$ channel service
     * (crembx/qio/assign) keys off it (sys_mailbox.c returns SS$_BADPARAM with
     * no PCB), and unlike DCL.EXE -- which seeds one at startup (dcl_main.c) --
     * the vendored MMK main never does. Seed it here, the first point MMK reaches
     * the executive. Privileges are left empty: the real executive on /dev/vms
     * governs what crembx/spawn may do; the userspace PCB is only the channel
     * table. Idempotent (guarded on vms_pcb_get). */
    if (!vms_pcb_get()) {
        if (!vms_pcb_init(0))
            return SS$_INSFMEM;
    }

    memset(&g_spb, 0, sizeof(g_spb));
    g_spb.rcvast = rcvast;
    g_spb.astprm = rcvastprm;

    /* Create the two mailboxes, publishing them as SYS$INPUT / SYS$OUTPUT so the
     * spawned DCL translates and binds them (vms-786 rendezvous).  On no
     * executive $CREMBX fails SS$_NOSUCHDEV -- returned honestly (Rule 9). */
    struct dsc$descriptor_s indsc  = sp_mkdsc(SP_SYSINPUT);
    struct dsc$descriptor_s outdsc = sp_mkdsc(SP_SYSOUTPUT);

    uint32_t st = sys$crembx(0, &g_spb.in_chan, 1024, 1024, 0, 0, &indsc);
    if (!(st & 1))
        return st;
    st = sys$crembx(0, &g_spb.out_chan, 1024, 1024, 0, 0, &outdsc);
    if (!(st & 1)) {
        (void)sys$dassgn(g_spb.in_chan);
        g_spb.in_chan = 0;
        return st;
    }

    /* Arm the write-attention AST on the result mailbox BEFORE the DCL can
     * write, so the first output record notifies us (vms-9003). */
    struct _iosb iosb;
    memset(&iosb, 0, sizeof(iosb));
    st = sys$qiow(0, g_spb.out_chan, IO$_SETMODE | IO$M_WRTATTN, &iosb,
                  NULL, 0, (void *)sp_wrtattn_trampoline, 0, 0, 0, 0, 0);
    if (!(st & 1)) {
        (void)sys$dassgn(g_spb.out_chan);
        (void)sys$dassgn(g_spb.in_chan);
        g_spb.in_chan = g_spb.out_chan = 0;
        return st;
    }

    /* Spawn the persistent DCL (CLI$M_NOWAIT).  No command / input / output
     * arguments: the DCL takes its SYS$INPUT / SYS$OUTPUT from the logical names
     * we just published (the mailboxes), not from a redirected file. */
    uint32_t flags = CLI$M_NOWAIT;
    uint32_t pid = 0;
    st = lib$spawn(NULL, NULL, NULL, &flags, NULL, &pid,
                   NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    if (!(st & 1)) {
        (void)sys$dassgn(g_spb.out_chan);
        (void)sys$dassgn(g_spb.in_chan);
        g_spb.in_chan = g_spb.out_chan = 0;
        return st;
    }
    g_spb.pid     = pid;
    g_spb.spawned = 1;

    if (ctxpp) *ctxpp = &g_spb;

    /* Feed MMK's initial (.FIRST setup) command line, if any. */
    if (inicmd)
        (void)sp_send(ctxpp, inicmd);

    return SS$_NORMAL;
}

/*
 * sp_send - write one command record into the SYS$INPUT mailbox; the DCL reads
 * it as one command line (dcl_mbx.c terminates each record with a newline).
 */
unsigned int sp_send(SPHANDLE *ctxpp, void *cmdstr)
{
    struct SPB *ctx = ctxpp ? (struct SPB *)*ctxpp : &g_spb;
    if (noaction || !ctx || ctx == (struct SPB *)&ovmx_sp_sentinel || !ctx->spawned)
        return SS$_NORMAL;

    /* CLASS_S and CLASS_D descriptors share the leading length+pointer fields. */
    struct dsc$descriptor_s *d = (struct dsc$descriptor_s *)cmdstr;
    uint16_t len = d->dsc$w_length;
    char    *adr = d->dsc$a_pointer;
    if (adr == NULL)
        return SS$_BADPARAM;

    struct _iosb iosb;
    memset(&iosb, 0, sizeof(iosb));
    uint32_t st = sys$qiow(0, ctx->in_chan, IO$_WRITEVBLK, &iosb, NULL, 0,
                           adr, len, 0, 0, 0, 0);
    if (st & 1) st = iosb.iosb$w_status;
    return st;
}

/*
 * sp_receive - non-blocking read of one result record from the SYS$OUTPUT
 * mailbox (IO$M_NOW, vms-5df).  An empty mailbox returns SS$_ENDOFFILE so MMK's
 * echo_ast `while (OK(sp_receive(...)))` drain loop ends rather than blocking.
 */
unsigned int sp_receive(SPHANDLE *ctxpp, void *rcvstr, int *rcvlen)
{
    struct SPB *ctx = ctxpp ? (struct SPB *)*ctxpp : &g_spb;
    if (noaction || !ctx || ctx == (struct SPB *)&ovmx_sp_sentinel || !ctx->spawned) {
        if (rcvlen) *rcvlen = 0;
        return SS$_ENDOFFILE;
    }

    static char buf[SP_MBX_BUF];
    struct _iosb iosb;
    memset(&iosb, 0, sizeof(iosb));
    uint32_t st = sys$qiow(0, ctx->out_chan, IO$_READVBLK | IO$M_NOW, &iosb,
                           NULL, 0, buf, sizeof(buf), 0, 0, 0, 0);
    if (st & 1) st = iosb.iosb$w_status;
    if (st & 1) {
        uint16_t n = iosb.iosb$w_bcnt;
        /* A record carrying a trailing NUL (a write of strlen+1) should not
         * push that NUL into MMK's output/marker comparison. */
        if (n > 0 && buf[n - 1] == '\0')
            n--;
        (void)str$copy_r((struct dsc$descriptor_s *)rcvstr, &n, buf);
        if (rcvlen) *rcvlen = n;
    } else if (rcvlen) {
        *rcvlen = 0;
    }
    return st;
}

/*
 * sp_close - tear the subprocess down: force it to exit, delete it, deassign the
 * mailboxes (the published logical names go with the temporary mailboxes).
 */
unsigned int sp_close(SPHANDLE *ctxpp)
{
    struct SPB *ctx = ctxpp ? (struct SPB *)*ctxpp : &g_spb;
    if (noaction || !ctx || ctx == (struct SPB *)&ovmx_sp_sentinel)
        return SS$_NORMAL;

    if (ctx->spawned && ctx->pid) {
        uint32_t pid = ctx->pid;
        (void)sys$forcex(&pid, NULL, SS$_NORMAL);
        (void)sys$delprc(&pid, NULL);
    }
    if (ctx->out_chan) (void)sys$dassgn(ctx->out_chan);
    if (ctx->in_chan)  (void)sys$dassgn(ctx->in_chan);
    ctx->in_chan = ctx->out_chan = 0;
    ctx->spawned = 0;
    ctx->pid = 0;
    return SS$_NORMAL;
}

unsigned int sp_show_subprocess(SPHANDLE ctx)   { (void)ctx;   return SS$_NORMAL; }

/* NB: set_ctrlt_ast / clear_ctrlt_ast are provided by MMK's own misc.c. */
