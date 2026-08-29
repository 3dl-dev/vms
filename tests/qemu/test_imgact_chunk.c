/*
 * test_imgact_chunk.c -- imgact_acp_pread() chunks a >1 MiB read into
 * <= 1 MiB IO$_READVBLK QIOs (vms-1162).
 *
 * THE BUG. The executive caps a single IO$_READVBLK at ACP_RW_MAX_XFER = 1 MiB
 * and rejects a longer length with SS$_BADPARAM
 * (src/kernel-core/vmsfs_acp.c:1878 and its check at ~:1972). imgact_acp_pread()
 * used to issue ONE IO$_READVBLK for the whole requested length, so activating
 * any image/producer with a single PT_LOAD larger than 1 MiB (DECC$SHR is
 * already ~1 MiB and grows with the CRTL) drew SS$_BADPARAM -> the read
 * returned -1 -> load_ovmx_producer()/load_object() reported failure ->
 * IMGACT open died with %IMGACT-F-IMGNOTFND. The existing ACP suite
 * (test_syssvc_imgact_acp.c) never exercised a >1 MiB file (its fixture is
 * 1424 bytes), so the cliff was invisible.
 *
 * WHAT THIS PROVES. This is a USERSPACE unit test (test_imgact_*, not
 * test_syssvc_* -- it needs no /dev/vms and passes in EVERY environment; see
 * tests/qemu/CMakeLists.txt's note on that naming). It links the REAL
 * src/imgact/imgact_acp.c -- the exact code IMGACT.EXE runs -- and backs its
 * three host primitives (imgact_acp_dev_open/close/ioctl) with an IN-MEMORY
 * ACP that reproduces the executive's per-QIO contract FAITHFULLY:
 *
 *   - A READVBLK with length > ACP_RW_MAX_XFER (1 MiB) is REJECTED with
 *     SS$_BADPARAM and transfers nothing -- exactly vmsfs_acp.c:1972. (The
 *     regression guard: the mock proves the old single-QIO whole-segment read
 *     WOULD have failed at the cliff.)
 *   - A READVBLK within the cap copies bytes from a deterministic in-memory
 *     file at {vbn, offset} and reports SS$_NORMAL / SS$_ENDOFFILE.
 *
 * Against that mock, imgact_acp_pread() must read a >1 MiB file BYTE-EXACT
 * across the 1-MiB boundary, issuing only <= 1 MiB QIOs. The mock records the
 * QIO count and the largest length it ever saw, so we assert both the exact
 * chunk count for each size AND that no QIO ever exceeded the executive bound.
 *
 * ARCH-INDEPENDENT. Pure C over an in-memory buffer -- no /dev/vms, no ELF, no
 * qemu -- so it runs identically on x86_64, Alpha (LP64) and VAX (ILP32). The
 * boot-level 3-arch proof that a real >1 MiB producer activates over a real
 * /dev/vms belongs in tests/qemu/test_syssvc_imgact_acp.c once its ODS-2
 * fixture builder can emit a >2048-block PT_LOAD (it caps at 1424 bytes today);
 * that wiring is noted in the PR as a follow-up, not authored here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "vms_ioctl.h"      /* VMS_IOCTL_ACP_READVBLK, struct vms_acp_rw_args */
#include "imgact_acp.h"

/* The executive's per-QIO bound (src/kernel-core/vmsfs_acp.c:1878). Kept in
 * sync deliberately; imgact_acp.c mirrors it as IMGACT_ACP_RW_MAX_XFER. */
#define EXEC_ACP_RW_MAX_XFER  (1u << 20)   /* 1 MiB */
#define ACP_BLK               512u

/* --------------------------------------------------------------------------
 * The in-memory faithful ACP. One deterministic file; READVBLK honours the
 * 1-MiB per-QIO cap the same way the executive does.
 * -------------------------------------------------------------------------- */
static uint8_t  *g_file;          /* backing bytes */
static uint32_t  g_file_len;      /* valid bytes  */
static uint32_t  g_qio_count;     /* READVBLKs issued since reset */
static uint32_t  g_qio_max_len;   /* largest length any READVBLK carried */

static uint8_t pat(uint32_t i)
{
	/* Multiplicative hash low byte: adjacent bytes differ, so any
	 * mis-ordered or dropped chunk shows up as a mismatch. */
	uint32_t h = i * 2654435761u;
	return (uint8_t)(h >> 24);
}

static void acp_reset(uint32_t len)
{
	uint32_t i;
	free(g_file);
	g_file = (uint8_t *)malloc(len ? len : 1);
	g_file_len = len;
	for (i = 0; i < len; i++)
		g_file[i] = pat(i);
	g_qio_count = 0;
	g_qio_max_len = 0;
}

/* --- the three host primitives imgact_acp.c calls (the freestanding seam) --- */
int  imgact_acp_dev_open(void)  { return 3; }   /* any non-negative fd */
void imgact_acp_dev_close(int fd) { (void)fd; }

long imgact_acp_dev_ioctl(int fd, unsigned long req, void *arg)
{
	(void)fd;
	if (req == VMS_IOCTL_ACP_READVBLK) {
		struct vms_acp_rw_args *r = (struct vms_acp_rw_args *)arg;
		uint64_t pos;
		uint32_t avail;

		g_qio_count++;
		if (r->length > g_qio_max_len)
			g_qio_max_len = r->length;

		/* Faithful to vmsfs_acp.c:1972 -- reject an over-cap QIO. */
		if (r->vbn == 0 || r->offset >= ACP_BLK ||
		    r->length > EXEC_ACP_RW_MAX_XFER) {
			r->status = SS$_BADPARAM;
			r->xferred = 0;
			return 0;
		}

		pos = (uint64_t)(r->vbn - 1u) * ACP_BLK + r->offset;
		if (pos >= g_file_len) {
			r->status = SS$_ENDOFFILE;
			r->xferred = 0;
			return 0;
		}
		avail = g_file_len - (uint32_t)pos;
		if (r->length > avail)
			r->length = avail;   /* short read at EOF */
		memcpy((void *)(uintptr_t)r->buffer, g_file + pos, r->length);
		r->xferred = r->length;
		r->status = SS$_NORMAL;
		return 0;
	}
	return -1;
}

/* -------------------------------------------------------------------------- */

static int pass = 0, fail = 0;
static void check(int cond, const char *name)
{
	if (cond) { printf("  PASS: %s\n", name); pass++; }
	else      { printf("  FAIL: %s\n", name); fail++; }
}

/*
 * One case. Read `n` bytes at `off` via the REAL imgact_acp_pread and return 1
 * iff it returned exactly `expect_bytes` (== n, except when the request runs
 * past EOF and clamps), those bytes are byte-exact against the file pattern,
 * the transfer used exactly `expect_qios` QIOs, and no single QIO exceeded the
 * executive's 1-MiB cap. Prints a per-case diagnostic line either way -- so a
 * bare-ctest failure still says WHICH case and HOW -- but does NOT itself
 * check(): the suite gates ONE aggregate property (see main), which keeps the
 * facility_defects mutation control's reddened-assertion set exact.
 */
static int one_case(struct imgact_acp_file *f, uint32_t off, uint32_t n,
		    uint32_t expect_bytes, uint32_t expect_qios,
		    const char *label)
{
	uint8_t *buf = (uint8_t *)malloc(n ? n : 1);
	long got;
	int exact = 1, ok;
	uint32_t i;

	g_qio_count = 0;
	g_qio_max_len = 0;
	got = imgact_acp_pread(f, buf, n, (long)off);

	for (i = 0; i < expect_bytes; i++) {
		if (buf[i] != pat(off + i)) { exact = 0; break; }
	}
	ok = (got == (long)expect_bytes) && exact &&
	     (g_qio_count == expect_qios) &&
	     (g_qio_max_len <= EXEC_ACP_RW_MAX_XFER);
	printf("    [%s] %s: got=%ld/%u exact=%d qios=%u(exp %u) maxlen=%u\n",
	       ok ? "ok" : "BAD", label, got, expect_bytes, exact,
	       g_qio_count, expect_qios, g_qio_max_len);
	free(buf);
	return ok;
}

int main(void)
{
	struct imgact_acp_file f;
	const uint32_t MiB = 1u << 20;
	int all_ok = 1;

	setvbuf(stdout, NULL, _IOLBF, 0);
	printf("=== test_imgact_chunk: imgact_acp_pread chunks a >1 MiB read into "
	       "<=1 MiB IO$_READVBLK QIOs (vms-1162) ===\n");

	/* --- the cliff exists: a single >1 MiB IO$_READVBLK is rejected. This is
	 * exactly what the pre-fix imgact_acp_pread put in r.length for a whole
	 * >1 MiB segment; the faithful in-memory ACP rejects it SS$_BADPARAM the
	 * way the executive does (vmsfs_acp.c:1972). Independent of the chunking
	 * under test, so it stays green under the mutation control -- it proves the
	 * bound the chunk loop exists to respect is real. */
	{
		struct vms_acp_rw_args r;
		acp_reset(2u * MiB);
		memset(&r, 0, sizeof(r));
		r.vbn = 1; r.offset = 0; r.length = 2u * MiB;
		r.buffer = (uint64_t)(uintptr_t)g_file;   /* dummy dst */
		(void)imgact_acp_dev_ioctl(3, VMS_IOCTL_ACP_READVBLK, &r);
		check(r.status == SS$_BADPARAM && r.xferred == 0,
		      "a single >1 MiB IO$_READVBLK is rejected SS$_BADPARAM (the cliff)");
	}

	/* Now drive the REAL imgact_acp_pread across the boundary, every case. */
	memset(&f, 0, sizeof(f));
	f.dev_fd = imgact_acp_dev_open();
	f.chan = 1;
	f.accessed = 1;

	/* A: exactly 1 MiB -> 1 QIO (the boundary itself, no crossing). */
	acp_reset(1u * MiB);
	f.valid = 1u * MiB;
	all_ok &= one_case(&f, 0, 1u * MiB, 1u * MiB, 1, "exactly 1 MiB -> 1 QIO");

	/* B: 1 MiB + 1 -> 2 QIOs (the smallest cliff crossing). */
	acp_reset(1u * MiB + 1u);
	f.valid = 1u * MiB + 1u;
	all_ok &= one_case(&f, 0, 1u * MiB + 1u, 1u * MiB + 1u, 2, "1 MiB + 1 -> 2 QIOs");

	/* C: DECC$SHR-shaped -- 2119 blocks = 1,084,928 B = 1 MiB + 36,352 ->
	 * 2 QIOs. The real producer segment size that first tripped the cliff. */
	acp_reset(2119u * ACP_BLK);
	f.valid = 2119u * ACP_BLK;
	all_ok &= one_case(&f, 0, 2119u * ACP_BLK, 2119u * ACP_BLK, 2,
			   "DECC$SHR-shaped 2119-block (1,084,928 B) -> 2 QIOs");

	/* D: > 2 MiB -> 3 QIOs (general multi-crossing). */
	acp_reset(2u * MiB + 123u);
	f.valid = 2u * MiB + 123u;
	all_ok &= one_case(&f, 0, 2u * MiB + 123u, 2u * MiB + 123u, 3,
			   "2 MiB + 123 B -> 3 QIOs");

	/* E: non-block-aligned start offset crossing the boundary. off=100 is
	 * mid-first-block; vbn/offset must be re-derived each chunk. */
	acp_reset(3u * MiB);
	f.valid = 3u * MiB;
	all_ok &= one_case(&f, 100u, 1u * MiB + 500u, 1u * MiB + 500u, 2,
			   "non-block-aligned offset crossing 1 MiB");

	/* F: EOF short read -- request more than valid; get exactly valid, chunked. */
	acp_reset(1u * MiB + 777u);
	f.valid = 1u * MiB + 777u;
	all_ok &= one_case(&f, 0, 4u * MiB, 1u * MiB + 777u, 2,
			   "read past EOF clamps to valid, still chunked");

	/* THE gated property: every >1 MiB read is byte-exact across the 1-MiB
	 * boundary and issued only <=1 MiB QIOs. This is the one assertion the
	 * facility_defects mutation control (imgact-acp-read-unchunked) reddens. */
	/* negctl: imgact-acp-read-unchunked */
	check(all_ok,
	      "imgact_acp_pread chunks every >1 MiB read into <=1 MiB QIOs, byte-exact "
	      "across the boundary");

	printf("=== test_imgact_chunk: %d passed, %d failed ===\n", pass, fail);
	free(g_file);
	return fail == 0 ? 0 : 1;
}
