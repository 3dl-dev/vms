/*
 * testprod_shr.c - a NON-RESIDENT producer shareable (TESTPROD.EXE) for the
 * in-process activation NON-RESIDENT-producer sub-step (vms-db2,
 * docs/design-in-process-activation.md Part II §A.8 remainder item 1). NOT a
 * test suite (its name matches no test_*.c glob): it is the producer the
 * activator MAPS into DCL's process when a consumer's .vms$imp names it and it
 * is not already resident.
 *
 * THE POINT. Every real OVMX producer (LIBVMS$SHR, DECC$SHR) is already resident
 * in DCL's process. This producer models the OTHER case the design names: an
 * image imports a universal from a shareable the process does NOT hold, so
 * imgact_map_producer() (src/libvms/syssvc/sys_imgact.c) maps it in-process,
 * registers it, and binds the consumer to it -- a genuinely shared single
 * instance, never a per-consumer private copy (the LARP the invariants forbid).
 *
 * WHAT IT EXPORTS (a pure LEAF producer -- no PT_TLS, no PT_INTERP, no
 * PT_DYNAMIC, no C-RTL bootstrap, so imgact_map_producer accepts it; anything
 * heavier stays deferred and forks):
 *   .vms$sv index 0  prod_bump   PROCEDURE  long prod_bump(void): ++counter, ret
 *   .vms$sv index 1  prod_counter DATA      the shared quadword the test reads
 *                                           AND mutates through the mapped base
 * prod_bump reaches prod_counter PC-relatively (no dynamic reloc); the .vms$sv
 * values are the sections' pinned image-relative vaddrs (0x100000 / 0x200000,
 * pinned via -Wl,--section-start), which ovmx_sv_resolve biases by the mapped
 * load base. No name is "__init_libc", so the C-RTL fence (imgact_producer_
 * needs_crtl) passes it as a non-C-RTL producer.
 *
 * Rule 8: ELF + OVMX symbol-vector (.vms$sv) mechanics only; no VMS byte format
 * is claimed. Built freestanding static-PIE with zero dynamic relocations.
 */
#include <stdint.h>

/* prod_bump: ++prod_counter; return prod_counter. In a pinned RX section so its
 * image-relative vaddr is the fixed literal the .vms$sv records; reaches the
 * (also pinned) counter PC-relatively, so the image carries no dynamic reloc. */
__asm__(
#if defined(__x86_64__)
    ".pushsection .vms$pcode,\"ax\",@progbits\n"
    ".balign 16\n"
    ".hidden prod_bump\n.type prod_bump,@function\n"
    "prod_bump:\n"                       /* pinned at 0x100000 */
    "  lea prod_counter(%rip), %rax\n"
    "  mov (%rax), %rcx\n"
    "  add $1, %rcx\n"
    "  mov %rcx, (%rax)\n"
    "  mov %rcx, %rax\n"
    "  ret\n"
    ".size prod_bump,.-prod_bump\n"
    ".popsection\n"
    ".pushsection .vms$pdata,\"aw\",@progbits\n"
    ".balign 8\n"
    ".hidden prod_counter\n"
    "prod_counter: .quad 0\n"            /* pinned at 0x200000 */
    ".popsection\n"
#elif defined(__aarch64__)
    ".pushsection .vms$pcode,\"ax\",@progbits\n"
    ".balign 16\n"
    ".hidden prod_bump\n.type prod_bump,@function\n"
    "prod_bump:\n"
    "  adrp x1, prod_counter\n"
    "  add  x1, x1, :lo12:prod_counter\n"
    "  ldr  x0, [x1]\n"
    "  add  x0, x0, #1\n"
    "  str  x0, [x1]\n"
    "  ret\n"
    ".size prod_bump,.-prod_bump\n"
    ".popsection\n"
    ".pushsection .vms$pdata,\"aw\",@progbits\n"
    ".balign 8\n"
    ".hidden prod_counter\n"
    "prod_counter: .quad 0\n"
    ".popsection\n"
#elif defined(__alpha__)
    /* prod_bump is reached by an INDIRECT call through the mapped .vms$sv
     * entry's raw address (bias + 0x100000), not a normal C-ABI call from
     * this module -- so, exactly like real_start in testreal_inproc.c, it
     * cannot assume $27/gp are valid at entry and must self-establish gp via
     * the br+ldgp idiom before reaching prod_counter GP-relatively (Alpha has
     * no PC-relative lea/adrp; see testreal_inproc.c's got_slot for the full
     * rationale). Verified end-to-end under qemu-alpha with the identical
     * br+ldgp+gprelhigh/gprellow sequence. */
    ".pushsection .vms$pcode,\"ax\",@progbits\n"
    ".balign 16\n"
    ".hidden prod_bump\n.type prod_bump,@function\n"
    ".ent prod_bump\n"
    "prod_bump:\n"                       /* pinned at 0x100000 */
    "  .frame $30, 0, $26, 0\n"
    "  .prologue 0\n"
    "  br   $29, 1f\n"
    "1:\n"
    "  ldgp $29, 0($29)\n"
    "  ldah $1, prod_counter($29) !gprelhigh\n"
    "  lda  $1, prod_counter($1) !gprellow\n"
    "  ldq  $0, 0($1)\n"
    "  addq $0, 1, $0\n"
    "  stq  $0, 0($1)\n"
    "  ret\n"
    ".end prod_bump\n"
    ".size prod_bump,.-prod_bump\n"
    ".popsection\n"
    ".pushsection .vms$pdata,\"aw\",@progbits\n"
    ".balign 8\n"
    ".hidden prod_counter\n"
    "prod_counter: .quad 0\n"            /* pinned at 0x200000 */
    ".popsection\n"
#endif
);

/*
 * The .vms$sv symbol vector: header + 2 entries + a diagnostic name blob.
 * Entry values are the pinned image-relative vaddrs (0x100000 prod_bump,
 * 0x200000 prod_counter); GSMATCH ALWAYS 1.0 (any consumer version binds). The
 * struct layout mirrors src/vmslink/include/ovmx_image.h byte-for-byte:
 *   header: magic,count,gsmatch_kind,gsmatch_major,gsmatch_minor,
 *           names_off,names_size,reserved   (8 x u32)
 *   entry:  value(u64), kind(u32), name_off(u32)
 */
__asm__(
    ".pushsection .vms$sv,\"a\",@progbits\n"
    ".balign 8\n"
    "testprod_sv:\n"
    "  .long 0x31565356\n"        /* magic OVMX_SV_MAGIC "VSV1" */
    "  .long 2\n"                  /* count                      */
    "  .long 0\n"                  /* gsmatch_kind = ALWAYS       */
    "  .long 1\n"                  /* gsmatch_major              */
    "  .long 0\n"                  /* gsmatch_minor              */
    "  .long testprod_sv_names - testprod_sv\n"          /* names_off  */
    "  .long testprod_sv_names_end - testprod_sv_names\n"/* names_size */
    "  .long 0\n"                  /* reserved                   */
    /* entry[0] prod_bump: value=0x100000 kind=PROCEDURE(1) name_off=0 */
    "  .quad 0x100000\n  .long 1\n  .long 0\n"
    /* entry[1] prod_counter: value=0x200000 kind=DATA(2) name_off=10 */
    "  .quad 0x200000\n  .long 2\n  .long 10\n"
    "testprod_sv_names:\n"
    "  .asciz \"prod_bump\"\n"     /* offset 0  (10 bytes incl NUL) */
    "  .asciz \"prod_counter\"\n"  /* offset 10                     */
    "testprod_sv_names_end:\n"
    ".popsection\n"
);
