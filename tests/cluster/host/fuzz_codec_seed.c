// SPDX-License-Identifier: GPL-2.0
/*
 * fuzz_codec_seed.c - parser fuzz seed harness (FC-P0.6 done-condition:
 * "parser fuzz seed harness stub").
 *
 * TWO BUILDS FROM ONE FILE.
 *
 *   1. Host ctest (this TU's normal build, no special flags): a plain main()
 *      that drives vms_cluster_codec_fuzz_one() -- the single entry point
 *      declared in vms_cluster_codec.h §7 that reaches every parse path in
 *      the codec -- over the checked-in fixture corpus (as a REGRESSION
 *      corpus, not to prove anything the round-trip test doesn't already
 *      cover) plus a small hand-written set of malformed/truncated/boundary
 *      inputs. It must exit 0: the codec's whole contract is that it never
 *      crashes and never reads outside the buffer it was given, regardless
 *      of content, and this is the cheapest test of that contract.
 *
 *   2. A libFuzzer target: build this TU with
 *      `-DOVMX_CLUSTER_CODEC_LIBFUZZER -fsanitize=fuzzer,address` and link,
 *      no `main()` of ours in the link (libFuzzer supplies one). Then
 *
 *          ./fuzz_codec_seed tests/cluster/host/fixtures/
 *
 *      runs libFuzzer with the same fixture directory as its seed corpus.
 *      Coverage-guided mutation is NOT part of the R1 done-condition (it
 *      needs a sanitizer toolchain most host builds don't carry); this is
 *      the STUB the done-condition asks for -- the entry point, the seed
 *      corpus, and a harness that already knows how to drive both.
 *
 * A minimiser measuring how much of the frame-class registry a run reached
 * can read the return of vms_cluster_codec_fuzz_one() (the settled class) --
 * this harness does not attempt that itself.
 */

#include "cluster_fixture.h"
#include "vms_cluster_codec.h"

#include <stdint.h>
#include <string.h>

#ifndef OVMX_CLUSTER_CODEC_LIBFUZZER

#include "cluster_test.h"

#include <stdio.h>

/*
 * Hand-written boundary/malformed inputs: the cases a random mutator finds
 * eventually but a seed harness should hit on every run. Every one of these
 * is a documented historical or structural edge, not an arbitrary byte
 * string:
 *   - empty and 1-byte: shorter than any header read.
 *   - a truncated SCA header: ethertype present, everything past it absent.
 *   - a non-SCA ethertype: the VMS_CODEC_E_NOTSCA path.
 *   - a claimed SCA length field that overruns the actual buffer (spec sec
 *     2's length identity, deliberately violated).
 *   - all-0xff and all-0x00 frames at a plausible SCS length: neither is a
 *     valid frame, but both must classify (or fail to) without reading past
 *     their buffer.
 */
struct seed_case {
	const char *what;
	uint8_t     bytes[256];
	uint32_t    len;
};

static const struct seed_case g_seeds[] = {
	{ "empty input", { 0 }, 0 },
	{ "single byte", { 0x60 }, 1 },
	{ "ethertype only, header truncated",
	  { 0,0,0,0,0,0, 0,0,0,0,0,0, 0x60,0x07 }, 14 },
	{ "non-SCA ethertype",
	  { 0,0,0,0,0,0, 0,0,0,0,0,0, 0x08,0x00, 0,0 }, 16 },
	{ "SCA length field claims far past the real buffer",
	  { 0,0,0,0,0,0, 0,0,0,0,0,0, 0x60,0x07, 0xff,0xff }, 16 },
};

/* All-0xNN frames at a handful of lengths that hit different length classes
 * (see vms_frame_class_info.min_len in vms_cluster_codec.c): the classifier
 * must settle on SOME class (possibly UNKNOWN) without an out-of-range read,
 * which vms_cluster_codec_fuzz_one()'s own internal accessor calls enforce.
 */
static const uint32_t g_fill_lens[] = { 0, 1, 14, 16, 32, 41, 60, 78, 120,
					 134, 190, 204, 512, 1500 };

static void run_fill_pattern(uint8_t fill)
{
	static uint8_t buf[1500];
	unsigned i;

	memset(buf, fill, sizeof(buf));
	for (i = 0; i < sizeof(g_fill_lens) / sizeof(g_fill_lens[0]); i++) {
		uint32_t n = g_fill_lens[i];
		uint8_t cls;
		char what[64];

		if (n > sizeof(buf))
			continue;
		cls = vms_cluster_codec_fuzz_one(buf, n);
		snprintf(what, sizeof(what),
			 "fill 0x%02x len %u: no crash (settled class %u)",
			 fill, n, cls);
		ct_check(1, what); /* reaching this line without a crash IS the check */
	}
}

static void run_hand_seeds(void)
{
	unsigned i;

	printf("-- hand-written boundary/malformed seeds\n");
	for (i = 0; i < sizeof(g_seeds) / sizeof(g_seeds[0]); i++) {
		const struct seed_case *s = &g_seeds[i];
		uint8_t cls = vms_cluster_codec_fuzz_one(s->bytes, s->len);
		char what[128];

		snprintf(what, sizeof(what),
			 "%s: no crash (settled class %u)", s->what, cls);
		ct_check(1, what);
	}
}

static void run_fixture_corpus(void)
{
	static struct vms_fixture fx[VMS_FIXTURE_MAX_FILES];
	char err[VMS_FIXTURE_ERRLEN];
	int n, i;

	printf("-- checked-in fixture corpus as a regression seed set\n");
	n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				 fx, VMS_FIXTURE_MAX_FILES, err, sizeof(err));
	if (n <= 0) {
		printf("  FAIL fixture corpus: %s\n", err);
		ct_check(0, "fixture corpus loads for the fuzz seed harness");
		return;
	}
	for (i = 0; i < n; i++) {
		uint8_t cls = vms_cluster_codec_fuzz_one(fx[i].bytes,
							 fx[i].wire_len);
		char what[192];

		snprintf(what, sizeof(what),
			 "%s: no crash (settled class %u)", fx[i].name, cls);
		ct_check(1, what);
	}
	/* Also every prefix-truncation of the first specimen: a parser bug at
	 * a fixed offset shows up as SOME truncation length crashing even
	 * when the full frame does not. */
	if (n > 0) {
		uint32_t t;

		printf("-- every prefix truncation of one specimen\n");
		for (t = 0; t <= fx[0].wire_len; t++) {
			uint8_t cls = vms_cluster_codec_fuzz_one(fx[0].bytes, t);
			(void)cls;
		}
		ct_check(1, "all prefix truncations of the first specimen: no crash");
	}
}

int main(void)
{
	printf("fuzz_codec_seed: parser fuzz seed harness (FC-P0.6)\n");
	run_hand_seeds();
	run_fill_pattern(0x00);
	run_fill_pattern(0xff);
	run_fixture_corpus();
	return ct_summary("fuzz_codec_seed");
}

#else /* OVMX_CLUSTER_CODEC_LIBFUZZER */

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	(void)vms_cluster_codec_fuzz_one(data, (uint32_t)size);
	return 0;
}

#endif /* OVMX_CLUSTER_CODEC_LIBFUZZER */
