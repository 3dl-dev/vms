/*
 * consumer_main.c -- rd vms-d4a (vms-404 P4): the VAX cross-shareable
 * activation proof image, CONSUMER.EXE.
 *
 * Linked by LINKVAX.EXE `--executable --allow-undefined --use LIBVMS$SHR.EXE`
 * (tools/cross-vax/build-shr-activation-vax.sh) into an elf32-vax ET_DYN with
 * PT_INTERP=/run/ovmx-boot/IMGACT.EXE (never ld.elf_so) whose SINGLE `.vms$imp`
 * cross-image import is `purdy_s_hash` from the SHIPPED LIBVMS$SHR.EXE
 * (tools/cross-vax/build-vax-shareable-graph.sh, rd vms-c7f7). Its entry is
 * start_vax.S's freestanding `_start`; the NetBSD/vax libc.a is pulled
 * statically for printf/_exit, so purdy_s_hash is the only cross-shareable bind.
 *
 * THE VALUE-SENSITIVE ASSERTION. purdy_s_hash (src/libvms/rtl/purdy.c) is the
 * VMS Purdy password hash, imported through the shareable's `.vms$sv` -- not
 * computed inline, not statically linked. Called with the "VAX V1" vector
 * captured from a real OpenVMS VAX V7.3 system (docs/oracle/purdy-hash-
 * vectors.md; also run natively on this ILP32 width by tests/lab-vax/guest/
 * vmspurdy.c, rd vms-b86), the golden 64-bit hash can only come back if
 * IMGACT.EXE resolved the import against LIBVMS$SHR.EXE's symbol vector,
 * filled the import cell, and the call ran. Any other value is printed as is.
 *
 * tests/lab-vax/SYSTARTUP_VMS_STATUS_PROOF.COM RUNs this image and
 * tests/lab-vax/run-boot.sh status-gate requires both the golden line below
 * and $STATUS %X00000001 (the image returns 0 only on the golden value; DCL
 * maps a nonzero POSIX exit to SS$_ABORT, %X0000002C).
 */
#include <stdio.h>
#include <stdint.h>

/* purdy_s_hash's RTL signature (src/libvms/include/purdy.h), restated so the
 * cross-compile pulls in none of that header's other machinery. */
extern uint64_t purdy_s_hash(const char *password, unsigned long pwlen,
			     const char *username, unsigned short salt);

/* Entered from start_vax.S's `_start`, which hands the return value to _exit. */
int consumer_body(void)
{
	static const char pw[]   = "KNOWNPW12";
	static const char user[] = "A1ORA";
	const unsigned short salt = 0x4D63u;
	static const uint64_t want = 0x716CBDC03C071C59ull;

	uint64_t got = purdy_s_hash(pw, sizeof(pw) - 1, user, salt);

	printf("OVMX-VAX-SHR-ACT: purdy=0x%08lx%08lx\n",
	       (unsigned long)(got >> 32), (unsigned long)(got & 0xffffffffu));
	/* _start calls _exit, which does not flush stdio. */
	fflush(stdout);
	return (got == want) ? 0 : 1;
}
