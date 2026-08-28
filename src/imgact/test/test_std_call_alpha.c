/*
 * test_std_call_alpha.c -- qemu-alpha proof that imgact_vms_transfer issues a
 * correct OpenVMS Alpha STANDARD CALL to a VMS transfer address (bead vms-f60d).
 *
 * Links the REAL trampoline (arch/alpha/vms_transfer.S) with a capture stub
 * (std_call_stub_alpha.S) standing in for the port crt0's __main. Proves:
 *   (a) the six activation-context args land in R16..R21 in order;
 *   (b) R25 == AI, built by the documented Argument-Information layout;
 *   (c) R27 == PV == the PROCEDURE DESCRIPTOR address -- the VMS-native Alpha
 *       convention (vms-04e, corrected from vms-f60d): the trampoline jumps to
 *       the entry = *(PV+8) but leaves R27 = the descriptor, so the real
 *       alpha-dec-vms port __main reaches its linkage pairs GP-relative FROM
 *       R27 = PDSC (the CHECK fails on a regression to R27 = entry);
 *   (d) R26 (RA) points back INTO the trampoline -- control RETURNS to IMGACT
 *       (unlike _start's tail-jump), which is what lets IMGACT map the result;
 *   (e) the value the stub returned in R0 is delivered back to the caller.
 *
 * Runs under user-mode qemu-alpha (no /dev/vms, no full activation needed).
 * Exit 0 on success, 1 on first failure.
 */
#include "ovmx_activation.h"   /* OVMX_AI_VMS_ACTIVATION */

#include <stdio.h>
#include <stdint.h>

/* The real trampoline (arch/alpha/vms_transfer.S). */
unsigned long imgact_vms_transfer(void *pv, unsigned long ai,
				  const unsigned long args[6]);

/* The capture stub's entry code + its nine-quadword capture buffer
 * (std_call_stub_alpha.S). The stub is a real ELF-Alpha procedure: it ldgp's
 * from R27 and reaches std_capture GP-relative, so it stands in for the port
 * crt0's __main faithfully (R27 = entry, not descriptor). */
extern void std_stub_entry(void);
extern uint64_t std_capture[9];

static int failures = 0;
#define CHECK(cond, msg) do {                       \
    if (cond) { printf("PASS: %s\n", msg); }        \
    else { printf("FAIL: %s\n", msg); failures++; } \
} while (0)

int main(void)
{
	/* The stub fills its own global capture buffer (std_capture), reached via a
	 * SELF-established GP (not from R27) -- so R27 is captured verbatim and the
	 * test asserts it is the DESCRIPTOR address (vms-04e ground-truth). */
	uint64_t *capture = std_capture;

	/* Procedure descriptor: entry code at +8 (PDSC$Q_ENTRY). The trampoline
	 * reads *(PV+8) as the entry to jump to, and enters it with R27 = PV = the
	 * DESCRIPTOR address (the pointer we pass here), per the Alpha calling
	 * standard and the real alpha-dec-vms port __main (vms-04e). */
	static uint64_t pdsc[2];
	pdsc[0] = 0;
	pdsc[1] = (uint64_t)(uintptr_t)&std_stub_entry;   /* PDSC$Q_ENTRY */

	/* Six distinct sentinels for R16..R21. */
	const unsigned long args[6] = {
		0x1111111111111111UL, 0x2222222222222222UL,
		0x3333333333333333UL, 0x4444444444444444UL,
		0x5555555555555555UL, 0x6666666666666666UL,
	};
	const unsigned long ai = OVMX_AI_VMS_ACTIVATION;

	unsigned long ret = imgact_vms_transfer(pdsc, ai, args);

	CHECK(capture[0] == args[0], "R16 == args[0] (progxfer)");
	CHECK(capture[1] == args[1], "R17 == args[1] (cli_util)");
	CHECK(capture[2] == args[2], "R18 == args[2] (imghdr)");
	CHECK(capture[3] == args[3], "R19 == args[3] (image_file_desc)");
	CHECK(capture[4] == args[4], "R20 == args[4] (linkflag)");
	CHECK(capture[5] == args[5], "R21 == args[5] (cliflag)");
	CHECK(capture[6] == ai,      "R25 == AI (argument information)");
	CHECK(capture[6] == 6,       "R25 value == 6 (six 64-bit args, AI$K_AR_I64)");
	CHECK(capture[7] == (uint64_t)(uintptr_t)pdsc,
	      "R27 == PV = the PROCEDURE DESCRIPTOR address (VMS-native; callee reaches "
	      "its linkage pairs GP-relative FROM R27=PDSC)");

	/* (d) RA must point back into the trampoline, proving the call RETURNS to
	 * IMGACT. The trampoline is a small routine; the return address sits just
	 * a few instructions past its entry. Bound it generously (< 4 KB). */
	uint64_t ra    = capture[8];
	uint64_t tramp = (uint64_t)(uintptr_t)&imgact_vms_transfer;
	CHECK(ra != 0, "R26 (RA) is nonzero");
	CHECK(ra > tramp && ra < tramp + 4096,
	      "R26 (RA) points back INTO imgact_vms_transfer (returns to IMGACT)");

	/* (e) the stub returned 0x0BAD in R0; it must reach the caller. */
	CHECK(ret == 0x0BADUL, "return value (R0) delivered to caller == 0x0BAD");

	if (failures) {
		printf("\n%d CHECK(s) FAILED\n", failures);
		return 1;
	}
	printf("\nALPHA STANDARD-CALL PROOF PASSED "
	       "(R16-R21 args, R25 AI, R27 PV, R26 RA, R0 return)\n");
	return 0;
}
