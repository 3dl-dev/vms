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
are host-only and not in git. What ctest runs everywhere needs no captures: it
reads EXPECTED out of the same script and asserts every figure still appears in
the header and in the spec, and that the C constant, the header verdict and the
spec agree on the 16 bytes byte for byte.

AND SINCE vms-371, ON A HOST THAT HAS THE CAPTURES, ctest runs the other half
too: main() calls scs_connect_data_measure.rederive() and reds on any of its 67
figures the packets no longer support. That is not hypothetical -- six lab-2
captures once put 18 of those 67 into failure while `ctest -L scs` stayed 32/32
green, because this gate compared a table to prose and never opened a pcap. On
a host without the captures it now says so with a banner rather than silently.
See tests/vmsscs/scs_wire.py.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
# The overrides exist only so test_scs_figures_wire_mutants.py can point this
# gate at a scratch copy of the tree. Nothing in the repo sets them.
MEASURE = os.environ.get("OVMX_SCS_CONNDATA_MEASURE",
                         os.path.join(ROOT, "tools", "scs_connect_data_measure.py"))
HEADER = os.environ.get("OVMX_SCS_CONNDATA_HEADER",
                        os.path.join(ROOT, "src", "vmsscs", "include", "scs_connect.h"))
SOURCE = os.environ.get("OVMX_SCS_CONNDATA_SOURCE",
                        os.path.join(ROOT, "src", "vmsscs", "scs_connect.c"))
SPEC = os.environ.get("OVMX_SCS_CONNDATA_SPEC",
                      os.path.join(ROOT, "docs", "cluster-protocol-spec.md"))
# Every other file that quotes a connect-data census figure in a comment. They
# are scanned for the OVMX-inclusive totals too -- the correction is worthless
# if the rejected numbers survive in a comment three files away (vms-fdd).
CENSUS_CITERS = [
    os.path.join(ROOT, "src", "vmsscs", "scsd.c"),
    os.path.join(ROOT, "tests", "vmsscs", "test_scs_connect.c"),
]

failures = []
checks = 0

# ---------------------------------------------------------------------------
# THE REJECTED TOTALS, AND THE ONLY TWO WAYS THEY MAY APPEAR
# ---------------------------------------------------------------------------
# 1891 / 203 / 1052 are the OVMX-INCLUSIVE totals the veracity review rejected.
# They may not be cited as evidence about what a real VAX sends.
#
# The first version of this gate exempted any LINE containing the word "OVMX"
# or "exclud". That was a LOOPHOLE, and it was exploited: a line that
# legitimately mentions OVMX for an unrelated reason (e.g.
# "148/148 VAX-sourced frames across the library; OVMX's ...") became a free
# pass on which a rejected total could be reinstated. Word-presence anywhere on
# the line is not evidence that THIS NUMBER is the excluded one.
#
# The rule is now positional: a rejected total is allowed only INSIDE a span
# that names the corrected figure alongside it, in one of exactly two shapes --
#
#   (a) EXCLUSION ARITHMETIC   "<dropped> of <total>"     e.g. "466 of 1891"
#   (b) BEFORE -> AFTER        "<total> to|-> <corrected>" e.g. "203/203 to 148/148"
#
# Either way the reader is shown the number that replaced it. Every occurrence
# of the total must fall inside such a span; the word "OVMX" licenses nothing.
# The self-test at the bottom of this file asserts both directions.
REJECTED_TOTALS = {
    # total: (OVMX frames dropped, corrected VAX-only figure, what it counts)
    "1891": ("466", "1425", "connect frames including OVMX's own"),
    "203": ("55", "148", "VMS$VAXcluster frames including OVMX's own"),
    "1052": ("243", "809", "MSCP$DISK frames including OVMX's own"),
}
# Comment/markdown continuation between tokens: whitespace, C comment stars,
# markdown emphasis, backticks, table pipes.
_GAP = r"[\s*`|>_-]*"


def allowed_spans(text, total, dropped, corrected):
    """Character spans in which `total` is licensed. See REJECTED_TOTALS."""
    spans = []
    pats = (
        # (a) exclusion arithmetic: the dropped count, then the total
        r"(?<![\d.])%s%sof%s(?:the%s)?%s(?!\d)" % (dropped, _GAP, _GAP, _GAP, total),
        # (b) before -> after, with or without the N/N framing
        r"(?<![\d.])%s(?:/%s)?%s(?:->|-->|→|to)%s%s(?:/%s)?(?!\d)"
        % (total, total, _GAP, _GAP, corrected, corrected),
    )
    for pat in pats:
        for m in re.finditer(pat, text):
            spans.append(m.span())
    return spans


# ---------------------------------------------------------------------------
# THE REJECTED INDEPENDENCE WORDING, AND THE ONLY WAY IT MAY APPEAR
# ---------------------------------------------------------------------------
# The census was rejected TWICE for over-claiming how many sources agree. First
# "four independent real VAX nodes" (a source-MAC count). Then, after that was
# corrected, "3 independent hardware sources" / "distinct lab machines" -- also
# false, and worse, because it was the conservative figure the first correction
# retreated to: the five identities are three system ROOTS of ONE OpenVMS VAX
# V7.3 installation on ONE disk image, driven by three SIMH instances of one
# emulated model on one host.
#
# The corrected documents must still QUOTE those phrases, because the record of
# what was wrong is the point. So the rule is the same shape as the one above:
# the phrase is licensed only inside a span that also carries a correction
# marker, and bare use anywhere else reds. The self-test at the bottom asserts
# both directions.
REJECTED_INDEPENDENCE = (
    "independent hardware sources",
    "distinct lab machines",
    "independent real VAX nodes",
)
# Within this many characters either side, one of the markers must appear.
_MARKER_WINDOW = 400
_CORRECTION_MARKERS = ("false", "rejected", "previously said", "said next",
                       "earlier revision", "is also", "was the mac count",
                       "source-mac count")


def rejected_phrase_violations(text):
    """Every use of a rejected independence phrase with no correction nearby."""
    bad = []
    low = text.lower()
    for phrase in REJECTED_INDEPENDENCE:
        # Both sides lowercased -- an early version compared a phrase carrying
        # "VAX" against lowercased text and silently matched nothing.
        for m in re.finditer(re.escape(phrase.lower()), low):
            s, e = m.span()
            ctx = low[max(0, s - _MARKER_WINDOW):e + _MARKER_WINDOW]
            if not any(k in ctx for k in _CORRECTION_MARKERS):
                line = text.count("\n", 0, s) + 1
                bad.append((phrase, line,
                            " ".join(text[max(0, s - 60):e + 60].split())))
    return bad


def rejected_total_violations(text):
    """Every occurrence of a rejected total that no allowed span covers."""
    bad = []
    for total, (dropped, corrected, _what) in REJECTED_TOTALS.items():
        spans = allowed_spans(text, total, dropped, corrected)
        for m in re.finditer(r"(?<![\d.])%s(?![\d])" % total, text):
            s, e = m.span()
            if not any(a <= s and e <= b for a, b in spans):
                line = text.count("\n", 0, s) + 1
                ctx = text[max(0, s - 60):e + 60].replace("\n", " ")
                bad.append((total, line, " ".join(ctx.split())))
    return bad


def check(cond, what):
    global checks
    checks += 1
    if cond:
        print("  OK: %s" % what)
    else:
        print("  FAIL: %s" % what)
        failures.append(what)


def num_in(n, text):
    """
    vms-c66/vms-d04: `str(n) in text` is a SUBSTRING match. Several figures
    this gate pins (e.g. ovmx_connect_frames, ovmx_vaxcluster_frames) are only
    two or three digits, small enough to appear as a sibling inside an
    unrelated larger number anywhere in a multi-thousand-line spec or header --
    which means a real drift in the measured figure could still find some
    OTHER digit-string match and pass. Word-boundary the search instead.
    """
    return re.search(r"(?<!\d)%d(?!\d)" % n, text) is not None


def flat(text):
    """Comment/markdown text as one whitespace-normalised line.

    A phrase that straddles a C comment line break reads as "ONE\\n * OBSERVATION"
    in the file; without this, a literal check for it fails for a reason that
    has nothing to do with what the comment says.
    """
    return " ".join(text.replace("\n *", " ").split())


_WORD_NUM = {"one": "1", "two": "2", "three": "3", "four": "4", "five": "5"}


def numeric(text):
    """Small spelled-out numbers as digits.

    "THREE SYSTEM ROOTS" and "3 system roots" are the same claim; without this,
    a count check can be dodged by spelling the number out.
    """
    return re.sub(r"\b(one|two|three|four|five)\b",
                  lambda m: _WORD_NUM[m.group(1).lower()], text, flags=re.I)


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
    """Execute the measure script FROM SOURCE, never from cached bytecode.

    importlib's file loader will happily reuse a stale tools/__pycache__ entry,
    and this gate reads EXPECTED out of that module: a mutation test once got a
    false result because the .pyc still held the pre-restore table. Compiling
    the source text directly removes the cache from the path entirely.

    The loader now lives in scs_wire.load_source() so all six figures gates
    share ONE implementation and the mutation battery attacks one place.
    """
    return scs_wire.load_source(MEASURE, "scs_connect_data_measure")


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
        check(num_in(value, header), "%s (%d) appears in scs_connect.h" % (label, value))
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

    print("[the rejected-total rule licenses the NUMBER, not the word 'OVMX']")
    # Self-test of rejected_total_violations() itself, so the loophole cannot
    # come back unnoticed. These strings are literals, not file content.
    for good in ("excluded here: 466 of 1891, of which 55 are VMS$VAXcluster",
                 "moved these counts from `203/203` to `148/148`",
                 "quarter of the frames were OVMX's: 466 of\n * 1891 excluded",
                 "MSCP$DISK 1052 -> 809 after the exclusion",
                 "243 of the 1052 MSCP$DISK frames were OVMX's"):
        check(not rejected_total_violations(good),
              "ALLOWED: a total shown next to the figure that replaced it (%r)"
              % good[:46])
    for bad in (
            # the exact mutant the same-line exemption licensed
            "* our own builder (203/203 VAX-sourced frames across the library;"
            " OVMX's",
            # word-presence anywhere on the line must not license the number
            "Across ALL 203 VAX-sourced frames, and OVMX sends the same thing",
            "the 1891 connect frames, OVMX excluded, agree",
            "MSCP$DISK sends it in 1052/1052 frames (OVMX runs no such SYSAP)"):
        check(bool(rejected_total_violations(bad)),
              "REJECTED: a bare total on a line that merely mentions OVMX (%r)"
              % bad[:46])
    check(bool(rejected_total_violations("148/148 frames; OVMX's own 203/203")),
          "REJECTED: 'OVMX's own <total>' without the corrected figure")

    print("[the circular-grounding guard is stated, not just implemented]")
    # The census must NOT quote the OVMX-INCLUSIVE totals as evidence about what
    # a real VAX sends. Scoped to the vms-fdd prose (elsewhere in the spec those
    # digits mean unrelated things, e.g. a byte offset).
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
    per_figure = {f: [] for f in REJECTED_TOTALS}
    for docname, text in blocks:
        for figure, line, ctx in rejected_total_violations(text):
            per_figure[figure].append("%s:%d: ...%s..." % (docname, line, ctx))
    for figure, (_d, corrected, what) in REJECTED_TOTALS.items():
        bad = per_figure[figure]
        check(not bad,
              "%s is never cited as VMS evidence (only beside %s)%s"
              % (what, corrected,
                 "" if not bad else "\n        offending: %s" % bad[0]))
    # Distinctive phrases, so deleting the guard paragraph reds this. "OVMX"
    # and "VAX-sourced" alone would not: both occur all over these files.
    for phrase in ("NOT EVIDENCE ABOUT VMS", "VAX-SOURCED FRAMES ONLY"):
        check(phrase in connect_data_verdict(header),
              "the CONNECT DATA verdict states the guard (%r)" % phrase)
    for phrase in ("circular grounding", "VAX-sourced frames only"):
        check(any(phrase in b for b in spec_connect_data_blocks(spec)),
              "spec 4(N) states the guard (%r)" % phrase)
    check(num_in(exp["ovmx_connect_frames"], header) and num_in(exp["ovmx_connect_frames"], spec),
          "the dropped OVMX frame count (%d) is stated in both documents"
          % exp["ovmx_connect_frames"])
    check(num_in(exp["ovmx_vaxcluster_frames"], header)
          and num_in(exp["ovmx_vaxcluster_frames"], spec),
          "the dropped OVMX VMS$VAXcluster count (%d) is stated in both documents"
          % exp["ovmx_vaxcluster_frames"])
    check("SIOCGIFHWADDR" in header and "SIOCGIFHWADDR" in spec,
          "both documents say WHY the source MAC identifies OVMX reliably")
    for figure, label in ((exp["adopted_value_vax_frames"], "VAX frames carrying it"),
                          (exp["adopted_value_vax_frames_outside_specimen"],
                           "of them outside the specimen"),
                          (exp["adopted_value_vax_captures"], "captures")):
        check(num_in(figure, header) and num_in(figure, spec),
              "the adopted value's independent attestation (%d %s) is stated"
              % (figure, label))

    print("[a MAC is not a node -- identity counts, not source-MAC counts]")
    # The rejected census counted source MACs and the prose called them
    # "independent nodes". Both counts must now be stated, in both documents,
    # and the tool's own identity plumbing must still be wired.
    hverdict = connect_data_verdict(header)
    sblocks = spec_connect_data_blocks(spec)
    for phrase in ("A MAC IS NOT A NODE", "NODE IDENTITIES", "HARDWARE SOURCES"):
        check(phrase in hverdict,
              "the CONNECT DATA verdict distinguishes MACs from nodes (%r)" % phrase)
    for phrase in ("A MAC IS NOT A NODE", "node identities", "hardware sources"):
        check(any(phrase in b for b in sblocks),
              "spec 4(N) distinguishes MACs from nodes (%r)" % phrase)
    # The independence figure DROPPED from 4 to 3; both documents must say so
    # rather than quietly restating a larger number.
    for doc, name in ((hverdict, "scs_connect.h"), ("\n".join(sblocks), "the spec")):
        check("independence figure" in doc.lower() and "dropped" in doc.lower(),
              "%s records that the independence figure dropped" % name)
    ident = exp["vaxcluster_node_identities"]
    hw = exp["vaxcluster_hardware_sources"]
    # EVERY place either count is stated must state the SAME number. A single
    # phrase check is not enough -- each count appears in a table cell and in
    # several sentences, and flipping any ONE of them is the regression.
    counted = {
        r"(\d+)\s*(?:distinct\s+|independent\s+)?node identities": ident,
        r"(\d+)\s*emulator\s+instances": hw,
        r"distinct cluster members\.\s*(\d+) here": ident,
        r"distinct SIMH instances\.\s*(\d+) here": hw,
        r"\|\s*\*\*node identities\*\*[^\n]*?\|\s*\*\*(\d+)\*\*\s*\|": ident,
        r"\|\s*\*\*hardware sources\*\*[^\n]*?\|\s*\*\*(\d+)\*\*\s*\|": hw,
        r"\|\s*\*\*emulator instances\*\*[^\n]*?\|\s*\*\*(\d+)\*\*\s*\|": hw,
    }
    for docname, doc in (("scs_connect.h", hverdict),
                         ("the spec", "\n".join(sblocks))):
        seen, wrong = 0, []
        for pat, want in counted.items():
            for m in re.finditer(pat, doc):
                seen += 1
                if int(m.group(1)) != want:
                    wrong.append("%r -> %s (want %d)" % (pat, m.group(1), want))
        check(seen >= 3 and not wrong,
              "%s states the identity/independence counts and every statement of"
              " them agrees (%d sites%s)"
              % (docname, seen, "" if not wrong else "; WRONG: %s" % wrong[0]))
    # The per-identity census must appear frame-count for frame-count, so the
    # 148 total cannot silently go back to being attributed to 4 "nodes".
    for nodename, n in exp["vaxcluster_node_census"].items():
        check(re.search(r"%s\D{0,4}%d\b" % (re.escape(nodename), n), hverdict)
              is not None,
              "the header's per-identity census row %s = %d" % (nodename, n))
    # In the spec the census is a table; require both rows VERBATIM, built from
    # EXPECTED, so editing any single cell reds this.
    names = list(exp["vaxcluster_node_census"])
    head_row = "| node | " + " | ".join(names) + " | total |"
    data_row = ("| frames | "
                + " | ".join(str(exp["vaxcluster_node_census"][k]) for k in names)
                + " | **%d** |" % exp["vaxcluster_frames"])
    check(any(head_row in b for b in sblocks),
          "the spec's per-identity census header row is verbatim (%r)" % head_row)
    check(any(data_row in b for b in sblocks),
          "the spec's per-identity census frame row is verbatim (%r)" % data_row)
    check(sum(exp["vaxcluster_node_census"].values()) == exp["vaxcluster_frames"],
          "the per-identity census sums to vaxcluster_frames (%d)"
          % exp["vaxcluster_frames"])
    check(len(exp["vaxcluster_node_census"]) == ident,
          "the per-identity census has exactly %d rows" % ident)
    check(len(exp["hardware_source_groups"]) == hw,
          "hardware_source_groups has exactly %d groups" % hw)
    check(sorted(n for _macs, names in exp["hardware_source_groups"] for n in names)
          == sorted(exp["vaxcluster_node_census"]),
          "the hardware groups partition exactly the census identities")
    # The adopted value's own attestation, which has its own EXPECTED keys and
    # must not quietly revert to the source-MAC count it used to quote.
    aid, ahw = (exp["adopted_value_vax_node_identities"],
                exp["adopted_value_vax_hardware_sources"])
    attest = re.compile(
        r"%d distinct node identities on\s+%d emulator\s+instances" % (aid, ahw))
    for docname, doc in (("scs_connect.h", hverdict),
                         ("the spec", "\n".join(sblocks))):
        check(attest.search(flat(doc)) is not None,
              "%s attests the adopted value to %d identities on %d hardware sources"
              % (docname, aid, ahw))
    check(aid <= ident and ahw <= hw,
          "the adopted value's attestation cannot exceed the whole census")
    check(exp["vaxcluster_source_macs"] != ident
          and exp["vaxcluster_source_macs"] != hw,
          "the source-MAC count (%d) is recorded as a DIFFERENT number from both"
          " identity counts" % exp["vaxcluster_source_macs"])
    # The tool's identity plumbing, checkable without captures.
    check(measure.src_lavc_node(b"\x00" * 10 + b"\xaa\x00\x04\x00\x1a\x04"
                                + b"\x00" * 94) == 0x1a,
          "src_lavc_node() reads the LAVC node number out of payload [10:16]")
    check(measure.src_lavc_node(b"\x00" * 10 + b"\xb6\x16\x8a\xdc\x3a\x53"
                                + b"\x00" * 94) is None,
          "src_lavc_node() returns None (a residual) for a non-LAVC source")
    comps = measure.hardware_components({
        "08:00:2b:4a:b7:15": {1}, "aa:00:04:00:01:04": {1},
        "08:00:2b:78:56:b9": {2, 26, 75}, "08:00:2b:11:22:33": {3},
    })
    check(sorted(len(c[0]) for c in comps) == [1, 1, 2],
          "hardware_components() merges the two MACs of one node")
    check(sorted(len(c[1]) for c in comps) == [1, 1, 3],
          "hardware_components() keeps the three identities of one MAC apart")
    check(len(comps) == hw,
          "hardware_components() finds %d machines for the lab's MAC/identity map"
          % hw)

    print("[3 is not an independence count -- the configuration is stated]")
    # The second rejection: "3 independent hardware sources" / "distinct lab
    # machines" was also false. The honest configuration -- 1 VMS build, 3
    # system roots, 1 disk image, 3 emulator instances -- has to be stated
    # where the counts are, in BOTH documents, and the value's grounding has to
    # be marked in spec sec 5 rather than only in sec 4(N).
    # Self-test of rejected_phrase_violations() first, both directions.
    for good in ('previously said "3 independent hardware sources"; that is'
                 ' also false',
                 'an earlier revision called them distinct lab machines',
                 'the rejected wording "four independent real VAX nodes"'):
        check(not rejected_phrase_violations(good),
              "ALLOWED: the rejected phrase quoted beside its correction (%r)"
              % good[:46])
    for bad in ("attested on 3 independent hardware sources, 0 residuals",
                "the three distinct lab machines agree byte for byte",
                "confirmed by four independent real VAX nodes"):
        check(bool(rejected_phrase_violations(bad)),
              "REJECTED: the rejected phrase asserted as fact (%r)" % bad[:46])
    for docname, doc in (("scs_connect.h", hverdict),
                         ("the spec", "\n".join(sblocks))):
        bad = rejected_phrase_violations(doc)
        check(not bad,
              "%s never asserts the rejected independence wording%s"
              % (docname, "" if not bad else "\n        offending: %s: %s"
                 % (bad[0][0], bad[0][2])))
    # The configuration itself, as numbers, in both documents. EVERY statement
    # of each count must agree with the tool's declared configuration -- "at
    # least one site is right" is not enough and was measured not to be: a
    # first version of this check accepted a header that said "4 system roots"
    # in one sentence and "3 SYSTEM ROOTS" in another, which is precisely the
    # fix-the-named-instance-and-leave-the-siblings defect this item exists to
    # kill. Counts spelled as words are normalised first, so "THREE SYSTEM
    # ROOTS" cannot dodge the check by not being a digit.
    config_counted = {
        r"(\d+)\s+VMS (?:build|installation)s?": exp["lab_vms_installations"],
        r"(\d+)\s+system roots?": exp["lab_system_roots"],
        r"(\d+)\s+(?:system )?disk images?": exp["lab_system_disk_images"],
        r"(\d+)\s+emulator instances?": exp["lab_emulator_instances"],
    }
    for docname, doc in (("scs_connect.h", numeric(flat(hverdict))),
                         ("the spec", numeric(flat("\n".join(sblocks))))):
        for pat, want in config_counted.items():
            hits = [int(m.group(1)) for m in re.finditer(pat, doc, re.I)]
            check(bool(hits) and all(h == want for h in hits),
                  "%s states %s and every statement of it says %d (%d sites: %s)"
                  % (docname, pat, want, len(hits), hits))
    # "Last run: N checks, 0 failures" is itself a figure, and it drifted --
    # both documents said 57 while the script ran 66. It now comes from
    # EXPECTED, and the script asserts its own total against the same key, so
    # neither half can move without the other.
    runline = re.compile(r"(\d+)\s*checks,\s*0\s*failures")
    for docname, doc in (("scs_connect.h", flat(hverdict)),
                         ("the spec", flat("\n".join(sblocks)))):
        hits = [int(x) for x in runline.findall(doc)]
        check(bool(hits) and all(h == exp["measure_check_count"] for h in hits),
              "%s reports the measure script's run as %d checks (%d sites: %s)"
              % (docname, exp["measure_check_count"], len(hits), hits))
    # The measured half: one version string, and the frame count behind it.
    for docname, doc in (("scs_connect.h", flat(hverdict)),
                         ("the spec", "\n".join(sblocks))):
        check(num_in(exp["vax_start_frames"], doc),
              "%s states the %d VAX START frames the version count came from"
              % (docname, exp["vax_start_frames"]))
        for v in exp["vax_vms_versions"]:
            check(v in doc, "%s names the single measured version %r"
                  % (docname, v))
    check(len(exp["vax_vms_versions"]) == exp["lab_vms_installations"],
          "the measured version count and the declared install count agree")
    # "one observation repeated" is the honest reading and must be said, not
    # left for the reader to work out from the table.
    for docname, doc in (("scs_connect.h", flat(hverdict).lower()),
                         ("the spec", flat("\n".join(sblocks)).lower())):
        check("one observation repeated" in doc,
              "%s says what three roots of one install actually amount to"
              % docname)
    # Section 5 must carry the limit -- 4(N) marking its own grounding is not
    # enough, because sec 5 is the register a reader checks for what is NOT
    # grounded. Located independently of spec_connect_data_blocks().
    m5 = re.search(r"\n## 5\. Summary of unknown/inferred fields.*", spec, re.S)
    sec5 = m5.group(0) if m5 else ""
    check(bool(sec5), "spec section 5 was located")
    check("STANDING LIMIT" in sec5 and "ONE VMS BUILD" in sec5,
          "spec section 5 carries the one-VMS-build limit on 4(N)")
    for token in ("3 system roots", "1 system disk image", "SYS11", "d0.dsk"):
        check(token in sec5, "the section 5 limit names %r" % token)
    check(num_in(exp["vax_start_frames"], sec5),
          "the section 5 limit states the %d START frames behind it"
          % exp["vax_start_frames"])
    # ...and the sec 5 GROUNDED bullet for this field must not restate a node
    # count that disagrees with the census (the exact defect that was rejected:
    # "148/148 ... from 4 nodes" survived there after 4(N) was corrected).
    m5b = re.search(r"\n- \*\*SCA connect data[^\n]*\*\*.*?(?=\n\n)", sec5, re.S)
    bullet = m5b.group(0) if m5b else ""
    check(bool(bullet), "the section 5 SCA connect-data bullet was located")
    for mm in re.finditer(r"(\d+)\s*node", bullet):
        check(int(mm.group(1)) == ident,
              "the section 5 bullet's node count is %d, not %s"
              % (ident, mm.group(1)))
    check("emulator instances" in bullet and str(hw) in bullet,
          "the section 5 bullet reports the source count as emulator instances")
    check(not rejected_phrase_violations(bullet),
          "the section 5 bullet does not assert the rejected wording")

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

    # --- THE WIRE ITSELF (vms-371) -----------------------------------------
    # Everything above pins the PROSE to EXPECTED. This pins EXPECTED to the
    # PACKETS. Together they are what makes a green run mean "the prose matches
    # a re-derived measurement" instead of "the prose matches a table that also
    # drifted". `logdir=None` skips only the scsd-log cross-check, which reads
    # a log directory and not a capture; the tool records that as a skip.
    scs_wire.gate("scs_connect_data_figures", measure, measure.DEFAULT_CAPDIR,
                  check, logdir=os.environ.get("OVMX_SCSD_LOGS"))

    print("\n%d checks, %d failure(s)" % (checks, len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
