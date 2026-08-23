/*
 * reloc.h - Alpha ELF dynamic relocation kinds + CRTJMP.
 * OVMX alpha-dec-vms musl port (vms-960).
 *
 * R_ALPHA_* values from the public Alpha ELF ABI (System V ABI, Alpha
 * processor supplement). RUNG-1 note: the dynamic linker (ldso) is not built
 * or exercised at rung 1 (static libc.a only); these are provided so the arch
 * is complete and self-consistent. TPOFF_K is provisional pending the loader.
 */

#define LDSO_ARCH "alpha"

#define TPOFF_K 0

#define REL_SYMBOLIC    R_ALPHA_REFQUAD    /* 2  */
#define REL_GOT         R_ALPHA_GLOB_DAT   /* 25 */
#define REL_PLT         R_ALPHA_JMP_SLOT   /* 26 */
#define REL_RELATIVE    R_ALPHA_RELATIVE   /* 27 */
#define REL_COPY        R_ALPHA_COPY       /* 24 */
#define REL_DTPMOD      R_ALPHA_DTPMOD64   /* 28 */
#define REL_DTPOFF      R_ALPHA_DTPREL64   /* 30 */
#define REL_TPOFF       R_ALPHA_TPREL64    /* 33 */

#define CRTJMP(pc,sp) __asm__ __volatile__( \
	"mov %1,$30 ; jmp $31,(%0),0" : : "r"(pc), "r"(sp) : "memory" )
