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


def load_expected():
    spec = importlib.util.spec_from_file_location("scs_connect_data_measure", MEASURE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.EXPECTED


def main():
    for p in (MEASURE, HEADER, SOURCE, SPEC):
        if not os.path.exists(p):
            print("missing: %s" % p)
            return 1
    exp = load_expected()
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

    print("[the two invariant spans are stated with their counts]")
    n = exp["vaxcluster_frames"]
    check("203/203" in header and str(n) == "203",
          "the version quad / tail spans are reported as %d/%d" % (n, n))
    check("01 1b 01 03" in header, "the version quad appears in scs_connect.h")
    check("08 00 00 06 00" in header, "the tail appears in scs_connect.h")

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
