/*
 * ovmx_activation.h -- OVMX VMS image-activation context contract.
 *
 * The shared interface both IMGACT.EXE (src/imgact) and the OVMX C run-time
 * library (DECC$SHR -- the GCC-oracle lane's decc$main) compile against. It
 * defines the six-argument VMS image-activation context an `alpha-dec-vms` GCC
 * port image's crt0 (`__main` -> `decc$main` -> `main`, libgcc/config/vms/
 * vms-ucrt0.c, GPLv3) receives at its transfer address. IMGACT builds this
 * context and presents it by the Alpha calling standard (vms-f60d, the
 * activation-time face of R8); decc$main consumes it to produce argc/argv/envp.
 *
 * Item vms-f60d. Design: docs/design-imgact-vms-activation-context.md.
 *
 * ---------------------------------------------------------------------------
 * CLEAN-ROOM NOTE (CLAUDE.md Rule 8). Each structure below is labeled either:
 *   - VMS-AUTHENTIC   -- the byte layout is fully published (OpenVMS Calling
 *                        Standard / Programming Concepts Manual); reproduced
 *                        here to match VMS. (`dsc$descriptor_s`; the AI-register
 *                        field layout.)
 *   - OVMX-ORIGINAL   -- public docs give the SEMANTICS but no public byte
 *                        layout; OVMX defines its own representation and the
 *                        two halves of the interface (IMGACT + decc$main) agree
 *                        it here. (`ovmx_imghdr`, `ovmx_cli_util`.)
 * No VSI/HPE source or binary was read; the port crt0 contract is taken from
 * the GPL GCC source, the authoritative CONSUMER of this interface.
 * ---------------------------------------------------------------------------
 */
#ifndef OVMX_ACTIVATION_H
#define OVMX_ACTIVATION_H

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * VMS-AUTHENTIC: the VMS string descriptor (dsc$descriptor_s).
 *
 * Fully public layout (Programming Concepts Manual Ch.24 / Calling Standard
 * Ch.7): a 16-bit length, an 8-bit data type, an 8-bit class, then the data
 * pointer. IMGACT passes one of these as `image_file_desc` (arg 4) naming the
 * activated image's file spec; decc$main reads it for argv[0] when no CLI.
 *
 * descrip.h (src/libvms/include) carries the canonical HOSTED definition, which
 * pulls in <string.h>/<stdlib.h>. IMGACT is freestanding (-ffreestanding
 * -nostdlib) and cannot include descrip.h, so we (re)define the identical
 * struct here, guarded against descrip.h so a hosted consumer that includes
 * both headers sees exactly one definition. */
#ifndef __DESCRIP_H
#ifndef OVMX_ACT_HAVE_DSC_DESCRIPTOR_S
#define OVMX_ACT_HAVE_DSC_DESCRIPTOR_S 1
struct dsc$descriptor_s {
	uint16_t dsc$w_length;   /* length of the data in bytes            */
	uint8_t  dsc$b_dtype;    /* data type code (DSC$K_DTYPE_T == 14)    */
	uint8_t  dsc$b_class;    /* class code     (DSC$K_CLASS_S == 1)     */
	char    *dsc$a_pointer;  /* address of the first byte of the data   */
};
#endif
#endif

/* Descriptor codes IMGACT stamps into image_file_desc (subset of descrip.h,
 * repeated so this header is self-contained for the freestanding IMGACT). */
#ifndef DSC$K_DTYPE_T
#define DSC$K_DTYPE_T 14   /* ASCII text string */
#endif
#ifndef DSC$K_CLASS_S
#define DSC$K_CLASS_S 1    /* fixed-length (static) descriptor */
#endif

/* ---------------------------------------------------------------------------
 * OVMX-ORIGINAL: the image header (`imghdr`, arg 3).
 *
 * Near-vestigial: the port crt0 forwards it verbatim to decc$main, which reads
 * only `flags` (no flag is defined/set at first light). OVMX images are ELF,
 * not VMS image files, and no public VMS `imghdr` byte layout exists, so this
 * is OVMX's own representation -- IMGACT and decc$main must agree on it. */
struct ovmx_imghdr {
	uint32_t version;     /* == OVMX_IMGHDR_VERSION                       */
	uint32_t flags;       /* image characteristics (none defined yet)     */
	void    *image_base;  /* run-time load base of the activated image    */
};
#define OVMX_IMGHDR_VERSION 1u
/* enum ovmx_imghdr_flags: no flags defined yet -- decc$main branches on none. */

/* ---------------------------------------------------------------------------
 * OVMX-ORIGINAL: the CLI callback vector (`cli_util`, arg 2).
 *
 * When the process was launched by OVMX DCL as a CLI (cliflag != 0), IMGACT
 * passes a pointer to this vector; decc$main calls back through it to fetch the
 * command line and parse it into argv. When there is no CLI (cliflag == 0),
 * IMGACT passes 0 and decc$main derives argv[0] from image_file_desc instead.
 * The VMS CLI-callback CONTRACT is public behavior; the vector STRUCT shape is
 * OVMX's own, agreed here between IMGACT and decc$main.
 *
 * get_command_line: fills *out with a descriptor for the invoking command line
 * (sourced from the executive process context) and returns a VMS status
 * (bit 0 set == success). */
struct ovmx_cli_util {
	uint32_t version;     /* == OVMX_CLI_UTIL_VERSION                     */
	uint32_t reserved;    /* 0 (keeps the callback pointer 8-byte aligned) */
	uint32_t (*get_command_line)(struct dsc$descriptor_s *out);
};
#define OVMX_CLI_UTIL_VERSION 1u

/* ---------------------------------------------------------------------------
 * VMS-AUTHENTIC: the Alpha standard-call Argument Information (AI) register.
 *
 * Public layout (OpenVMS Alpha Calling Standard): R25 (AI) carries, in
 * <7:0>, the argument count (AI$B_ARG_COUNT); in <25:8>, up to six 3-bit
 * argument-register-kind groups (AI$V_ARG_REG_INFO), each AI$K_AR_I64 (== 0)
 * for a 64-bit integer/pointer argument; <63:26> zero. IMGACT sets R25
 * explicitly by this layout (never a hard-coded 6) before the transfer.
 *
 * All six activation-context arguments are 64-bit integers/pointers, so every
 * kind group is AI$K_AR_I64; OVMX_AI_VMS_ACTIVATION is the resulting value. */
#define OVMX_AI_AR_I64        0u   /* AI$K_AR_I64: 64-bit sign-extended integer */
#define OVMX_AI_ARG_COUNT_POS 0
#define OVMX_AI_ARG_INFO_POS  8
#define OVMX_AI_KIND_BITS     3

#define OVMX_AI_BUILD6(k0, k1, k2, k3, k4, k5)                                \
	( ((uint64_t)6u << OVMX_AI_ARG_COUNT_POS)                            \
	| (((uint64_t)(k0) & 7u) << (OVMX_AI_ARG_INFO_POS + 0 * OVMX_AI_KIND_BITS)) \
	| (((uint64_t)(k1) & 7u) << (OVMX_AI_ARG_INFO_POS + 1 * OVMX_AI_KIND_BITS)) \
	| (((uint64_t)(k2) & 7u) << (OVMX_AI_ARG_INFO_POS + 2 * OVMX_AI_KIND_BITS)) \
	| (((uint64_t)(k3) & 7u) << (OVMX_AI_ARG_INFO_POS + 3 * OVMX_AI_KIND_BITS)) \
	| (((uint64_t)(k4) & 7u) << (OVMX_AI_ARG_INFO_POS + 4 * OVMX_AI_KIND_BITS)) \
	| (((uint64_t)(k5) & 7u) << (OVMX_AI_ARG_INFO_POS + 5 * OVMX_AI_KIND_BITS)) )

#define OVMX_AI_VMS_ACTIVATION                                                \
	OVMX_AI_BUILD6(OVMX_AI_AR_I64, OVMX_AI_AR_I64, OVMX_AI_AR_I64,        \
		       OVMX_AI_AR_I64, OVMX_AI_AR_I64, OVMX_AI_AR_I64)

#endif /* OVMX_ACTIVATION_H */
