/* SYS$GL_CALL_HANDL + the condition-dispatcher frame on OVMX/Alpha (vms-bfd03).
 *
 * main -> f -> g, and g signals. Inside the handler the test walks its own
 * invocation chain with the genuine LIB$GET_CURR/PREV_INVO_CONTEXT, exactly as
 * libgcc's VMS unwinder does when an exception crosses a condition handler:
 *   1. the handler's caller is a condition-dispatcher frame: its procedure
 *      value (0(FP)) equals SYS$GL_CALL_HANDL, and its ICB carries a CHF
 *      context (libicb$ph_chfctx_addr) whose exception FP is g's frame and
 *      whose exception PC lies inside g;
 *   2. the next frame out is the SIGNALLING POINT -- g's own frame;
 *   3. then f, then main.
 * The handler returns SS$_CONTINUE, so g resumes and everything returns
 * normally. Sentinel 7 = every check held. */
#include <stdint.h>
#include "libicb.h"

extern int printf(const char *, ...);
extern void *lib$establish(void *);
extern unsigned int lib$signal(unsigned int, ...);
extern uint32_t lib$get_curr_invo_context(INVO_CONTEXT_BLK *);
extern uint32_t lib$get_prev_invo_context(INVO_CONTEXT_BLK *);
extern int SYS$GL_CALL_HANDL;

struct chfctx { uint64_t flink, blink, sigarglst, mcharglst, expt_fp, expt_pc, expt_ps; };

static uint64_t fp_g, fp_f, fp_main;
/* This frame's FP register (R29), the value the invocation walk reports. */
#define MY_FP(v) __asm__ __volatile__("mov $29,%0" : "=r"(v))
static int disp_ok, chfctx_ok, sigpt_ok, f_ok, main_ok, handler_ran, g_resumed;
static int (*volatile g_pv)(void);

static uint64_t frame_pv(uint64_t fp) { return fp ? *(uint64_t *)(uintptr_t)fp : 0; }
static uint64_t pdsc_entry(uint64_t pv) { return *(uint64_t *)(uintptr_t)(pv + 8); }

static unsigned int handler(unsigned int *sig, void *mech)
{
    (void)sig; (void)mech;
    handler_ran = 1;
    INVO_CONTEXT_BLK icb;
    if (lib$get_curr_invo_context(&icb) != 1)        /* = the handler itself */
        return 1;
    if (lib$get_prev_invo_context(&icb) != 1)        /* -> dispatcher frame  */
        return 1;
    uint64_t want = (uint64_t)(int64_t)SYS$GL_CALL_HANDL;   /* longword, sign-extended */
    disp_ok = (want != 0 && frame_pv(icb.libicb$q_ireg[29]) == want);
    struct chfctx *c = (struct chfctx *)icb.libicb$ph_chfctx_addr;
    uint64_t gent = pdsc_entry((uint64_t)(uintptr_t)g_pv);
    chfctx_ok = (c && c->sigarglst && c->mcharglst && c->expt_fp == fp_g &&
                 c->expt_pc >= gent && c->expt_pc < gent + 0x400);
    if (lib$get_prev_invo_context(&icb) != 1)        /* -> signalling point  */
        return 1;
    sigpt_ok = (icb.libicb$q_ireg[29] == fp_g);
    if (lib$get_prev_invo_context(&icb) != 1)
        return 1;
    f_ok = (icb.libicb$q_ireg[29] == fp_f);
    if (lib$get_prev_invo_context(&icb) != 1)
        return 1;
    main_ok = (icb.libicb$q_ireg[29] == fp_main);
    return 1;                                         /* SS$_CONTINUE */
}

static int g(void)
{
    MY_FP(fp_g);
    lib$signal(0x2C, 0);
    g_resumed = 1;
    return 5;
}

static int f(void)
{
    MY_FP(fp_f);
    return g() + 1;
}

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;
    MY_FP(fp_main);
    g_pv = g;
    lib$establish((void *)handler);
    int r = f();
    printf("OVMX CHF dispatcher test: handler=%d disp=%d chfctx=%d sigpt=%d f=%d main=%d resumed=%d r=%d argc=%d\n",
           handler_ran, disp_ok, chfctx_ok, sigpt_ok, f_ok, main_ok, g_resumed, r, argc);
    if (handler_ran && disp_ok && chfctx_ok && sigpt_ok && f_ok && main_ok && g_resumed && r == 6)
        return 7;
    return 3;
}
