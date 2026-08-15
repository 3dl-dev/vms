/*
 * vms_rawpread.c - vms-d5d de-risk probe: can an ordinary NetBSD userspace
 * process open(2) + pread(2) a raw ODS-2 block off an MSCP disk, and if so,
 * off WHICH device node -- the block node ("/dev/ra1c" on vax) or the
 * raw-character node ("/dev/rra1c")?
 *
 * This directly answers the vms-5eb atomic-flip A1 assumption for the VAX
 * lane: does the shared ods2_bdev userspace reader (src/vmsfs/ods2/ods2_bdev.c)
 * work unmodified against a NetBSD/vax device node the same way it works
 * against Linux's /dev/vda, or does it need a raw-char node selection?
 *
 *   usage: vms_rawpread <device-path> <lbn> [block-count]
 *
 * Prints machine-parseable "KEY=value" lines (grepped by the driver) for:
 *   OPEN            ok | fail(errno=N desc)
 *   PREAD           ok(bytes=N) | fail(errno=N desc)
 * The probe recognizes TWO on-disk home-block formats at LBN 1, because this
 * repo's `tests/lab-vax` substrate currently masters volumes in EITHER shape
 * depending on the tool used:
 *
 *   - VMFS   : OVMX's own bespoke "simplified ODS-2-inspired" kernel format
 *              (src/kernel/vmsfs/vmsfs_ondisk.h) -- magic VMSFS_HOME_MAGIC
 *              ("VMFS", 0x564D4653) at byte 0, XOR checksum at byte 508.
 *              This is what tests/qemu/mkimage_vmsfs.c masters and what
 *              vmsfs.kmod's mount(2) validates today (run-vmsfs.sh's proof).
 *   - ODS-2  : the REAL Files-11 ODS-2 (Level 2) home block VSI OpenVMS
 *              documents -- format string "DECFILE11B  " at byte 496, two
 *              additive 16-bit checksums at bytes 58/510
 *              (src/vmsfs/include/vmsfs/ods2.h, `ods2_home_parse`). This is
 *              what the vms-5eb flip's userspace `ods2_bdev` reader actually
 *              consumes, and what `vms_initialize --ods2` masters, and what a
 *              real OpenVMS `INITIALIZE /STRUCTURE=2` produces
 *              (tests/ods2/real_vax_ods2.dsk).
 *
 * Both are OVMX's own already-public, already-documented layouts (the VMFS
 * one explicitly invented and labelled "not byte-compatible with real
 * ODS-2"; the ODS-2 one clean-room-derived from the public VSI manual) --
 * this file duplicates only their checksum/magic CONSTANTS, not any parsing
 * code, so the probe has zero build-time dependency on either's header tree
 * and can run standalone in the guest.
 *
 * Prints, after a successful PREAD of exactly one 512-byte block:
 *   HOME_MATCH      vmfs | ods2 | none    (which format's magic+checksum(s)
 *                                          validated against the bytes read)
 *   HOME_VOLNAME    "<12 bytes>"          (the matched format's volume-name
 *                                          field, for a human sanity check)
 *   -- plus the raw per-format checksum lines for both formats, always, so a
 *      "none" result is still fully diagnosable from the log.
 *
 * Exit codes: 0 = open+pread succeeded AND one of the two formats validated;
 *             1 = open(2) failed; 2 = pread(2) failed;
 *             3 = pread succeeded but NEITHER format validated (wrong LBN,
 *                 wrong device, or genuinely not a home block -- distinguishes
 *                 "raw I/O works but this isn't the home block" from "raw I/O
 *                 is blocked").
 *
 * Statically linked (matches tests/netbsd/guest/vmsfs_mount.c's build
 * pattern in tools/cross-vax/build-vmsfs-mount-vax.sh); no shared-lib
 * plumbing needed in the guest.
 *
 * Clean-room (CLAUDE.md Rule 8): built only from the public NetBSD open(2)/
 * pread(2) interfaces and OVMX's own already-documented on-disk layouts. No
 * NetBSD or VSI/HPE source is copied.
 */

#include <sys/types.h>

#include <err.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define OVMX_BLOCK_SIZE   512

/* ---- real ODS-2 (DECFILE11B) home block, src/vmsfs/include/vmsfs/ods2.h - */
#define ODS2_FORMAT_OFF   496
#define ODS2_FORMAT_LEN   12
#define ODS2_VOLNAME_OFF  472
#define ODS2_VOLNAME_LEN  12
#define ODS2_CKSUM1_OFF   58
#define ODS2_CKSUM2_OFF   510

static const char ODS2_FORMAT_STRING[ODS2_FORMAT_LEN] = "DECFILE11B  ";

/* ---- OVMX's own VMFS kernel format, src/kernel/vmsfs/vmsfs_ondisk.h ------ */
#define VMFS_HOME_MAGIC     0x564D4653u   /* "VMFS" */
#define VMFS_MAGIC_OFF      0
#define VMFS_VOLNAME_OFF    64
#define VMFS_VOLNAME_LEN    12
#define VMFS_CKSUM_OFF      508

static uint16_t
le16(const uint8_t *p)
{
	return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t
le32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* Additive 16-bit checksum over `count' little-endian words, matching the
 * ODS-2 home-block convention (checksum1: first 29 words; checksum2: all
 * 255 words up to but excluding the checksum2 field itself). */
static uint16_t
ods2_checksum(const uint8_t *b, unsigned count)
{
	unsigned i, sum = 0;
	for (i = 0; i < count; i++)
		sum += le16(b + (size_t)i * 2);
	return (uint16_t)sum;
}

/* XOR checksum over the first (size/4 - 1) little-endian 32-bit words,
 * matching vmsfs_checksum() in src/kernel/vmsfs/vmsfs_ondisk.h exactly
 * (excludes the final word, which holds the checksum itself). */
static uint32_t
vmfs_checksum(const uint8_t *b, unsigned size)
{
	unsigned words = (size - 4) / 4;
	uint32_t sum = 0;
	unsigned i;
	for (i = 0; i < words; i++)
		sum ^= le32(b + (size_t)i * 4);
	return sum;
}

int
main(int argc, char *argv[])
{
	const char *dev;
	long lbn;
	long nblocks = 1;
	int fd;
	off_t off;
	size_t want;
	uint8_t *buf;
	ssize_t got;
	int rc = 0;

	if (argc < 3 || argc > 4) {
		fprintf(stderr, "usage: %s <device> <lbn> [block-count]\n", argv[0]);
		return 2;
	}
	dev = argv[1];
	lbn = strtol(argv[2], NULL, 10);
	if (argc == 4)
		nblocks = strtol(argv[3], NULL, 10);
	if (lbn < 0 || nblocks < 1) {
		fprintf(stderr, "usage: %s <device> <lbn> [block-count]\n", argv[0]);
		return 2;
	}

	want = (size_t)nblocks * OVMX_BLOCK_SIZE;
	buf = malloc(want);
	if (buf == NULL)
		err(2, "malloc(%zu)", want);
	memset(buf, 0, want);

	printf("DEVICE=%s LBN=%ld BLOCKS=%ld\n", dev, lbn, nblocks);

	fd = open(dev, O_RDONLY);
	if (fd < 0) {
		printf("OPEN=fail errno=%d (%s)\n", errno, strerror(errno));
		return 1;
	}
	printf("OPEN=ok fd=%d\n", fd);

	off = (off_t)lbn * OVMX_BLOCK_SIZE;
	got = pread(fd, buf, want, off);
	if (got < 0) {
		printf("PREAD=fail errno=%d (%s)\n", errno, strerror(errno));
		close(fd);
		return 2;
	}
	if ((size_t)got != want) {
		printf("PREAD=short got=%zd want=%zu\n", got, want);
		close(fd);
		return 2;
	}
	printf("PREAD=ok bytes=%zd\n", got);
	close(fd);

	/* Home-block validation only makes sense for a single-block read (the
	 * caller is expected to pass the home LBN, conventionally 1). */
	if (nblocks != 1) {
		free(buf);
		return 0;
	}

	{
		/* ODS-2 (DECFILE11B) check */
		uint16_t o_stored1 = le16(buf + ODS2_CKSUM1_OFF);
		uint16_t o_calc1   = ods2_checksum(buf, 29);
		uint16_t o_stored2 = le16(buf + ODS2_CKSUM2_OFF);
		uint16_t o_calc2   = ods2_checksum(buf, 255);
		int fmt_ok = (memcmp(buf + ODS2_FORMAT_OFF, ODS2_FORMAT_STRING,
		                      ODS2_FORMAT_LEN) == 0);
		int ods2_ok = fmt_ok && (o_stored1 == o_calc1) && (o_stored2 == o_calc2);

		/* VMFS (OVMX kernel format) check */
		uint32_t v_magic  = le32(buf + VMFS_MAGIC_OFF);
		uint32_t v_stored = le32(buf + VMFS_CKSUM_OFF);
		uint32_t v_calc   = vmfs_checksum(buf, OVMX_BLOCK_SIZE);
		int vmfs_ok = (v_magic == VMFS_HOME_MAGIC) && (v_stored == v_calc);

		printf("ODS2_FORMAT=%s (stored=%.12s)\n",
		    fmt_ok ? "match" : "mismatch", buf + ODS2_FORMAT_OFF);
		printf("ODS2_CHECKSUM1=%s (stored=0x%04x calc=0x%04x)\n",
		    (o_stored1 == o_calc1) ? "match" : "mismatch", o_stored1, o_calc1);
		printf("ODS2_CHECKSUM2=%s (stored=0x%04x calc=0x%04x)\n",
		    (o_stored2 == o_calc2) ? "match" : "mismatch", o_stored2, o_calc2);
		printf("VMFS_MAGIC=%s (stored=0x%08x want=0x%08x)\n",
		    (v_magic == VMFS_HOME_MAGIC) ? "match" : "mismatch", v_magic, VMFS_HOME_MAGIC);
		printf("VMFS_CHECKSUM=%s (stored=0x%08x calc=0x%08x)\n",
		    (v_stored == v_calc) ? "match" : "mismatch", v_stored, v_calc);

		if (ods2_ok) {
			char volname[ODS2_VOLNAME_LEN + 1];
			memcpy(volname, buf + ODS2_VOLNAME_OFF, ODS2_VOLNAME_LEN);
			volname[ODS2_VOLNAME_LEN] = '\0';
			printf("HOME_MATCH=ods2\n");
			printf("HOME_VOLNAME=\"%s\"\n", volname);
		} else if (vmfs_ok) {
			char volname[VMFS_VOLNAME_LEN + 1];
			memcpy(volname, buf + VMFS_VOLNAME_OFF, VMFS_VOLNAME_LEN);
			volname[VMFS_VOLNAME_LEN] = '\0';
			printf("HOME_MATCH=vmfs\n");
			printf("HOME_VOLNAME=\"%s\"\n", volname);
		} else {
			printf("HOME_MATCH=none\n");
			printf("HOME_VOLNAME=\"\"\n");
			rc = 3;
		}
	}

	free(buf);
	return rc;
}
