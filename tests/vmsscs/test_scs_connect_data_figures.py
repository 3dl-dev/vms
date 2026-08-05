#!/usr/bin/env python3
"""
test_scs_connect_data_figures.py -- vms-fdd.

The CONNECT DATA verdict in src/vmsscs/include/scs_connect.h and section 4(n)
of docs/cluster-protocol-spec.md are PROSE, and prose drifts. The bytes OVMX
stamps into its VMS$VAXcluster CONNECT_REQ are a version claim that a peer is
documented to reject on (VAXcluster Principles p. 2-25), so the measurement
behind them has to stay pinned to the words that report it.

tools/scs_connect_data_measure.py re-derives every figure from the lab-1
captures and PASS/FAILs it against a checked-in EXPECTED table. Those captures
are host-only and not in git, so ctest cannot run that half -- run it by hand
on a lab host. What ctest DOES run needs no captures: it reads EXPECTED out of
the same script and asserts every figure still appears in the header and in the
spec, and that the C constant, the header verdict and the spec agree on the
16 bytes byte for byte.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
MEASURE = os.path.join(ROOT, "tools", "scs_connect_data_measure.py")
HEADER = os.path.join(ROOT, "src", "vmsscs", "include", "scs_connect.h")
SOURCE = os.path.join(ROOT, "src", "vmsscs", "scs_connect.c")
SPEC = os.path.join(ROOT, "docs", "cluster-protocol-spec.md")
# Every other file that quotes a connect-data census figure in a comment. They
# are scanned for the OVMX-inclusive totals too -- the correction is worthless
# if the rejected numbers survive in a comment three files away (vms-fdd).
CENSUS_CITERS = [
    os.path.join(ROOT, "src", "vmsscs", "scsd.c"),
    os.path.join(ROOT, "tests", "vmsscs", "test_scs_connect.c"),
]

failures = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if cond:
        print("  OK: %s" % what)
    else:
        print("  FAIL: %s" % what)
        failures.append(what)


def connect_data_verdict(header):
    """The CONNECT DATA verdict block of scs_connect.h, or "" if it moved."""
    m = re.search(r"\* CONNECT DATA -- THE 16-BYTE SCA FIELD.*?(?=\n#ifndef)", header, re.S)
    return m.group(0) if m else ""


def spec_connect_data_blocks(spec):
    """Section 4(N) and the §5 connect-data bullets, or [] if they moved."""
    out = []
    m = re.search(r"### 4\(N\) The 16-byte SCA connect data.*?\n---\n", spec, re.S)
    if m:
        out.append(m.group(0))
    for m in re.finditer(r"\n- \*\*[^\n]*connect data[^\n]*\*\*.*?(?=\n\n)", spec, re.S):
        out.append(m.group(0))
    return out


def load_measure():
    spec = importlib.util.spec_from_file_location("scs_connect_data_measure", MEASURE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def main():
    for p in (MEASURE, HEADER, SOURCE, SPEC):
        if not os.path.exists(p):
            print("missing: %s" % p)
            return 1
    measure = load_measure()
    exp = measure.EXPECTED
    header = open(HEADER, encoding="utf-8").read()
    source = open(SOURCE, encoding="utf-8").read()
    spec = open(SPEC, encoding="utf-8").read()

    print("[the measured figures appear in the header verdict]")
    for label, value in (
        ("pcaps scanned", exp["pcaps_scanned"]),
        ("connect frames", exp["connect_frames"]),
        ("VMS$VAXcluster frames", exp["vaxcluster_frames"]),
    ):
        check(str(value) in header, "%s (%d) appears in scs_connect.h" % (label, value))
    hist = exp["msgtype_histogram"]
    check("{0: %d, 2: %d, 10: %d}" % (hist[0], hist[2], hist[10]) in header,
          "the 110-byte message-type histogram appears verbatim in scs_connect.h")
    for name, (n, distinct) in exp["sysap_census"].items():
        check(re.search(r"%s\s+%d frames, %d distinct" % (re.escape(name), n, distinct), header)
              is not None,
              "per-SYSAP census row for %s (%d frames, %d distinct)" % (name, n, distinct))

    print("[the guard's own constants are self-consistent -- no captures needed]")
    # Re-running the measurement needs the lab captures, so CI cannot catch a
    # tool edit that lets OVMX back into the VAX population. These four checks
    # can: they are pure consistency between the tool's constants and EXPECTED.
    check(bool(measure.OVMX_HW_MACS), "OVMX_HW_MACS is not empty")
    missing = [s for s in exp["ovmx_source_macs"] if s not in measure.OVMX_HW_MACS]
    check(not missing,
          "every MAC EXPECTED attributes to OVMX is in OVMX_HW_MACS (missing: %s)"
          % (missing or "none"))
    check(all(measure.classify_source(s) == measure.OVMX for s in exp["ovmx_source_macs"]),
          "classify_source() places every EXPECTED OVMX source as OVMX")
    check(measure.classify_source(measure.VAX3_HW) == measure.VAX
          and measure.classify_source(measure.VAX1_LOGICAL) == measure.VAX,
          "classify_source() places both specimen endpoints as VAX")
    check(measure.classify_source("b6:16:8a:00:00:01") is None,
          "an unknown locally-administered MAC is UNCLASSIFIED, not silently VAX")

    print("[the circular-grounding guard is stated, not just implemented]")
    # The census must NOT quote the OVMX-INCLUSIVE totals as evidence about what
    # a real VAX sends. 1891 / 203 / 1052 are exactly those totals -- they are
    # the figures the veracity review rejected. Scoped to the vms-fdd prose
    # (elsewhere in the spec those digits mean unrelated things, e.g. a byte
    # offset), and evaluated per PARAGRAPH so a sentence may still cite a total
    # while explaining that it was excluded.
    circular = {
        "1891": "connect frames including OVMX's own",
        "203": "VMS$VAXcluster frames including OVMX's own",
        "1052": "MSCP$DISK frames including OVMX's own",
    }
    blocks = [("scs_connect.h", connect_data_verdict(header))]
    blocks += [("the spec", b) for b in spec_connect_data_blocks(spec)]
    check(all(b for _n, b in blocks) and len(blocks) >= 2,
          "the vms-fdd prose blocks were located in both documents")
    # The C array's own comment, and every other file that cites the census.
    blocks.append(("scs_connect.c", source))
    for path in CENSUS_CITERS:
        check(os.path.exists(path), "census citer %s exists" % os.path.basename(path))
        if os.path.exists(path):
            blocks.append((os.path.basename(path), open(path, encoding="utf-8").read()))
    for figure, what in circular.items():
        # Word-boundaried so 1891 does not match 11891 and 203 does not match
        # a byte offset like [72..203] -> that one is out of scope anyway.
        pat = re.compile(r"(?<![\d.])%s(?![\d])" % figure)
        bad = []
        for docname, text in blocks:
            for i, line in enumerate(text.splitlines()):
                if not pat.search(line):
                    continue
                # A total may be cited ONLY on a line that SAYS it is OVMX's or
                # excluded. Deliberately same-line, not a window: in the
                # corrected text the guard prose is always a line or two away,
                # so a window-based rule would license exactly the regression
                # this gate exists to catch (measured -- a ±2-line version let
                # five of eight mutants through).
                if "OVMX" not in line and "exclud" not in line.lower():
                    bad.append("%s:%d: %s" % (docname, i + 1, line.strip()[:90]))
        check(not bad,
              "%s is never cited as VMS evidence%s"
              % (what, "" if not bad else "\n        offending: %s" % bad[0]))
    # Distinctive phrases, so deleting the guard paragraph reds this. "OVMX"
    # and "VAX-sourced" alone would not: both occur all over these files.
    for phrase in ("NOT EVIDENCE ABOUT VMS", "VAX-SOURCED FRAMES ONLY"):
        check(phrase in connect_data_verdict(header),
              "the CONNECT DATA verdict states the guard (%r)" % phrase)
    for phrase in ("circular grounding", "VAX-sourced frames only"):
        check(any(phrase in b for b in spec_connect_data_blocks(spec)),
              "spec 4(N) states the guard (%r)" % phrase)
    check(str(exp["ovmx_connect_frames"]) in header and str(exp["ovmx_connect_frames"]) in spec,
          "the dropped OVMX frame count (%d) is stated in both documents"
          % exp["ovmx_connect_frames"])
    check(str(exp["ovmx_vaxcluster_frames"]) in header
          and str(exp["ovmx_vaxcluster_frames"]) in spec,
          "the dropped OVMX VMS$VAXcluster count (%d) is stated in both documents"
          % exp["ovmx_vaxcluster_frames"])
    check("SIOCGIFHWADDR" in header and "SIOCGIFHWADDR" in spec,
          "both documents say WHY the source MAC identifies OVMX reliably")
    for figure, label in ((exp["adopted_value_vax_frames"], "VAX frames carrying it"),
                          (exp["adopted_value_vax_frames_outside_specimen"],
                           "of them outside the specimen"),
                          (exp["adopted_value_vax_nodes"], "distinct VAX nodes")):
        check(str(figure) in header and str(figure) in spec,
              "the adopted value's independent attestation (%d %s) is stated"
              % (figure, label))

    print("[the two invariant spans are stated with their counts]")
    n = exp["vaxcluster_frames"]
    check("%d/%d" % (n, n) in header, "the version quad / tail spans are reported as %d/%d"
          % (n, n))
    check("01 1b 01 03" in header, "the version quad appears in scs_connect.h")
    check("08 00 00 06 00" in header, "the tail appears in scs_connect.h")

    print("[the ungrounded middle span is enumerated, not summarised]")
    for value, n in exp["vaxcluster_mid_values"].items():
        check(value in header and value in spec,
              "[98:105] value %s (%d frames) is listed in both documents" % (value, n))

    print("[the OVMX value agrees across the tool, the header, the C array and the spec]")
    val = exp["ovmx_value"]
    check(val in header, "the 16-byte value appears verbatim in scs_connect.h")
    check(val in spec, "the 16-byte value appears verbatim in the spec")
    # The C array, recovered from scs_connect.c and re-rendered.
    m = re.search(r"scs_connect_data_vaxcluster\[SCS_CONNECT_DATA_LEN\]\s*=\s*\{(.*?)\}",
                  source, re.S)
    check(m is not None, "scs_connect_data_vaxcluster[] is defined in scs_connect.c")
    if m:
        body = re.sub(r"/\*.*?\*/", " ", m.group(1), flags=re.S)
        bytes_ = re.findall(r"0x([0-9a-fA-F]{2})", body)
        rendered = " ".join(b.lower() for b in bytes_)
        check(len(bytes_) == 16, "the C array holds exactly 16 bytes (got %d)" % len(bytes_))
        check(rendered == val,
              "the C array is byte-identical to the measured value\n"
              "        C   : %s\n        want: %s" % (rendered, val))

    print("[the member contrast and the RE gap are both recorded]")
    check(exp["member_value_in_specimen"] in header,
          "the contrasting MEMBER value from the specimen appears in scs_connect.h")
    check("vax3-2to3-established-join-20260730.pcap" in header,
          "the specimen is named in scs_connect.h")
    check("tools/scs_connect_data_measure.py" in header,
          "the header says how to re-derive its figures")
    check("OVMX_NO_CONNECT_DATA" in header, "the kill switch is documented in scs_connect.h")
    for phrase in ("[98:105]", "INFERRED", "not grounded"):
        check(phrase in header, "the header states the RE gap over %r" % phrase)
    check("connect data" in spec.lower() and "4(N)" in spec,
          "the spec carries a section 4(N) on connect data")
    check("[98:105]" in spec, "the spec records the ungrounded span")

    print("\n%d checks, %d failure(s)" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
