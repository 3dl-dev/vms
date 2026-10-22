// SPDX-License-Identifier: GPL-2.0
/*
 * test_rms_lockname.c - the RMS file-access lock's name, checked BYTE-FOR-BYTE
 * against frames real OpenVMS VAX nodes put on the wire (rd vms-326).
 *
 * R1 host unit: src/kernel-core/vms_rms_lockname.h is pure and header-only, so
 * this compiles it with a plain host compiler -- no kernel, no boot, no seam.
 *
 * WHAT MAKES THIS A PROOF AND NOT A RESTATEMENT. Every fixture below is a real
 * 26-byte name lifted out of a committed capture, with its capture file and
 * frame number, together with the file id and volume label the SAME bytes
 * decode to. The builder is handed the id and the label and must reproduce the
 * 26 bytes exactly. A builder that agreed with its own idea of the form -- the
 * way the predecessor name did -- cannot pass: the expected bytes are the
 * VAX's.
 *
 * The population these come from: 1,130 real cat-0x02 requests across the five
 * name-carrying opcodes, every one 26 bytes of this shape at UIC group 0 /
 * access mode 1 (tools/cluster/dlm_rms_name_hash_check.py --selftest, ctest
 * cluster_host_dlm_rms_name_hash), of which 47 names are distinct.
 */

#include "cluster_test.h"

#include "vms_rms_lockname.h"

#include <stdio.h>
#include <string.h>

/*
 * One real name off the wire. `bytes` is what the frame carried at
 * body[48:74]; the id and label are what those bytes decode to, and are what
 * the ACP holds at IO$_ACCESS time.
 */
struct rms_name_fixture {
	const char *capture;      /* the committed capture it came from       */
	unsigned    frame;        /* ...and the frame number in it            */
	uint16_t    fid_num;
	uint16_t    fid_seq;
	uint8_t     fid_rvn;
	uint8_t     fid_nmx;
	const char *label;        /* the ODS-2 volume label, unpadded         */
	uint8_t     bytes[VMS_RMS_LOCKNAME_LEN];
};

/*
 * Eight of the 47 distinct real names, spanning TWO volumes (a 7-character
 * label and an 8-character one, so the space padding is exercised at two
 * widths), four-digit and five-digit file sequences, and both captures that
 * carry them.
 */
static const struct rms_name_fixture fixtures[] = {
	{ "rejoin-rebuild.pcap", 2510, 284, 1, 0, 0, "SYSDSK1",
	  { 0x52,0x4d,0x53,0x24, 0x1c,0x01, 0x01,0x00, 0x00,0x00, 0x02,
	    0x53,0x59,0x53,0x44,0x53,0x4b,0x31,0x20,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
	{ "rejoin-rebuild.pcap", 7811, 2330, 2, 0, 0, "SYSDSK1",
	  { 0x52,0x4d,0x53,0x24, 0x1a,0x09, 0x02,0x00, 0x00,0x00, 0x02,
	    0x53,0x59,0x53,0x44,0x53,0x4b,0x31,0x20,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
	{ "rejoin-rebuild.pcap", 12759, 302, 1, 0, 0, "SYSDSK1",
	  { 0x52,0x4d,0x53,0x24, 0x2e,0x01, 0x01,0x00, 0x00,0x00, 0x02,
	    0x53,0x59,0x53,0x44,0x53,0x4b,0x31,0x20,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
	{ "rejoin-rebuild.pcap", 11894, 2352, 2, 0, 0, "SYSDSK1",
	  { 0x52,0x4d,0x53,0x24, 0x30,0x09, 0x02,0x00, 0x00,0x00, 0x02,
	    0x53,0x59,0x53,0x44,0x53,0x4b,0x31,0x20,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
	{ "wire.pcap.gz", 4511, 540, 10601, 0, 0, "VAX1DATA",
	  { 0x52,0x4d,0x53,0x24, 0x1c,0x02, 0x69,0x29, 0x00,0x00, 0x02,
	    0x56,0x41,0x58,0x31,0x44,0x41,0x54,0x41,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
	{ "wire.pcap.gz", 4453, 540, 10602, 0, 0, "VAX1DATA",
	  { 0x52,0x4d,0x53,0x24, 0x1c,0x02, 0x6a,0x29, 0x00,0x00, 0x02,
	    0x56,0x41,0x58,0x31,0x44,0x41,0x54,0x41,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
	{ "wire.pcap.gz", 5325, 540, 10603, 0, 0, "VAX1DATA",
	  { 0x52,0x4d,0x53,0x24, 0x1c,0x02, 0x6b,0x29, 0x00,0x00, 0x02,
	    0x56,0x41,0x58,0x31,0x44,0x41,0x54,0x41,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
	{ "wire.pcap.gz", 5478, 540, 10604, 0, 0, "VAX1DATA",
	  { 0x52,0x4d,0x53,0x24, 0x1c,0x02, 0x6c,0x29, 0x00,0x00, 0x02,
	    0x56,0x41,0x58,0x31,0x44,0x41,0x54,0x41,0x20,0x20,0x20,0x20,
	    0x00,0x00,0x00 } },
};

#define N_FIXTURES ((unsigned)(sizeof(fixtures) / sizeof(fixtures[0])))

static void show(const char *what, const uint8_t *b, uint32_t n)
{
	uint32_t i;

	printf("    %s:", what);
	for (i = 0u; i < n; i++)
		printf(" %02x", b[i]);
	printf("\n");
}

/* ================================================================
 * 1. The builder reproduces real VAX frames, byte for byte.
 * ================================================================ */
static void matches_real_frames(void)
{
	unsigned f;

	printf("--- the 26 bytes a real VAX sent, rebuilt from the file id and "
	       "the volume label ---\n");
	for (f = 0u; f < N_FIXTURES; f++) {
		const struct rms_name_fixture *x = &fixtures[f];
		uint8_t out[VMS_RMS_LOCKNAME_LEN + 4];
		char what[160];
		uint32_t n;

		memset(out, 0xee, sizeof(out));
		n = vms_rms_lockname(x->fid_num, x->fid_seq, x->fid_rvn,
				     x->fid_nmx, x->label,
				     (uint32_t)strlen(x->label), out,
				     (uint32_t)sizeof(out));
		snprintf(what, sizeof(what),
			 "(%u,%u,%u,%u) on %s == %s frame %u",
			 (unsigned)x->fid_num, (unsigned)x->fid_seq,
			 (unsigned)x->fid_rvn, (unsigned)x->fid_nmx, x->label,
			 x->capture, x->frame);
		if (n != VMS_RMS_LOCKNAME_LEN) {
			ct_check(0, what);
			continue;
		}
		if (memcmp(out, x->bytes, VMS_RMS_LOCKNAME_LEN) != 0) {
			ct_check(0, what);
			show("built", out, VMS_RMS_LOCKNAME_LEN);
			show("wire ", x->bytes, VMS_RMS_LOCKNAME_LEN);
			continue;
		}
		ct_check(1, what);
		/* ...and nothing past the name was touched. */
		ct_check(out[VMS_RMS_LOCKNAME_LEN] == 0xeeu,
			 "  and it wrote exactly 26 bytes");
	}
}

/* ================================================================
 * 2. The properties that make the name a cluster-wide file identity.
 * ================================================================ */
static void two_volumes_two_resources(void)
{
	uint8_t a[VMS_RMS_LOCKNAME_LEN], b[VMS_RMS_LOCKNAME_LEN];

	printf("--- the volume is IN the name: the same file id on two volumes "
	       "is two resources ---\n");
	ct_check(vms_rms_lockname(284, 1, 0, 0, "SYSDSK1", 7u, a, sizeof(a)) ==
		 VMS_RMS_LOCKNAME_LEN, "the name on SYSDSK1");
	ct_check(vms_rms_lockname(284, 1, 0, 0, "VAX1DATA", 8u, b, sizeof(b)) ==
		 VMS_RMS_LOCKNAME_LEN, "...and on VAX1DATA");
	ct_check(memcmp(a, b, VMS_RMS_LOCKNAME_LEN) != 0,
		 "*** they DIFFER -- the collision rd vms-acf3 measured cannot "
		 "happen ***");
	ct_check(memcmp(a, b, VMS_RMS_LOCKNAME_LBL_OFF) == 0,
		 "...and they differ ONLY from the label on, so the file id "
		 "half is identical");

	printf("--- ...and the name CONTAINS NULs, which is why it must be "
	       "counted ---\n");
	ct_check(a[VMS_RMS_LOCKNAME_TAIL_OFF] == 0u &&
		 a[VMS_RMS_LOCKNAME_TAIL_OFF + 1u] == 0u &&
		 a[VMS_RMS_LOCKNAME_TAIL_OFF + 2u] == 0u,
		 "the three trailing NULs are there");
	ct_check(strnlen((const char *)a, VMS_RMS_LOCKNAME_LEN) <
		 VMS_RMS_LOCKNAME_LEN,
		 "*** a C string of this name is SHORTER than the name ***");
	{
		/* The two names a C string cannot tell apart: a file id whose
		 * sequence high byte is 0 ends the string at byte 7. */
		uint8_t c[VMS_RMS_LOCKNAME_LEN];
		size_t clen;

		ct_check(vms_rms_lockname(284, 1, 0, 0, "SYSDSK1", 7u, c,
					  sizeof(c)) == VMS_RMS_LOCKNAME_LEN,
			 "a name whose sequence is 1");
		clen = strnlen((const char *)c, VMS_RMS_LOCKNAME_LEN);
		ct_check(clen == 7u,
			 "*** its C-string length is SEVEN of twenty-six -- the "
			 "truncation that made every such file one resource ***");
	}
}

/* ================================================================
 * 3. The refusals. There is no second-best name (INV-6).
 * ================================================================ */
static void no_label_no_name(void)
{
	uint8_t out[VMS_RMS_LOCKNAME_LEN];
	char longlabel[32];

	printf("--- no volume label, no name: the executive invents none ---\n");
	memset(out, 0xaa, sizeof(out));
	ct_check_eq_u32(vms_rms_lockname(284, 1, 0, 0, NULL, 0u, out,
					 sizeof(out)), 0u,
			"a NULL label builds nothing");
	ct_check_eq_u32(vms_rms_lockname(284, 1, 0, 0, "SYSDSK1", 0u, out,
					 sizeof(out)), 0u,
			"a zero-length label builds nothing");
	ct_check(out[0] == 0xaau,
		 "*** and a refusal writes NOTHING -- no blank-label name ever "
		 "reaches a frame ***");

	memset(longlabel, 'X', sizeof(longlabel));
	longlabel[sizeof(longlabel) - 1] = '\0';
	ct_check_eq_u32(vms_rms_lockname(284, 1, 0, 0, longlabel, 13u, out,
					 sizeof(out)), 0u,
			"a label longer than an ODS-2 label builds nothing");
	ct_check_eq_u32(vms_rms_lockname(284, 1, 0, 0, "SYSDSK1", 7u, out,
					 VMS_RMS_LOCKNAME_LEN - 1u), 0u,
			"and so does a buffer too small for the name");

	printf("--- the identity the executive assigns to it ---\n");
	ct_check_eq_u32((uint32_t)VMS_RMS_LOCKNAME_GROUP, 0u,
			"UIC group 0 -- a system-wide name");
	ct_check_eq_u32((uint32_t)VMS_RMS_LOCKNAME_ACMODE, 1u,
			"access mode 1 (EXECUTIVE) -- which is why the "
			"EXECUTIVE takes this lock, not a user-mode caller");
}

int main(void)
{
	printf("=== test_rms_lockname (rd vms-326: the RMS file-access lock's "
	       "name, against real VAX frames) ===\n");
	matches_real_frames();
	two_volumes_two_resources();
	no_label_no_name();
	return ct_summary("test_rms_lockname");
}
