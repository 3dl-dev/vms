/*
 * IMGACT.EXE — aarch64 architecture definitions.
 *
 * Part of OVMX (OpenVMS-compatible environment for Linux), bead vms-913.2.
 * Design contract: docs/design-image-activation.md.
 *
 * Clean-room: all VMS-facing semantics are derived from the OVMX design spec
 * and public VMS behavior only. The ELF loading structure is adapted from
 * musl libc's ldso/dynlink.c (MIT license) — see imgact.c header comment.
 */
#ifndef OVMX_IMGACT_ARCH_AARCH64_H
#define OVMX_IMGACT_ARCH_AARCH64_H

/*
 * AArch64 dynamic relocation types (Elf64 R_TYPE).
 * Values from the AArch64 ELF ABI (public documentation). Confirmed against
 * `readelf -r` output of musl-gcc built shared objects.
 */
#define R_AARCH64_ABS64        257
#define R_AARCH64_GLOB_DAT     1025
#define R_AARCH64_JUMP_SLOT    1026
#define R_AARCH64_RELATIVE     1027
#define R_AARCH64_TLS_DTPMOD   1028
#define R_AARCH64_TLS_DTPREL   1029
#define R_AARCH64_TLS_TPREL    1030
#define R_AARCH64_TLSDESC      1031

/*
 * AArch64 uses TLS Variant I (TLS_ABOVE_TP): the thread pointer (TPIDR_EL0)
 * points at the thread control block; TLS blocks live at positive offsets
 * above it, after a reserved 2-word (16 byte) TCB area.
 */
#define TLS_TCB_SIZE   16

/* Assembly helpers (arch/aarch64/start.S). */
void _start(void);                 /* ELF entry point */
void __tlsdesc_static(void);       /* TLSDESC static resolver */
void imgact_set_tp(void *tp);      /* msr tpidr_el0, x0 */

#endif /* OVMX_IMGACT_ARCH_AARCH64_H */
