/*
 * consumer_main.c -- vms-d4a (vms-404 P4) the ANTI-LARP RUNTIME proof consumer.
 *
 * Linked by LINKVAX.EXE `--executable --use LIBVMS$SHR.EXE --use
 * P4BOOT$SHR.EXE` into CONSUMER.EXE, a genuine elf32-vax ET_DYN with
 * PT_INTERP=IMGACT.EXE (never ld.elf_so) and a `.vms$imp` that imports
 * `purdy_s_hash` from the SHIPPED LIBVMS$SHR.EXE (tools/cross-vax/
 * build-vax-shareable-graph.sh, rd vms-c7f7) plus `exit`/`p4boot_puts` from
 * this gate's tiny runtime shim (p4boot_rt.c).
 *
 * THE VALUE-SENSITIVE ASSERTION. purdy_s_hash (src/libvms/rtl/purdy.c) is the
 * REAL VMS Purdy password hash -- not a synthetic stand-in. Called here with
 * the exact "VAX V1" real-OpenVMS-oracle vector (docs/oracle/
 * purdy-hash-vectors.md, also run natively on this same ILP32 width by
 * tests/lab-vax/guest/vmspurdy.c / rd vms-b86), the ONLY way this prints the
 * expected 64-bit hash is if IMGACT.EXE genuinely resolved the `.vms$imp`
 * against the shipped LIBVMS$SHR.EXE's `.vms$sv` symbol vector, the PLT/GOT
 * import cell was filled correctly, and the call ran to completion on real
 * VAX silicon (SIMH) -- a corrupted resolution, a wrong producer, a stale
 * import cell, or a miscompiled call would print some OTHER 64-bit value (or
 * nothing at all, or a crash), astronomically unlikely to coincide with the
 * golden vector. This mirrors the Alpha shipped-shareable gate's
 * $STATUS=%X0035A019 sentinel discipline (rd vms-410/#1075): a value that
 * only appears if the real cross-shareable call really ran.
 *
 * Freestanding (like the P3a/vms-099 readelf-shape test's fixtures): no libc,
 * -ffreestanding -fno-builtin, so the ONLY external calls this object makes
 * are the two imported PROCEDURE universals (purdy_s_hash, p4boot_puts) plus
 * the crt0's own tail-call to `exit`.
 */
#include <stdint.h>

/* purdy_s_hash's genuine RTL signature (src/libvms/include/purdy.h),
 * restated here rather than #included -- the freestanding cross-compile has
 * no reason to pull in <stddef.h>'s size_t machinery for one prototype. */
extern uint64_t purdy_s_hash(const char *password, unsigned long pwlen,
			     const char *username, unsigned short salt);

/* Imported from P4BOOT$SHR.EXE (this gate's tiny runtime shim producer). */
extern long p4boot_puts(const char *s);

/* Format a 64-bit value as 16 lowercase hex digits + NUL. No libc: this is
 * freestanding, same discipline as IMGACT.EXE's own imgact_u32_hex8. */
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

int main(int argc, char **argv)
{
	(void)argc;
	(void)argv;

	/* The real OpenVMS VAX V7.3 AUTHORIZE oracle vector "VAX V1"
	 * (docs/oracle/purdy-hash-vectors.md; tests/lab-vax/guest/vmspurdy.c
	 * proves the SAME vector on this SAME ILP32 width, natively, rd
	 * vms-b86). want = 0x716CBDC03C071C59. */
	static const char pw[]   = "KNOWNPW12";
	static const char user[] = "A1ORA";
	const unsigned short salt = 0x4D63u;

	uint64_t got = purdy_s_hash(pw, sizeof(pw) - 1, user, salt);

	/* "OVMX-VAX-SHR-ACT: purdy=0x<16 lowercase hex digits>\n" -- built by
	 * hand (no snprintf; freestanding) into a fixed buffer, then written
	 * whole in one p4boot_puts() call over a REAL write(2) into the SIMH
	 * console transcript. */
	char line[64];
	char *p = line;
	static const char pfx[] = "OVMX-VAX-SHR-ACT: purdy=0x";
	const char *q = pfx;
	while (*q)
		*p++ = *q++;
	hex16(got, p);
	p += 16;
	*p++ = '\n';
	*p = '\0';
	p4boot_puts(line);

	static const uint64_t want = 0x716CBDC03C071C59ull;
	return (got == want) ? 0 : 1;
}
