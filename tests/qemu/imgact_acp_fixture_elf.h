/*
 * imgact_acp_fixture_elf.h - a deterministic minimal ELF64 image, shared by the
 * fixture builder (mkimage_ods2_imgact.c) and the proof (test_syssvc_imgact_acp.c).
 *
 * The builder lays these exact bytes down as [IMGACT]TESTIMG.EXE on a genuine
 * ODS-2 volume; the test rebuilds the identical bytes in memory as the GOLDEN
 * and asserts that what IMGACT's ACP reader (imgact_acp.c) reads back over
 * IO$_ACCESS + IO$_READVBLK is byte-for-byte the same -- header AND every
 * PT_LOAD segment. Because both sides call this one generator, the golden is
 * the builder's own output, not a value the test invents.
 *
 * It is a WELL-FORMED ELF64 header + two PT_LOAD program headers with distinct,
 * position-derived fill patterns; it is not meant to be executed (the item
 * accepts "its bytes match the on-disk image" as the activation proof). The
 * layout is fixed and self-describing so the test parses e_phoff/e_phnum and
 * each segment's p_offset/p_filesz exactly as load_object() does.
 */

#ifndef IMGACT_ACP_FIXTURE_ELF_H
#define IMGACT_ACP_FIXTURE_ELF_H

#include <stdint.h>
#include <string.h>

/* Fixed layout (bytes). Two PT_LOAD segments at block-aligned file offsets. */
#define IMGACT_FIX_EHSZ     64u
#define IMGACT_FIX_PHOFF    64u
#define IMGACT_FIX_PHENT    56u
#define IMGACT_FIX_PHNUM    2u

#define IMGACT_FIX_SEG0_OFF   512u
#define IMGACT_FIX_SEG0_SZ    300u
#define IMGACT_FIX_SEG0_VADDR 0x1000u

#define IMGACT_FIX_SEG1_OFF   1024u
#define IMGACT_FIX_SEG1_SZ    400u
#define IMGACT_FIX_SEG1_VADDR 0x2000u

#define IMGACT_FIX_TOTAL   (IMGACT_FIX_SEG1_OFF + IMGACT_FIX_SEG1_SZ)  /* 1424 */

/* Segment fill patterns (position-derived so a misread shows up as a mismatch). */
static inline uint8_t imgact_fix_seg0_byte(unsigned i) { return (uint8_t)(0x11u ^ i); }
static inline uint8_t imgact_fix_seg1_byte(unsigned i) { return (uint8_t)(0x80u + i); }

/* Little helpers so the header is endian-explicit regardless of host. */
static inline void imgact_fix_put16(uint8_t *p, uint16_t v)
{ p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static inline void imgact_fix_put32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static inline void imgact_fix_put64(uint8_t *p, uint64_t v)
{ imgact_fix_put32(p, (uint32_t)v); imgact_fix_put32(p + 4, (uint32_t)(v >> 32)); }

/* One ELF64 program header (PT_LOAD) written at *p. */
static inline void imgact_fix_phdr(uint8_t *p, uint32_t flags, uint64_t off,
				   uint64_t vaddr, uint64_t filesz)
{
	memset(p, 0, IMGACT_FIX_PHENT);
	imgact_fix_put32(p + 0,  1);        /* p_type  = PT_LOAD */
	imgact_fix_put32(p + 4,  flags);    /* p_flags */
	imgact_fix_put64(p + 8,  off);      /* p_offset */
	imgact_fix_put64(p + 16, vaddr);    /* p_vaddr */
	imgact_fix_put64(p + 24, vaddr);    /* p_paddr */
	imgact_fix_put64(p + 32, filesz);   /* p_filesz */
	imgact_fix_put64(p + 40, filesz);   /* p_memsz */
	imgact_fix_put64(p + 48, 0x1000);   /* p_align */
}

/*
 * Build the fixture image into out[0..cap). Returns its total length
 * (IMGACT_FIX_TOTAL) or 0 if cap is too small.
 */
static inline size_t imgact_acp_fixture_elf_build(uint8_t *out, size_t cap)
{
	unsigned i;

	if (cap < IMGACT_FIX_TOTAL)
		return 0;
	memset(out, 0, IMGACT_FIX_TOTAL);

	/* ELF64 header. */
	out[0] = 0x7f; out[1] = 'E'; out[2] = 'L'; out[3] = 'F';
	out[4] = 2;    /* EI_CLASS = ELFCLASS64 */
	out[5] = 1;    /* EI_DATA  = ELFDATA2LSB */
	out[6] = 1;    /* EI_VERSION */
	imgact_fix_put16(out + 16, 3);            /* e_type   = ET_DYN */
	imgact_fix_put16(out + 18, 62);           /* e_machine = EM_X86_64 (nominal) */
	imgact_fix_put32(out + 20, 1);            /* e_version */
	imgact_fix_put64(out + 24, IMGACT_FIX_SEG0_VADDR); /* e_entry */
	imgact_fix_put64(out + 32, IMGACT_FIX_PHOFF);      /* e_phoff */
	imgact_fix_put64(out + 40, 0);            /* e_shoff (none) */
	imgact_fix_put32(out + 48, 0);            /* e_flags */
	imgact_fix_put16(out + 52, IMGACT_FIX_EHSZ);   /* e_ehsize */
	imgact_fix_put16(out + 54, IMGACT_FIX_PHENT);  /* e_phentsize */
	imgact_fix_put16(out + 56, IMGACT_FIX_PHNUM);  /* e_phnum */
	imgact_fix_put16(out + 58, 0);            /* e_shentsize */
	imgact_fix_put16(out + 60, 0);            /* e_shnum */
	imgact_fix_put16(out + 62, 0);            /* e_shstrndx */

	/* Two PT_LOAD program headers. */
	imgact_fix_phdr(out + IMGACT_FIX_PHOFF,
			5 /* R+X */, IMGACT_FIX_SEG0_OFF,
			IMGACT_FIX_SEG0_VADDR, IMGACT_FIX_SEG0_SZ);
	imgact_fix_phdr(out + IMGACT_FIX_PHOFF + IMGACT_FIX_PHENT,
			6 /* R+W */, IMGACT_FIX_SEG1_OFF,
			IMGACT_FIX_SEG1_VADDR, IMGACT_FIX_SEG1_SZ);

	/* Segment contents. */
	for (i = 0; i < IMGACT_FIX_SEG0_SZ; i++)
		out[IMGACT_FIX_SEG0_OFF + i] = imgact_fix_seg0_byte(i);
	for (i = 0; i < IMGACT_FIX_SEG1_SZ; i++)
		out[IMGACT_FIX_SEG1_OFF + i] = imgact_fix_seg1_byte(i);

	return IMGACT_FIX_TOTAL;
}

#endif /* IMGACT_ACP_FIXTURE_ELF_H */
