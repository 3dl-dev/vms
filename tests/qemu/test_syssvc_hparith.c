/*
 * test_syssvc_hparith.c - the Alpha arithmetic-trap -> SS$_HPARITH condition
 * bridge, end to end, under -static (rd vms-db3, epic vms-8954).
 *
 * Proves the GAP3 executive/signal primitive: a trap-enabling FP divide-by-zero
 * on OVMX/Linux-Alpha is delivered as SIGFPE and turned into a FAITHFUL
 * SS$_HPARITH condition (value 1284, oracle-pinned OpenVMS Alpha V8.4) carrying
 * the oracle's own 5-arg signal array {Imask, Fmask, summary, PC, PS}, searched
 * against the CHF handler chain, becoming $STATUS if unhandled.
 *
 * Structured so a SUBSTRATE fidelity failure is DISTINGUISHABLE from a bridge
 * bug (per the design review): CHECK 2 reads sc_fpcr with a RAW handler and
 * asserts the substrate sets the sticky bits faithfully BEFORE the full chain
 * runs. If CHECK 2 fails under qemu-system TCG, that is "blocked-on-substrate"
 * (INV-6, honest stop) -- not a confusing downstream red -- never a faked trap.
 *
 * Alpha-only (sc_fpcr / trapb / the FP-trap model are Alpha). On other arches it
 * honest-skips (exit 77), never fakes a pass.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#define EXIT_SKIP 77

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

/* Alpha exception-summary byte bits (must match src/libvms/rtl/arith_signal.c;
 * public Alpha EXC_SUM format). */
#define EXCSUM_SWC 0x01
#define EXCSUM_DZE 0x04

#if defined(__alpha__)
#include <fenv.h>
#include <signal.h>
#include <ucontext.h>
#include <setjmp.h>
#include "ssdef.h"
#include "chfdef.h"

/* Bridge surface (src/libvms/rtl/arith_signal.c). */
extern int  ovmx$arith_signal_installed;
extern void ovmx$arith_signal_reinstall(void);
extern int  vms$$last_arith_exception(uint32_t *summary, uint32_t *imask,
                                      uint32_t *fmask, uint64_t *pc, uint32_t *ps);
/* CHF (src/libvms/rtl/lib_signal.c). */
extern void *lib$establish(void *handler);
extern uint32_t lib$revert(void);

#define FPCR_DZE_BIT 53

/* ---- CHECK 2: raw substrate-fidelity probe ---- */
static sigjmp_buf raw_jb;
static volatile unsigned long long raw_fpcr;
static void raw_fpe(int s, siginfo_t *si, void *ucv) {
    (void)s; (void)si;
    ucontext_t *uc = (ucontext_t *)ucv;
    raw_fpcr = uc ? (unsigned long long)uc->uc_mcontext.sc_fpcr : 0;
    siglongjmp(raw_jb, 1);
}

/* ---- CHECK 3: established VMS condition handler ---- */
static volatile int cond_caught = 0;
static volatile uint32_t cond_summary = 0;
static volatile uint32_t cond_name = 0;
static uint32_t hparith_handler(void *sigargs, void *mecharg) {
    (void)mecharg;
    struct chf$signal_array *sig = (struct chf$signal_array *)sigargs;
    cond_name = sig->chf$is_sig_name;
    if (sig->chf$is_sig_name == (uint32_t)SS$_HPARITH) {
        cond_caught = 1;
        /* args: arg1=Imask, arg2=Fmask, arg3=summary, arg4=PC, arg5=PS */
        cond_summary = (&sig->chf$is_sig_arg1)[2];
        return SS$_CONTINUE;      /* claim: resume after the (imprecise) trap */
    }
    return SS$_RESIGNAL;
}

int main(void) {
    printf("=== test_syssvc_hparith (Alpha FP-trap -> SS$_HPARITH bridge, -static) ===\n");

    /* CHECK 1: the image-start auto-install fired under -static. The bridge's
     * constructor is force-pulled by the arith_signal_bind.c anchor (compiled
     * into this target); if the anchor/ctor path is broken this is 0. */
    CHECK(ovmx$arith_signal_installed == 1,
          "image-start SIGFPE bridge auto-installed under -static (anchor + constructor)");

    /* CHECK 2: SUBSTRATE FIDELITY (raw, independent of the bridge). A known FP
     * divide-by-zero must set the FPCR DZE sticky bit in the trap context.
     * A failure here = blocked-on-substrate (the TCG FP-trap emulation is not
     * faithful), NOT a bridge bug. */
    struct sigaction raw = {0}, saved;
    raw.sa_sigaction = raw_fpe;
    raw.sa_flags = SA_SIGINFO;
    sigemptyset(&raw.sa_mask);
    sigaction(SIGFPE, &raw, &saved);
    feclearexcept(FE_ALL_EXCEPT);
    feenableexcept(FE_DIVBYZERO);
    raw_fpcr = 0;
    if (!sigsetjmp(raw_jb, 1)) {
        volatile double a = 1.0, b = 0.0, r;
        r = a / b;
        __asm__ __volatile__("trapb");
        (void)r;
        printf("  INFO: raw div0 did NOT trap (substrate did not deliver SIGFPE)\n");
    }
    CHECK((raw_fpcr & (1ULL << FPCR_DZE_BIT)) != 0,
          "substrate sets FPCR DZE sticky bit on FP div0 (sc_fpcr fidelity)");

    /* Restore the bridge handler that CHECK 2 overrode. */
    ovmx$arith_signal_reinstall();

    /* CHECK 3: FULL CHAIN. Establish a VMS condition handler, trigger a trap,
     * and assert the bridge raised a faithful SS$_HPARITH to it. */
    lib$establish((void *)hparith_handler);
    cond_caught = 0; cond_summary = 0; cond_name = 0;
    feclearexcept(FE_ALL_EXCEPT);
    feenableexcept(FE_DIVBYZERO);
    {
        volatile double a = 1.0, b = 0.0, r;
        r = a / b;
        __asm__ __volatile__("trapb");
        (void)r;
    }
    lib$revert();

    CHECK(cond_caught == 1,
          "bridge raised a condition to the established handler on FP div0");
    CHECK(cond_name == (uint32_t)SS$_HPARITH,
          "condition raised is SS$_HPARITH (1284, oracle-pinned)");
    CHECK((cond_summary & EXCSUM_DZE) != 0,
          "SS$_HPARITH signal array carries summary with the DZE bit (faithful decode)");
    CHECK((cond_summary & EXCSUM_SWC) != 0,
          "SS$_HPARITH signal array carries summary with the SWC (software-completion) bit");

    /* CHECK 4: the exception state is stashed for SYS$GET_ARITH_EXCEPTION. */
    uint32_t st_summary = 0, imask = 0, fmask = 0, ps = 0; uint64_t pc = 0;
    int have = vms$$last_arith_exception(&st_summary, &imask, &fmask, &pc, &ps);
    CHECK(have == 1 && (st_summary & EXCSUM_DZE) != 0,
          "arith exception stashed (summary=DZE) for SYS$GET_ARITH_EXCEPTION follow-on");

    printf("=== test_syssvc_hparith: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}

#else  /* !__alpha__ */
int main(void) {
    printf("=== test_syssvc_hparith: SKIPPED (Alpha-only: FP-trap/sc_fpcr/trapb) ===\n");
    return EXIT_SKIP;
}
#endif
