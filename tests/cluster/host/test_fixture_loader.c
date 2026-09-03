// SPDX-License-Identifier: GPL-2.0
/*
 * test_fixture_loader.c - the clean-room specimen loader and its refusals
 * (FC-P0.6, rung R1).
 *
 * The loader's value is entirely in what it REFUSES. tests/vmsscs/
 * test_capture_manifest.py records why: six lab-2 captures were dropped into
 * the directory every lab-1 measurement tool globbed, and a filename blocklist
 * could not catch it -- only a manifest that refuses an UNKNOWN capture could.
 * The same reasoning applies to fixtures, so these tests drive the refusals as
 * hard as the happy path.
 */

#include "cluster_fixture.h"
#include "cluster_sha256.h"
#include "cluster_test.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char g_tmpdir[] = "/tmp/ovmx-fixture-XXXXXX";

static const char *write_tmp(const char *basename, const char *body)
{
	static char path[512];
	FILE *f;

	snprintf(path, sizeof(path), "%s/%s", g_tmpdir, basename);
	f = fopen(path, "w");
	if (!f) {
		printf("  FAIL cannot write %s\n", path);
		exit(1);
	}
	fputs(body, f);
	fclose(f);
	return path;
}

/* Load a temp specimen and assert it is REFUSED with a reason. */
static void expect_reject(const char *basename, const char *body,
			  const char *what)
{
	struct vms_fixture fx;
	char err[VMS_FIXTURE_ERRLEN] = "";
	const char *path = write_tmp(basename, body);
	int rc = vms_fixture_load(path, OVMX_CLEANROOM_MANIFEST, &fx,
				  err, sizeof(err));

	ct_check(rc != 0, what);
	if (rc != 0)
		printf("       reason: %s\n", err);
	unlink(path);
}

static void test_sha256_vectors(void)
{
	char hex[65];

	printf("-- SHA-256 against the FIPS 180-4 published vectors\n");
	cluster_sha256_hex((const uint8_t *)"", 0, hex);
	ct_check(!strcmp(hex, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934c"
			      "a495991b7852b855"), "empty string");
	cluster_sha256_hex((const uint8_t *)"abc", 3, hex);
	ct_check(!strcmp(hex, "ba7816bf8f01cfea414140de5dae2223b00361a396177a9c"
			      "b410ff61f20015ad"), "\"abc\"");
	cluster_sha256_hex((const uint8_t *)
		"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56,
		hex);
	ct_check(!strcmp(hex, "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167"
			      "f6ecedd419db06c1"),
		 "the 56-byte vector (two-block padding path)");
}

static void test_corpus_loads(void)
{
	static struct vms_fixture fx[VMS_FIXTURE_MAX_FILES];
	char err[VMS_FIXTURE_ERRLEN];
	int n, i;

	printf("-- the checked-in corpus loads, digests and all\n");
	n = vms_fixture_load_all(OVMX_FIXTURE_DIR, OVMX_CLEANROOM_MANIFEST,
				 fx, VMS_FIXTURE_MAX_FILES, err, sizeof(err));
	if (n < 0)
		printf("       reason: %s\n", err);
	ct_check(n > 0, "corpus loads");
	for (i = 0; i < n; i++) {
		char what[256];

		snprintf(what, sizeof(what),
			 "%s: origin %s, %u cited bytes of %u, %u spans",
			 fx[i].name, vms_fixture_origin_name(fx[i].origin),
			 vms_fixture_cited_bytes(&fx[i]), fx[i].wire_len,
			 fx[i].n_cited);
		ct_check(fx[i].n_cited > 0, what);
	}
}

static void test_cited_spans(void)
{
	struct vms_fixture fx;
	char err[VMS_FIXTURE_ERRLEN];
	char path[512];

	printf("-- cited vs uncited spans\n");
	snprintf(path, sizeof(path), "%s/hello-multicast.spec",
		 OVMX_FIXTURE_DIR);
	ct_check(vms_fixture_load(path, OVMX_CLEANROOM_MANIFEST, &fx,
				  err, sizeof(err)) == 0,
		 "hello-multicast.spec loads");
	ct_check(vms_fixture_is_cited(&fx, 0, 32),
		 "abs 0..31 (the harvest span) is cited");
	/* E56: abs 47..67 WAS the "unpublished capability span" and was
	 * deliberately left uncited. Spec SS4(a).2 now grounds it (census over
	 * 11575 HELLOs, five senders, 0 residuals among real nodes) and the
	 * specimen cites it, because a zero there is not omission -- it is a
	 * different value than every real node sends, and it stalled the join. */
	ct_check(vms_fixture_is_cited(&fx, 47, 21),
		 "abs 47..67 (the SS4(a).2 discovery-format span) IS cited");
	ct_check(!vms_fixture_is_cited(&fx, 96, 6),
		 "abs 96..101 (the live timer) is NOT cited");
	ct_check(!vms_fixture_is_cited(&fx, 92, 10),
		 "a span straddling cited and uncited bytes is NOT cited");
	ct_check(fx.bytes[96] == 0, "uncited bytes are zero-filled");
	ct_check(fx.origin == VMS_FIXTURE_ORIGIN_SPEC,
		 "origin is spec-composed, not a claim of capture extraction");
}

static void test_manifest_gate(void)
{
	char err[VMS_FIXTURE_ERRLEN];

	printf("-- the clean-room manifest gate\n");
	ct_check(vms_fixture_capture_in_manifest(OVMX_CLEANROOM_MANIFEST,
						 "scs-idle-baseline.pcap",
						 err, sizeof(err)) == 0,
		 "a manifest-hashed capture is accepted");
	ct_check(vms_fixture_capture_in_manifest(OVMX_CLEANROOM_MANIFEST,
						 "totally-made-up.pcap",
						 err, sizeof(err)) != 0,
		 "an unknown capture is REFUSED by name");
	ct_check(vms_fixture_capture_in_manifest("/no/such/manifest",
						 "scs-idle-baseline.pcap",
						 err, sizeof(err)) != 0,
		 "a missing manifest is a failure, never a pass-through");
}

/* ---- refusals ---------------------------------------------------- */

#define HDR(origin, capture, extra)                                          \
	"%OVMX-CLUSTER-SPECIMEN-1\n"                                         \
	"name: t\nclass: hello\norigin: " origin "\n"                        \
	"spec: s\n" capture extra "wire-len: 20\n"

static void test_refusals(void)
{
	printf("-- refusals\n");

	expect_reject("no-magic.spec",
		      "name: t\nclass: hello\norigin: synthetic\n",
		      "a file without the magic line is refused");

	expect_reject("bad-capture.spec",
		      HDR("capture", "capture: not-in-the-manifest.pcap\n", "")
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 60 07\n",
		      "origin=capture naming an UNLISTED capture is refused");

	expect_reject("capture-nocapture.spec",
		      HDR("capture", "", "")
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 60 07\n",
		      "origin=capture with no capture: is refused");

	expect_reject("spec-nocapture.spec",
		      HDR("spec-composed", "", "")
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 60 07\n",
		      "origin=spec-composed with no capture cite is refused");

	expect_reject("synthetic-real-class.spec",
		      HDR("synthetic", "", "")
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 60 07\n",
		      "a synthetic specimen claiming a real frame class is "
		      "refused (class 'hello', origin synthetic)");

	expect_reject("bad-digest.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nspec: s\nwire-len: 20\n"
		      "sha256: 1111111111111111111111111111111111111111111111111111111111111111\n"
		      "%bytes\n@0 60 07\n",
		      "a wrong sha256 is refused (tamper-evident)");

	expect_reject("oob-byte.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nspec: s\nwire-len: 4\n"
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@2 60 07 aa\n",
		      "a byte past wire-len is refused");

	expect_reject("overlap.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nspec: s\nwire-len: 20\n"
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 60 07 aa bb\n@2 cc dd\n",
		      "overlapping cited spans are refused");

	expect_reject("no-offset.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nspec: s\nwire-len: 20\n"
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n60 07\n",
		      "a byte line before any @offset is refused");

	expect_reject("bad-hex.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nspec: s\nwire-len: 20\n"
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 6007\n",
		      "a malformed hex token is refused");

	expect_reject("no-bytes.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nspec: s\nwire-len: 20\n"
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n",
		      "a specimen with no %bytes section is refused");

	expect_reject("unknown-key.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nnonsense: 1\nwire-len: 20\n"
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 60 07\n",
		      "an unknown header key is refused (typos do not pass)");

	expect_reject("huge.spec",
		      "%OVMX-CLUSTER-SPECIMEN-1\nname: t\nclass: unknown\n"
		      "origin: synthetic\nspec: s\nwire-len: 999999\n"
		      "sha256: 0000000000000000000000000000000000000000000000000000000000000000\n"
		      "%bytes\n@0 60 07\n",
		      "an absurd wire-len is refused");
}

int main(void)
{
	if (!mkdtemp(g_tmpdir)) {
		printf("  FAIL cannot create a temp directory\n");
		return 1;
	}
	printf("test_fixture_loader: clean-room specimen loader (FC-P0.6)\n");
	test_sha256_vectors();
	test_corpus_loads();
	test_cited_spans();
	test_manifest_gate();
	test_refusals();
	rmdir(g_tmpdir);
	return ct_summary("test_fixture_loader");
}
