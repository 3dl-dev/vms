/*
 * test_invo_context.c - vms-1fa CHF rung-3: Alpha invocation-context primitives.
 *
 * Proves the PDSC/RSA invocation-chain walk that LIB$GET_INVO_CONTEXT /
 * LIB$GET_PREV_INVO_CONTEXT / LIB$GET_INVO_HANDLE / LIB$GET_PREV_INVO_HANDLE
 * (src/libvms/rtl/lib_invo.c) implement, and the anchorless-target
 * reconstruction the CHF dispatcher (perform_unwind, lib_signal.c) consults.
 *
 * HOW (host-buildable, arch-generic for the WALK ENGINE). The walk is pure
 * control flow over the Alpha Calling Standard's procedure descriptors (PDSC)
 * and register save areas (RSA). We CONSTRUCT a faithful Alpha frame chain in
 * memory - real PDSC descriptors + RSA quadwords, 64-bit like Alpha (host
 * x86_64 is LP64) - register a resolver over it, seed an innermost invocation
 * context, and assert that each LIB$GET_PREV_INVO_CONTEXT reconstructs the
 * caller's PC, frame pointer (R29), stack pointer, established-handler flag and
 * bottom-of-stack marker exactly. This is the host proof called for by the
 * design (docs/design-chf-condition-handling.md rung-3); capturing a REAL
 * Alpha register file and the machine register-restore transfer are the
 * deferred Alpha-runtime children (vms-cc8 / vms-8e8c).
 *
 * Reference: Alpha/OpenVMS Calling Standard, "Procedure Descriptors",
 *            "Register Save Areas", "Invocation Context".
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "ssdef.h"
#include "libicb.h"
#include "pdscdef.h"
#include "lib$routines.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ================================================================
 * A synthetic Alpha stack image + a PDSC resolver over it.
 *
 * `mem[]` holds the frames' register save areas; frame pointers are addresses
 * into it. Each procedure gets a PDSC and a distinct PC; the resolver maps a
 * PC (exact match - the test controls all PCs) to its descriptor.
 * ================================================================ */

static uint64_t mem[512];

#define ADDR(idx)   ((uint64_t)(uintptr_t)&mem[(idx)])

struct pdsc_map_entry {
    uint64_t pc;
    const struct pdsc_descriptor *pd;
};

static struct pdsc_map_entry g_map[16];
static int g_map_count = 0;

static const struct pdsc_descriptor *test_resolver(uint64_t pc, void *user)
{
    (void)user;
    for (int i = 0; i < g_map_count; i++) {
        if (g_map[i].pc == pc) {
            return g_map[i].pd;
        }
    }
    return NULL;   /* outside known code -> base of chain */
}

static void map_reset(void)
{
    g_map_count = 0;
}

static void map_add(uint64_t pc, const struct pdsc_descriptor *pd)
{
    g_map[g_map_count].pc = pc;
    g_map[g_map_count].pd = pd;
    g_map_count++;
}

/* ================================================================
 * Scenario A: a 3-deep all-stack-frame chain  C -> B -> main.
 *
 *   - C (current), B (has an established handler), main (bottom of stack).
 *   - Each stack frame saves the caller's FP (R29) in its RSA; main's RSA
 *     saved-return is 0, which marks the bottom.
 * ================================================================ */

#define PC_C     0x0000000000003000ull
#define PC_B     0x0000000000002000ull
#define PC_MAIN  0x0000000000001000ull

/* Handler address B advertises (any non-zero token). */
static char b_handler_token;

static void build_scenario_a(struct pdsc_descriptor *pd_c,
                             struct pdsc_descriptor *pd_b,
                             struct pdsc_descriptor *pd_main,
                             INVO_CONTEXT_BLK *seed)
{
    memset(mem, 0, sizeof(mem));

    /* Frame pointers into the stack image. */
    uint64_t fp_c    = ADDR(100);
    uint64_t fp_b    = ADDR(200);
    uint64_t fp_main = ADDR(300);
    uint64_t sp_c    = ADDR(90);

    /* C's RSA at fp_c: saved return -> PC_B, saved R29 -> fp_b. */
    mem[100] = PC_B;      /* RSA$Q_SAVED_RETURN */
    mem[101] = fp_b;      /* saved R29 (first/only bit in ireg_mask) */

    /* B's RSA at fp_b: saved return -> PC_MAIN, saved R29 -> fp_main. */
    mem[200] = PC_MAIN;
    mem[201] = fp_main;

    /* main's RSA at fp_main: saved return 0 -> bottom of stack. */
    mem[300] = 0;
    mem[301] = 0;

    memset(pd_c, 0, sizeof(*pd_c));
    pd_c->pdsc$w_flags    = PDSC$K_KIND_FP_STACK | PDSC$V_BASE_REG_IS_FP;
    pd_c->pdsc$w_rsa_offset = 0;
    pd_c->pdsc$l_size     = 80;
    pd_c->pdsc$l_ireg_mask = (1u << ALPHA_REG_FP);
    pd_c->pdsc$q_entry    = PC_C;

    memset(pd_b, 0, sizeof(*pd_b));
    pd_b->pdsc$w_flags    = PDSC$K_KIND_FP_STACK | PDSC$V_BASE_REG_IS_FP
                          | PDSC$V_HANDLER_VALID;
    pd_b->pdsc$w_rsa_offset = 0;
    pd_b->pdsc$l_size     = 80;
    pd_b->pdsc$l_ireg_mask = (1u << ALPHA_REG_FP);
    pd_b->pdsc$q_entry    = PC_B;
    pd_b->pdsc$q_handler  = (uint64_t)(uintptr_t)&b_handler_token;

    memset(pd_main, 0, sizeof(*pd_main));
    pd_main->pdsc$w_flags    = PDSC$K_KIND_FP_STACK | PDSC$V_BASE_REG_IS_FP;
    pd_main->pdsc$w_rsa_offset = 0;
    pd_main->pdsc$l_size     = 80;
    pd_main->pdsc$l_ireg_mask = (1u << ALPHA_REG_FP);
    pd_main->pdsc$q_entry    = PC_MAIN;

    map_reset();
    map_add(PC_C, pd_c);
    map_add(PC_B, pd_b);
    map_add(PC_MAIN, pd_main);
    vms$$invo_set_pdsc_resolver(test_resolver, NULL);

    /* Seed the innermost context (frame C). */
    memset(seed, 0, sizeof(*seed));
    seed->libicb$q_program_counter = PC_C;
    seed->libicb$q_ireg[ALPHA_REG_FP] = fp_c;
    seed->libicb$q_ireg[ALPHA_REG_SP] = sp_c;
    seed->libicb$q_stack_pointer = sp_c;
}

static void test_stack_walk(void)
{
    printf("Scenario A: LIB$GET_PREV_INVO_CONTEXT walks a 3-deep stack chain...\n");

    struct pdsc_descriptor pd_c, pd_b, pd_main;
    INVO_CONTEXT_BLK icb;
    build_scenario_a(&pd_c, &pd_b, &pd_main, &icb);

    uint64_t fp_b    = ADDR(200);
    uint64_t fp_main = ADDR(300);

    /* Frame 0: the seeded innermost frame C. */
    check(icb.libicb$q_program_counter == PC_C, "frame 0 PC is C");
    INVO_HANDLE h_c = lib$get_invo_handle(&icb);

    /* Walk to frame 1: B. */
    uint32_t st = lib$get_prev_invo_context(&icb);
    check($VMS_STATUS_SUCCESS(st), "get_prev to B succeeds");
    check(icb.libicb$q_program_counter == PC_B, "frame 1 PC reconstructed as B");
    check(icb.libicb$q_ireg[ALPHA_REG_FP] == fp_b,
          "frame 1 FP (R29) restored from C's RSA");
    check(icb.libicb$v_handler_present == 1,
          "frame 1 reports an established handler (PDSC$V_HANDLER_VALID)");
    check(icb.libicb$v_bottom_of_stack == 0, "frame 1 is not bottom of stack");
    INVO_HANDLE h_b = lib$get_invo_handle(&icb);

    /* Walk to frame 2: main (bottom of stack). */
    st = lib$get_prev_invo_context(&icb);
    check($VMS_STATUS_SUCCESS(st), "get_prev to main succeeds");
    check(icb.libicb$q_program_counter == PC_MAIN, "frame 2 PC reconstructed as main");
    check(icb.libicb$q_ireg[ALPHA_REG_FP] == fp_main,
          "frame 2 FP (R29) restored from B's RSA");
    check(icb.libicb$v_handler_present == 0, "frame 2 (main) has no handler");
    check(icb.libicb$v_bottom_of_stack == 1,
          "frame 2 (main) is marked bottom of stack (RSA saved-return 0)");

    /* One more walk: no caller to produce. */
    st = lib$get_prev_invo_context(&icb);
    check(st == LIBICB$_NOMOREFRAMES,
          "get_prev past the bottom returns LIBICB$_NOMOREFRAMES");

    /* Handles are distinct per invocation. */
    check(h_c != h_b && h_c != LIBICB$K_INVO_HANDLE_NULL,
          "invocation handles are distinct per frame");
}

/* ================================================================
 * Scenario A': handle round-trip through the current-context seam.
 *
 * With C seeded as the current context, LIB$GET_INVO_CONTEXT(handle) must
 * return to a frame's context, and LIB$GET_PREV_INVO_HANDLE must step outward.
 * ================================================================ */

static void test_handle_roundtrip(void)
{
    printf("Scenario A': handle round-trip (get_invo_context / get_prev_invo_handle)...\n");

    struct pdsc_descriptor pd_c, pd_b, pd_main;
    INVO_CONTEXT_BLK seed;
    build_scenario_a(&pd_c, &pd_b, &pd_main, &seed);

    /* Make C the "current" context the handle routines walk out from. */
    vms$$invo_set_curr_context(&seed);

    INVO_CONTEXT_BLK cur;
    uint32_t st = lib$get_curr_invo_context(&cur);
    check($VMS_STATUS_SUCCESS(st) && cur.libicb$q_program_counter == PC_C,
          "get_curr returns the seeded frame C");
    INVO_HANDLE h_c = lib$get_invo_handle(&cur);

    /* The caller of C is B. */
    INVO_HANDLE h_b = lib$get_prev_invo_handle(h_c);
    check(h_b != LIBICB$K_INVO_HANDLE_NULL, "get_prev_invo_handle(C) yields B");

    /* That handle round-trips back to B's context. */
    INVO_CONTEXT_BLK icb_b;
    st = lib$get_invo_context(h_b, &icb_b);
    check($VMS_STATUS_SUCCESS(st) && icb_b.libicb$q_program_counter == PC_B,
          "get_invo_context(handle_B) returns B's context");

    /* The caller of B is main. */
    INVO_HANDLE h_main = lib$get_prev_invo_handle(h_b);
    check(h_main != LIBICB$K_INVO_HANDLE_NULL, "get_prev_invo_handle(B) yields main");

    /* main is the bottom: it has no previous invocation. */
    INVO_HANDLE h_none = lib$get_prev_invo_handle(h_main);
    check(h_none == LIBICB$K_INVO_HANDLE_NULL,
          "get_prev_invo_handle(main) is NULL at the bottom of stack");

    vms$$invo_set_curr_context(NULL);   /* restore genuine capture */
}

/* ================================================================
 * Scenario B: a register-frame (leaf) procedure hop.
 *
 * A register-frame procedure keeps its caller's return address in a register
 * (R26) and shares the caller's stack; walking to the caller is a register
 * read with FP/SP unchanged.  L (register/leaf) -> P (stack, bottom).
 * ================================================================ */

#define PC_L   0x0000000000005000ull
#define PC_P   0x0000000000004000ull

static void test_register_frame_hop(void)
{
    printf("Scenario B: register-frame (leaf) hop via the return-address register...\n");

    memset(mem, 0, sizeof(mem));
    uint64_t fp_p = ADDR(400);
    uint64_t sp_l = ADDR(390);

    /* P's RSA: saved-return 0 -> P is the base of this mini-chain. */
    mem[400] = 0;
    mem[401] = 0;

    struct pdsc_descriptor pd_l, pd_p;

    memset(&pd_l, 0, sizeof(pd_l));
    pd_l.pdsc$w_flags   = PDSC$K_KIND_FP_REGISTER;
    pd_l.pdsc$b_save_ra = ALPHA_REG_RA;   /* return address is live in R26 */
    pd_l.pdsc$q_entry   = PC_L;

    memset(&pd_p, 0, sizeof(pd_p));
    pd_p.pdsc$w_flags    = PDSC$K_KIND_FP_STACK | PDSC$V_BASE_REG_IS_FP;
    pd_p.pdsc$w_rsa_offset = 0;
    pd_p.pdsc$l_size     = 80;
    pd_p.pdsc$l_ireg_mask = (1u << ALPHA_REG_FP);
    pd_p.pdsc$q_entry    = PC_P;

    map_reset();
    map_add(PC_L, &pd_l);
    map_add(PC_P, &pd_p);
    vms$$invo_set_pdsc_resolver(test_resolver, NULL);

    INVO_CONTEXT_BLK icb;
    memset(&icb, 0, sizeof(icb));
    icb.libicb$q_program_counter = PC_L;
    icb.libicb$q_ireg[ALPHA_REG_RA] = PC_P;   /* caller return addr, in R26 */
    icb.libicb$q_ireg[ALPHA_REG_FP] = fp_p;   /* shares P's frame */
    icb.libicb$q_ireg[ALPHA_REG_SP] = sp_l;

    uint32_t st = lib$get_prev_invo_context(&icb);
    check($VMS_STATUS_SUCCESS(st), "get_prev across a register frame succeeds");
    check(icb.libicb$q_program_counter == PC_P,
          "caller PC taken from the return-address register (R26)");
    check(icb.libicb$q_ireg[ALPHA_REG_FP] == fp_p,
          "register-frame hop leaves FP unchanged (shared frame)");
    check(icb.libicb$q_ireg[ALPHA_REG_SP] == sp_l,
          "register-frame hop leaves SP unchanged (shared stack)");
    check(icb.libicb$v_bottom_of_stack == 1,
          "the reached caller (P) is the bottom of this chain");
}

/* ================================================================
 * Scenario C: the anchorless-target reconstruction the CHF dispatcher uses.
 *
 * perform_unwind() (lib_signal.c) calls vms$$invo_reconstruct_target() with a
 * target frame's establisher PC when that frame armed no VMS$UNWIND_ANCHOR.
 * Prove it walks the current chain to that frame and hands back its
 * reconstructed context (what the Alpha runtime would then transfer into).
 * ================================================================ */

static void test_reconstruct_target(void)
{
    printf("Scenario C: anchorless-target reconstruction (perform_unwind wiring)...\n");

    struct pdsc_descriptor pd_c, pd_b, pd_main;
    INVO_CONTEXT_BLK seed;
    build_scenario_a(&pd_c, &pd_b, &pd_main, &seed);
    vms$$invo_set_curr_context(&seed);

    /* Reconstruct the frame whose PC is B (an anchorless ancestor of C). */
    INVO_CONTEXT_BLK target;
    uint32_t st = vms$$invo_reconstruct_target(PC_B, &target);
    check(st == SS$_NORMAL, "reconstruct_target finds the anchorless frame B");
    check(target.libicb$q_program_counter == PC_B,
          "reconstructed target carries B's resume PC");
    check(target.libicb$q_ireg[ALPHA_REG_FP] == ADDR(200),
          "reconstructed target carries B's restored frame pointer");

    /* A PC that is not on the chain reconstructs nothing. */
    st = vms$$invo_reconstruct_target(0xDEADBEEFull, &target);
    check(st == LIBICB$_NOMOREFRAMES,
          "reconstruct_target reports no match for an off-chain PC");

    /* vms$$invo_transfer honestly reports it cannot machine-transfer on the
     * host (no Alpha register file to restore) - the pop-only unwind stands. */
    int transferred = vms$$invo_transfer(&target, NULL);
    check(transferred == 0,
          "vms$$invo_transfer reports host cannot restore an Alpha frame (Alpha-runtime child)");

    vms$$invo_set_curr_context(NULL);
}

int main(void)
{
    printf("=== vms-1fa rung-3: Alpha invocation-context primitive tests ===\n");
    test_stack_walk();
    test_handle_roundtrip();
    test_register_frame_hop();
    test_reconstruct_target();

    if (failures == 0) {
        printf("\nAll invocation-context tests passed.\n");
        return 0;
    }
    printf("\n%d assertion(s) FAILED.\n", failures);
    return 1;
}
