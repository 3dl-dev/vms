/* SPDX-License-Identifier: GPL-2.0 */
/*
 * cluster_fixture.h - the clean-room SPECIMEN loader for the host cluster
 * codec tests (plan FC-P0.6; design §3.9 rule 4 "every grounded frame class
 * has a fixture from the clean-room manifest").
 *
 * WHY A LOADER AND NOT A byte[] IN A .c FILE. A specimen has to carry its
 * PROVENANCE, or it is just a number someone typed. docs/clean-room/
 * PROVENANCE.md §3 defines the derivation chain this project is auditable
 * under:
 *
 *      packet capture (source) -> decoder -> documented fact -> code
 *
 * A fixture is the third arrow's fixed point: the file records which capture
 * (by the exact name hashed in docs/clean-room/reference-captures.sha256) and
 * which section of docs/cluster-protocol-spec.md every cited byte comes from,
 * and the loader REFUSES a specimen whose capture is not in that manifest.
 * That refusal is the same mechanism tools/cluster/capture_manifest.py gives
 * the measurement tools, applied to fixtures.
 *
 * ------------------------------------------------------------------------
 * THE SPECIMEN FILE FORMAT (text; git-diffable; provenance inline)
 * ------------------------------------------------------------------------
 *
 *   %OVMX-CLUSTER-SPECIMEN-1
 *   name:      hello-multicast-vax1
 *   class:     hello                 # a vms_frame_class_info .name
 *   origin:    spec-composed         # spec-composed | capture | synthetic
 *   spec:      §2, §4(a), §4(b)      # required for spec-composed
 *   capture:   scs-idle-baseline.pcap  # must be in reference-captures.sha256
 *   frame:     1                     # SCA frame index in that capture
 *   wire-len:  134                   # TRUE on-wire frame length
 *   sha256:    <64 hex over the assembled wire-len bytes>
 *   %bytes
 *   @0    ab 00 04 01 01 01    # abs 0..5   eth dst: multicast, spec §3
 *   @14   76 00                # abs 14     SCA length field, spec §2
 *
 * The `@N` directives place CITED SPANS at absolute frame offsets; a span may
 * be given in any order and the file need not cover the whole frame. Bytes
 * not covered by any span are zero-filled AND recorded as UNCITED. A test may
 * not assert on an uncited byte -- vms_fixture_is_cited() is the gate, and
 * the round-trip test proves the class's whole harvest span is cited before
 * it compares anything. That is how a specimen stays honest when the published
 * spec grounds some spans of a frame and not others (e.g. the 17-byte
 * capability span at abs 47 of a HELLO, which §4(a) records as present but
 * does not publish the bytes of).
 *
 * ORIGINS, and what each one claims:
 *   capture        Bytes extracted VERBATIM from the named manifest-hashed
 *                  capture. Only producible on the lab host, where the pcaps
 *                  live (they are host-only by the clean-room retention
 *                  procedure and are NOT in this repo). Strongest evidence.
 *   spec-composed  Bytes assembled from the GROUNDED field tables published in
 *                  docs/cluster-protocol-spec.md, citing the capture the spec
 *                  itself cites. This is the same epistemic standing as the
 *                  existing tests/vmsscs/test_scs_hello.c expectations. It is
 *                  NOT a capture extract and does not claim to be.
 *   synthetic      Not a VMS frame at all -- a negative control (a non-SCA
 *                  ethertype, a truncated frame). May only carry class
 *                  "unknown"; the loader enforces that.
 *
 * A later harvest item running on the lab host upgrades a specimen by
 * replacing its bytes with a capture extract and flipping `origin` -- no
 * loader or test change required.
 */
#ifndef OVMX_CLUSTER_FIXTURE_H
#define OVMX_CLUSTER_FIXTURE_H

#include <stddef.h>
#include <stdint.h>

#define VMS_FIXTURE_MAX_WIRE   2048u
#define VMS_FIXTURE_MAX_SPANS    64u
#define VMS_FIXTURE_MAX_FILES   128u
#define VMS_FIXTURE_ERRLEN      256u

enum vms_fixture_origin {
	VMS_FIXTURE_ORIGIN_CAPTURE = 0,
	VMS_FIXTURE_ORIGIN_SPEC,
	VMS_FIXTURE_ORIGIN_SYNTHETIC
};

struct vms_fixture_span {
	uint32_t off;
	uint32_t len;
};

struct vms_fixture {
	char     name[64];
	char     class_name[32];
	char     capture[128];
	char     spec[256];
	int      origin;          /* enum vms_fixture_origin */
	long     frame_index;     /* -1 when not stated */
	uint32_t wire_len;
	uint8_t  bytes[VMS_FIXTURE_MAX_WIRE];
	struct vms_fixture_span cited[VMS_FIXTURE_MAX_SPANS];
	unsigned n_cited;
	char     sha256[65];      /* as declared in the file */
	char     path[512];
};

/*
 * Load and fully validate one specimen:
 *   - header well-formed, every required key present for its origin;
 *   - spans in range, non-overlapping, and totalling <= wire-len;
 *   - the declared sha256 matches the assembled bytes;
 *   - `capture` (when present) is listed in `manifest_path`, the
 *     docs/clean-room/reference-captures.sha256 chain of custody.
 * Returns 0 on success; on failure returns -1 and writes a one-line reason.
 */
int vms_fixture_load(const char *path, const char *manifest_path,
		     struct vms_fixture *out, char *err, size_t errlen);

/* 1 iff every byte of [off, off+len) is inside a cited span. */
int vms_fixture_is_cited(const struct vms_fixture *f, uint32_t off, uint32_t len);

/* Total cited bytes (for reporting how much of a frame class is grounded). */
uint32_t vms_fixture_cited_bytes(const struct vms_fixture *f);

/*
 * Sorted list of *.spec paths in `dir`. Returns the count, or -1 on error.
 * Sorted so a test's output is stable and diffable run to run.
 */
int vms_fixture_list(const char *dir, char paths[][512], size_t max,
		     char *err, size_t errlen);

/*
 * Load every *.spec in `dir` (sorted). Returns the count, or -1 with a reason
 * naming the offending file. There is no "skip the broken one" mode: a
 * specimen that will not load is a red test, not a smaller corpus.
 */
int vms_fixture_load_all(const char *dir, const char *manifest_path,
			 struct vms_fixture *out, size_t max,
			 char *err, size_t errlen);

/* 1 iff `capture` (basename match) appears in the clean-room manifest. */
int vms_fixture_capture_in_manifest(const char *manifest_path,
				    const char *capture,
				    char *err, size_t errlen);

const char *vms_fixture_origin_name(int origin);

#endif /* OVMX_CLUSTER_FIXTURE_H */
