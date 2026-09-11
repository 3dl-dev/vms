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
 * THREE OUTPUT CHANNELS, ON PURPOSE (rd vms-d4a re-instrumentation #2). The
 * FIRST re-instrumentation printed the value over a bare write(2) to fd 1/2 and
 * a C-RTL printf()+fflush() -- and NEITHER reached the SIMH console (measured:
 * hit=0 crtl=0 raw=0), so it stayed UNKNOWABLE whether the cross-shareable call
 * returned the golden value. The root cause is a SUBSTRATE fact, not a stdio
 * fault: on NetBSD the kernel does NOT wire a process's fd 0/1/2 to the console
 * (unlike Linux, which opens /dev/console as init's fd 0/1/2) -- PID 1
 * (ovmx_init) has to open /dev/console ITSELF and dup2 it onto 0/1/2
 * (src/ovmx_init/ovmx_boot_netbsd.c ovmx_netbsd_wire_console). CONSUMER.EXE is
 * a fork()+execve() child activated SysV-flavor by IMGACT.EXE; its inherited
 * fd 1/2 are NOT the console, so a write to them lands nowhere the transcript
 * sees. A file-on-ODS-2 readback (the operator's first suggestion) is NOT open
 * to this image: the /vms VFS write passthrough is retired (Rule 9) and the
 * only sanctioned ODS-2 write is RMS $CREATE/$PUT over /dev/vms
 * (src/ovmx_init/opcom_kmsg.c) -- which a freestanding image whose ONLY
 * cross-image bind is purdy_s_hash cannot issue. So this cut writes the value
 * on the channel a NetBSD process ALWAYS has, regardless of its fd table:
 *   (0) "OVMX-VAX-SHR-ACT-CON: purdy=0x..." via a direct open("/dev/console")
 *       + write(2) -- the SIMH serial DEVICE, the exact open PID 1 uses to wire
 *       its own console; independent of how fd 1/2 were inherited. PRIMARY.
 *   (1) "OVMX-VAX-SHR-ACT-RAW: purdy=0x..." via bare write(2) to the inherited
 *       fd 1/2 -- kept as a fallback in case some CI fd wiring differs; and
 *   (2) "OVMX-VAX-SHR-ACT: purdy=0x..."     via C-RTL printf()+fflush().
 * INDEPENDENTLY, the return value below is value-sensitive: 0 IFF got==golden,
 * nonzero otherwise. On the fork()+execve() path DCL surfaces the child's 8-bit
 * exit code on the console verbatim -- "%DCL-E-ABORT, image ... exited with
 * error status %X000000NN" for nonzero, or $STATUS = SS$_NORMAL (%X00000001)
 * for zero (dcl_cmd_process.c) -- so the SYSTARTUP's "STATUS=" line is a SECOND,
 * console-proven golden witness even if every text channel above were to fail.
 * The gate (run-shr-activation-vax.sh) reads the golden value from ANY channel,
 * corroborates with the $STATUS witness, reports which carried it, and prints a
 * WRONG value VERBATIM (never hides a mismatch) as a real product finding.
 */
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>

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

/* Compose "<prefix>0x<16 hex>\n" into buf (no stdio), returning its length.
 * `prefix` ends at "purdy=" (the trailing "0x" is added here). Same
 * self-contained discipline as hex16: no printf, no malloc, no emutls init on
 * this path, so the console channel emits even if the C-RTL stdio path faults. */
static int compose_line(char *buf, const char *prefix, const char *hx)
{
	char *p = buf;
	const char *q = prefix;
	int i;
	while (*q)
		*p++ = *q++;
	*p++ = '0';
	*p++ = 'x';
	for (i = 0; i < 16; i++)
		*p++ = hx[i];
	*p++ = '\n';
	return (int)(p - buf);
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

	char line[64];
	int n;

	/* Channel (0), PRIMARY: the console DEVICE directly. On NetBSD the kernel
	 * does not wire a process's fd 0/1/2 to the console, and this SysV-activated
	 * fork()+execve() child did not inherit a console-wired fd 1/2 (measured
	 * hit=0 crtl=0 raw=0) -- but /dev/console is the SIMH serial and ANY process
	 * can open it, exactly as PID 1 does to wire its own stdio
	 * (src/ovmx_init/ovmx_boot_netbsd.c). O_NOCTTY: do not steal it as our
	 * controlling terminal. This is the channel the gate matches first. */
	{
		int cfd = open("/dev/console", O_WRONLY | O_NOCTTY);
		if (cfd >= 0) {
			n = compose_line(line, "OVMX-VAX-SHR-ACT-CON: purdy=", hx);
			(void)write(cfd, line, (size_t)n);
			if (cfd > 2)
				(void)close(cfd);
		}
	}

	/* Channel (1): a bare write(2) to the inherited fd 1/2, no stdio -- kept as
	 * a fallback in case a CI fd wiring differs from the measured case. */
	n = compose_line(line, "OVMX-VAX-SHR-ACT-RAW: purdy=", hx);
	(void)write(1, line, (size_t)n);
	(void)write(2, line, (size_t)n);

	/* Channel (2): C-RTL SYS$OUTPUT via printf()+fflush() -- the shape
	 * src/apps/rctest/rc3.c uses; another fallback. */
	printf("OVMX-VAX-SHR-ACT: purdy=0x%s\n", hx);
	fflush(stdout);

	static const uint64_t want = 0x716CBDC03C071C59ull;
	return (got == want) ? 0 : 1;
}
