/*
 * test_syssvc_imgact_acp.c - IMGACT activates an image by reading its ELF header
 * and PT_LOAD segments over the executive Files-11 (ODS-2) ACP -- IO$_ACCESS +
 * IO$_READVBLK on a channel $ASSIGNed to a mounted volume, NOT open()/pread()
 * on a /vms POSIX path (vms-3e8e, rung of epic vms-208), proven against a real
 * /dev/vms.
 *
 * This drives the EXACT freestanding reader IMGACT.EXE runs: src/imgact/
 * imgact_acp.c is compiled into this test unchanged (CMakeLists), and the only
 * seam that differs -- the three host primitives imgact_acp_dev_{open,close,
 * ioctl} -- is provided below on libc instead of raw syscalls. So a pass here
 * is a pass of the activator's real ACP path, not a re-implementation.
 *
 * WHAT THIS PROVES, against a real /dev/vms over the generated ODS-2 fixture the
 * harness seeds on DKA400: (vde, from tests/qemu/mkimage_ods2_imgact.c):
 *
 *   1. OPEN BY FILESPEC + DIRECTORY WALK. imgact_acp_open("DKA400:",
 *      "/IMGACT/TESTIMG.EXE") $ASSIGNs a file-class channel, walks [IMGACT] as
 *      an ODS-2 directory (IO$_ACCESS "IMGACT.DIR", DID-chaining to its FID),
 *      then IO$_ACCESSes TESTIMG.EXE -- resolving the real image file and its
 *      valid-byte count (1424) off the on-disk FH2.
 *
 *   2. IMAGE HEADER via IO$_READVBLK. The ELF64 header read at byte offset 0 is
 *      well-formed (magic, ELFCLASS64) and carries the fixture's two program
 *      headers -- byte-for-byte the on-disk header.
 *
 *   3. PT_LOAD SEGMENTS via IO$_READVBLK. Each PT_LOAD's p_filesz bytes, read at
 *      its p_offset exactly as load_object() maps them, are byte-identical to
 *      the committed fixture (the builder's own output, imgact_acp_fixture_elf.h).
 *
 *   4. WHOLE IMAGE BYTE-EXACT. The entire file read back over the window equals
 *      the golden image -- the read-then-map first cut's core guarantee.
 *
 *   5. FAIL-HONEST, NO POSIX FALLBACK (INV-6). A name not on the volume is
 *      SS$_NOSUCHFILE; a unit that is not an ACP-mounted volume is
 *      SS$_NOSUCHDEV -- never a silent read off a /vms POSIX tree.
 *
 * NO /dev/vms -> honest SKIP (77): the ACP, the volume mount, the file window
 * and the transfer are all executive-resident, so with no /dev/vms there is
 * nothing to assert (the contract every test_syssvc_* suite is held to).
 *
 * GROUND TRUTH. The image bytes are the deterministic fixture ELF the builder
 * laid down; this test rebuilds the identical bytes in memory as the golden, so
 * every assertion below is against the fixture's own output, not values this
 * test invents.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <elf.h>

#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "imgact_acp.h"
#include "imgact_acp_fixture_elf.h"

#define EXIT_SKIP 77

/* DKA400: (vde) carries the generated ODS-2 fixture with [IMGACT]TESTIMG.EXE. */
#define ODS2_UNIT   "DKA400:"
#define IMG_PATH    "/IMGACT/TESTIMG.EXE"

/* --------------------------------------------------------------------------
 * Host primitives (the freestanding/hosted seam). IMGACT.EXE backs these with
 * raw syscall6(); here they are libc, so imgact_acp.c runs unchanged.
 * -------------------------------------------------------------------------- */
int imgact_acp_dev_open(void)
{
	return open("/dev/vms", O_RDWR);
}
void imgact_acp_dev_close(int fd)
{
	if (fd >= 0)
		close(fd);
}
long imgact_acp_dev_ioctl(int fd, unsigned long req, void *arg)
{
	return ioctl(fd, req, arg) < 0 ? -1 : 0;
}

static int pass = 0;
static int fail = 0;

static void check(int cond, const char *name)
{
	if (cond) { printf("  PASS: %s\n", name); pass++; }
	else      { printf("  FAIL: %s\n", name); fail++; }
}

static int executive_present(void)
{
	int fd = vms_kif_open();
	if (fd < 0)
		return 0;
	vms_kif_close();
	return 1;
}

int main(void)
{
	struct imgact_acp_file f;
	uint32_t st;
	uint8_t golden[IMGACT_FIX_TOTAL];
	uint8_t buf[IMGACT_FIX_TOTAL];
	Elf64_Ehdr eh;
	Elf64_Phdr ph[8];
	long got;
	int i;

	setvbuf(stdout, NULL, _IOLBF, 0);

	printf("=== test_syssvc_imgact_acp: IMGACT reads an image (header + PT_LOAD) over "
	       "the Files-11 ACP IO$_ACCESS+IO$_READVBLK (vms-3e8e, epic vms-208) ===\n");

	if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
		printf("  FAIL: vms_pcb_init() failed\n");
		return 1;
	}

	if (!executive_present()) {
		printf("=== test_syssvc_imgact_acp: 0 passed, 0 failed (SKIPPED: no /dev/vms -- "
		       "the ACP, the mount, the file window and the transfer are executive-resident) ===\n");
		return EXIT_SKIP;
	}

	/* The committed golden: the builder's own fixture image, rebuilt here. */
	if (imgact_acp_fixture_elf_build(golden, sizeof(golden)) != IMGACT_FIX_TOTAL) {
		printf("  FAIL: fixture golden build\n");
		return 1;
	}

	/* Precondition: the boot-time $MOUNT PID 1 will do for SYS$DISK. IMGACT
	 * itself never mounts -- it only $ASSIGNs -- so the mount is the harness's
	 * (as it is the executive's job in the real boot the atomic flip lands). */
	st = vms_kif_acp_mount(ODS2_UNIT);
	check($VMS_STATUS_SUCCESS(st),
	      "$MOUNT of the generated ODS-2 " ODS2_UNIT " (precondition)");
	if (!$VMS_STATUS_SUCCESS(st)) {
		printf("=== test_syssvc_imgact_acp: %d passed, %d failed ===\n", pass, fail);
		return 1;
	}

	/* --- (1) open by filespec: $ASSIGN + directory walk + IO$_ACCESS ------ */
	st = imgact_acp_open(&f, ODS2_UNIT, IMG_PATH);
	check($VMS_STATUS_SUCCESS(st),
	      "imgact_acp_open walks [IMGACT] and IO$_ACCESSes TESTIMG.EXE over the ACP");
	/* negctl: imgact-acp-valid-bytes-offbyone */
	check($VMS_STATUS_SUCCESS(st) && f.valid == IMGACT_FIX_TOTAL,
	      "the accessed image's valid-byte count (1424) matches the on-disk FH2");
	if (!$VMS_STATUS_SUCCESS(st)) {
		printf("=== test_syssvc_imgact_acp: %d passed, %d failed ===\n", pass, fail);
		return 1;
	}

	/* --- (2) image header via IO$_READVBLK -------------------------------- */
	got = imgact_acp_pread(&f, &eh, sizeof(eh), 0);
	if (eh.e_phnum > 8)     /* guard: a garbage header must not overflow ph[] */
		eh.e_phnum = 8;
	check(got == (long)sizeof(eh) &&
	      eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' &&
	      eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F' &&
	      eh.e_ident[EI_CLASS] == ELFCLASS64,
	      "IO$_READVBLK reads a well-formed ELF64 header at offset 0");
	check(got == (long)sizeof(eh) && eh.e_phnum == IMGACT_FIX_PHNUM &&
	      eh.e_phoff == IMGACT_FIX_PHOFF,
	      "the header carries the fixture's two program headers (e_phnum=2, e_phoff=64)");
	check(got == (long)sizeof(eh) && memcmp(&eh, golden, sizeof(eh)) == 0,
	      "the ACP-read ELF header is BYTE-EXACT vs the on-disk image");

	/* --- (3) each PT_LOAD segment via IO$_READVBLK ------------------------ */
	got = imgact_acp_pread(&f, ph,
			       (unsigned long)eh.e_phnum * sizeof(Elf64_Phdr),
			       (long)eh.e_phoff);
	check(got == (long)((long)eh.e_phnum * (long)sizeof(Elf64_Phdr)),
	      "IO$_READVBLK reads the program-header table");
	check(memcmp(ph, golden + eh.e_phoff,
		     (size_t)eh.e_phnum * sizeof(Elf64_Phdr)) == 0,
	      "the ACP-read program-header table is BYTE-EXACT vs the on-disk image");

	{
		int loads = 0, seg_ok = 1;
		for (i = 0; i < (int)eh.e_phnum; i++) {
			uint8_t seg[IMGACT_FIX_TOTAL];
			if (ph[i].p_type != PT_LOAD || ph[i].p_filesz == 0)
				continue;
			if (ph[i].p_filesz > IMGACT_FIX_TOTAL ||
			    ph[i].p_offset > IMGACT_FIX_TOTAL) {   /* garbage guard */
				seg_ok = 0;
				continue;
			}
			loads++;
			got = imgact_acp_pread(&f, seg, ph[i].p_filesz,
					       (long)ph[i].p_offset);
			if (got != (long)ph[i].p_filesz ||
			    memcmp(seg, golden + ph[i].p_offset,
				   (size_t)ph[i].p_filesz) != 0)
				seg_ok = 0;
		}
		check(loads == 2, "the image has two PT_LOAD segments (as load_object walks them)");
		check(seg_ok,
		      "every PT_LOAD's p_filesz bytes, read at p_offset via IO$_READVBLK, "
		      "are BYTE-EXACT vs the on-disk image");
	}

	/* --- (4) whole image byte-exact over the window ----------------------- */
	got = imgact_acp_pread(&f, buf, IMGACT_FIX_TOTAL, 0);
	check(got == (long)IMGACT_FIX_TOTAL &&
	      memcmp(buf, golden, IMGACT_FIX_TOTAL) == 0,
	      "the whole image read back over the ACP window is BYTE-EXACT vs the golden");

	imgact_acp_close(&f);

	/* --- (5) fail-honest: no POSIX fallback ------------------------------- */
	{
		struct imgact_acp_file nf;
		st = imgact_acp_open(&nf, ODS2_UNIT, "/IMGACT/NOSUCH.EXE");
		check(st == SS$_NOSUCHFILE,
		      "an image name not on the volume is SS$_NOSUCHFILE (no POSIX fallback, INV-6)");
		imgact_acp_close(&nf);

		st = imgact_acp_open(&nf, "DKA999:", IMG_PATH);
		check(st == SS$_NOSUCHDEV,
		      "a unit that is not an ACP-mounted volume is SS$_NOSUCHDEV (no POSIX fallback, INV-6)");
		imgact_acp_close(&nf);
	}

	printf("=== test_syssvc_imgact_acp: %d passed, %d failed ===\n", pass, fail);
	return fail == 0 ? 0 : 1;
}
