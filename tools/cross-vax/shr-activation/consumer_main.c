/*
 * consumer_main.c -- vms-d4a (vms-404 P4) the ANTI-LARP RUNTIME proof consumer.
 *
 * Linked by LINKVAX.EXE `--executable --allow-undefined --use LIBVMS$SHR.EXE`
 * (build-shr-activation-vax.sh) into CONSUMER.EXE, a genuine elf32-vax ET_DYN
 * with PT_INTERP=IMGACT.EXE (never ld.elf_so) whose SINGLE `.vms$imp`
 * cross-image import is `purdy_s_hash` from the SHIPPED LIBVMS$SHR.EXE
 * (tools/cross-vax/build-vax-shareable-graph.sh, rd vms-c7f7). Its own entry
 * is start_vax.S's freestanding `_start` (see that file for why no crt0); the
 * NetBSD/vax libc.a is pulled statically for printf/write/_exit, so those are
 * NOT imports -- purdy_s_hash is the only cross-shareable bind, which is the
 * whole point of the gate.
 *
 * THE VALUE-SENSITIVE ASSERTION. purdy_s_hash (src/libvms/rtl/purdy.c) is the
 * REAL VMS Purdy password hash -- not a synthetic stand-in -- imported from
 * the shipped shareable's `.vms$sv`, NOT computed inline and NOT statically
 * linked. Called here with the exact "VAX V1" real-OpenVMS-oracle vector
 * (docs/oracle/purdy-hash-vectors.md; also run natively on this same ILP32
 * width by tests/lab-vax/guest/vmspurdy.c / rd vms-b86), the ONLY way this
 * prints the golden 64-bit hash is if IMGACT.EXE genuinely resolved the
 * `.vms$imp` against the shipped LIBVMS$SHR.EXE's `.vms$sv`, filled the
 * PLT/GOT import cell, and the call ran to completion on real VAX silicon
 * (SIMH). A corrupted resolution, a wrong producer, a stale import cell, or a
 * miscompiled call prints some OTHER 64-bit value -- which this consumer
 * prints VERBATIM (it never hides a mismatch) so the gate can report it as a
 * REAL product finding rather than a silent red.
 *
 * TWO OUTPUT CHANNELS, ON PURPOSE (rd vms-d4a re-instrumentation). The prior
 * cut printed the value over a raw write(2) from a gate-private producer and
 * the line never reached the SIMH console -- leaving it UNKNOWABLE whether the
 * cross-shareable call ran (only the fork()-path POSIX exit code surfaced, and
 * it was nonzero). This cut prints the value TWICE, lowest-risk first:
 *   (1) "OVMX-VAX-SHR-ACT-RAW: purdy=0x..."  via a bare write(2) syscall --
 *       self-contained, no stdio/emutls/malloc init, so it emits even if the
 *       C-RTL stdio path faults during its first-call buffer setup; and
 *   (2) "OVMX-VAX-SHR-ACT: purdy=0x..."      via C-RTL printf()+fflush(),
 *       the channel src/apps/rctest/rc3.c proves RUN routes to the console.
 * The gate (run-shr-activation-vax.sh) reads the golden value from EITHER
 * line and reports which channel carried it -- so the run is diagnostic no
 * matter which of the two the substrate delivers, and the value-sensitive
 * teeth bite identically.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

/* purdy_s_hash's genuine RTL signature (src/libvms/include/purdy.h),
 * restated here rather than #included -- the cross-compile has no reason to
 * pull in that header's other machinery for one prototype. */
extern uint64_t purdy_s_hash(const char *password, unsigned long pwlen,
			     const char *username, unsigned short salt);

/* start_vax.S's `_start` tail-calls libc _exit(2) with our return value. */
extern void _exit(int);

/* Format a 64-bit value as 16 lowercase hex digits + NUL, by hand (the raw
 * channel must not depend on printf; same discipline as IMGACT's own
 * imgact_u32_hex8). */
static void hex16(uint64_t v, char *buf)
{
	static const char d[] = "0123456789abcdef";
	int i;
	for (i = 15; i >= 0; i--) {
		buf[i] = d[v & 0xful];
		v >>= 4;
	}
	buf[16] = '\0';
}

/* Entered via start_vax.S's `_start` (a real CALLS frame from here down).
 * Returns 0 iff the cross-shareable purdy_s_hash returned the golden vector;
 * nonzero otherwise (start_vax.S hands it to _exit -> DCL's $STATUS). */
int consumer_body(void)
{
	/* The real OpenVMS VAX V7.3 AUTHORIZE oracle vector "VAX V1"
	 * (docs/oracle/purdy-hash-vectors.md; tests/lab-vax/guest/vmspurdy.c
	 * proves the SAME vector on this SAME ILP32 width, natively, rd
	 * vms-b86). want = 0x716CBDC03C071C59. */
	static const char pw[]   = "KNOWNPW12";
	static const char user[] = "A1ORA";
	const unsigned short salt = 0x4D63u;

	uint64_t got = purdy_s_hash(pw, sizeof(pw) - 1, user, salt);

	char hx[17];
	hex16(got, hx);

	/* Channel (1): a bare write(2) syscall, no stdio -- built into a fixed
	 * buffer and emitted whole, so it survives even a C-RTL init fault. */
	{
		char raw[64];
		char *p = raw;
		static const char pfx[] = "OVMX-VAX-SHR-ACT-RAW: purdy=0x";
		const char *q = pfx;
		int i;
		while (*q)
			*p++ = *q++;
		for (i = 0; i < 16; i++)
			*p++ = hx[i];
		*p++ = '\n';
		(void)write(1, raw, (size_t)(p - raw));
		(void)write(2, raw, (size_t)(p - raw));
	}

	/* Channel (2): C-RTL SYS$OUTPUT via printf()+fflush() -- the exact
	 * shape src/apps/rctest/rc3.c uses to prove RUN routes a child's
	 * stdout to the console. This is the line run-shr-activation-vax.sh's
	 * EXPECT_LINE matches first. */
	printf("OVMX-VAX-SHR-ACT: purdy=0x%s\n", hx);
	fflush(stdout);

	static const uint64_t want = 0x716CBDC03C071C59ull;
	return (got == want) ? 0 : 1;
}
