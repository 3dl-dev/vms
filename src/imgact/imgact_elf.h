/*
 * imgact_elf.h — ELF class-width abstraction for IMGACT.EXE (rd vms-73b2,
 * epic vms-404). Design contract: docs/design/vax-symbol-vector-imgact.md §1c.
 *
 * The activator core (imgact.c) is single-source across the Elf64 arches
 * (aarch64/x86_64/alpha, Linux) and the one Elf32 arch (vax, NetBSD). The
 * ELF *container* — Ehdr/Phdr/Shdr/Sym/Dyn/Rela/Addr and the R_TYPE/R_SYM/
 * ST_BIND accessors — is class-sized; every other structure the activator
 * touches (the OVMX symbol-vector format, ovmx_image.h) is width-portable by
 * construction (64-bit containers + a software load_bias add, so it is
 * byte-identical on the wire across arches — see ovmx_symvec.h).
 *
 * ElfW(T) and the ELFW_* accessors resolve to the target's class:
 *   - Elf32 under __vax__ (or IMGACT_FORCE_ELFCLASS32, used by the host unit
 *     test to drive the elf32 path on an x86_64 build host);
 *   - Elf64 everywhere else.
 *
 * ZERO-REGRESSION invariant (CLAUDE.md Rule 7, the byte-unchanged done-tell of
 * vms-73b2): on every Elf64 arch, ElfW(Ehdr) expands to the EXACT token
 * `Elf64_Ehdr` the code used before this abstraction landed, ELFW_R_TYPE(x)
 * to `ELF64_R_TYPE(x)`, etc. The preprocessed output — hence the codegen — is
 * identical to the pre-vms-73b2 source. The Elf32 path is reached only when
 * IMGACT_ELFCLASS32 is defined, which no Elf64 arch defines.
 */
#ifndef OVMX_IMGACT_ELF_H
#define OVMX_IMGACT_ELF_H

#include <elf.h>

#if defined(__vax__) || defined(IMGACT_FORCE_ELFCLASS32)
#  define IMGACT_ELFCLASS32 1
#endif

/* NetBSD's <elf.h> already defines ElfW(x) (keyed on the ELFSIZE macro); glibc
 * does not define it in <elf.h>. We drive the class from the target arch, not
 * ELFSIZE, so replace any pre-existing definition with ours. */
#ifdef ElfW
#  undef ElfW
#endif

#if defined(IMGACT_ELFCLASS32)
#  define ElfW(type)       Elf32_##type
#  define ELFW_R_TYPE(i)   ELF32_R_TYPE(i)
#  define ELFW_R_SYM(i)    ELF32_R_SYM(i)
#  define ELFW_ST_BIND(i)  ELF32_ST_BIND(i)
#else
#  define ElfW(type)       Elf64_##type
#  define ELFW_R_TYPE(i)   ELF64_R_TYPE(i)
#  define ELFW_R_SYM(i)    ELF64_R_SYM(i)
#  define ELFW_ST_BIND(i)  ELF64_ST_BIND(i)
#endif

/*
 * auxv entry. The Linux Elf64 arches use the glibc/musl `Elf64_auxv_t`
 * ({ a_type; a_un.a_val; }). The NetBSD-vax substrate names its aux entry
 * `Aux32Info`/`AuxInfo` with a different member spelling (`a_v`), and does NOT
 * define a glibc-style `Elf32_auxv_t`. The activator's aux walk uses
 * `a->a_type` / `a->a_un.a_val`, so on __vax__ we supply a struct with that
 * exact member shape and the correct 2-longword SysV-ELF32 layout (type then
 * value); the kernel's on-stack aux vector is laid out identically. This is a
 * NetBSD-substrate concern, not an Elf32 concern, so it is keyed on __vax__
 * (the host FORCE_ELFCLASS32 test never walks the aux vector).
 */
#if defined(__vax__)
typedef struct {
	Elf32_Word a_type;
	union { Elf32_Word a_val; } a_un;
} imgact_auxv_t;
#else
typedef Elf64_auxv_t imgact_auxv_t;
#endif

#endif /* OVMX_IMGACT_ELF_H */
