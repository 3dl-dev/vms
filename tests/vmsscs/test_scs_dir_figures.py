#!/usr/bin/env python3
"""
test_scs_dir_figures.py -- vms-66f.

Two claims in this epic were REFUTED by the very capture they cited, and both
lived in prose next to correct code:

  1. docs/cluster-protocol-spec.md sec 4(h)(2a) and src/vmsscs/scsd.c said the
     reference JOINER "only answers" and opens its own VMS$VAXcluster connection
     "without having polled anybody". The capture says the opposite on both
     counts: the only VMS$VAXcluster CONNECT_REQ in the file is the MEMBER's,
     and the joiner does poll.
  2. src/vmsscs/scs_dir.c commented SCA [48:50] on the lookup REQUEST template
     as a fixed 0, "a request; the response has 1". The capture's 12 lookup
     messages put 1 on four requests and on all six responses.

tools/cluster/scs_dir_role_measure.py re-derives both censuses from the golden
capture and PASS/FAILs them against a checked-in EXPECTED table. That capture is
host-only and not in git, so ctest cannot run that half -- run it by hand on a
lab host. What ctest DOES run needs no capture:

  (A) FIGURES ARE PINNED. Every figure the prose states is read out of
      EXPECTED and must appear verbatim in the spec and in the source that
      quotes it. Editing EXPECTED without editing the prose reds.
  (B) THE REFUTED SENTENCES STAY DEAD. Each rejected phrase may appear ONLY
      between an explicit REFUTED-QUOTE-BEGIN / REFUTED-QUOTE-END pair -- the
      same delimiter vms-6b3 introduced for exactly this. Anywhere else, in any
      scanned file, it reds.

      THE FIRST DRAFT OF (B) WAS A PROXIMITY WINDOW and it is written down here
      because it is the defect vms-6b3 was rejected for: a window of N lines
      around "REFUTED"/"corrected"/the item id passes trivially inside documents
      that are entirely ABOUT the refutation, so a re-assertion pasted three
      lines under the correction survives it. The delimiter is exact, and
      test_scs_dir_mutants.py MEASURES the kill count on every ctest run rather
      than asserting one in a comment.

Path overrides (used only by the mutation battery, which runs this gate against
a scratch copy): OVMX_SCS_DIR_ROOT relocates every input.
"""
import importlib.util
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.environ.get("OVMX_SCS_DIR_ROOT") or os.path.abspath(os.path.join(HERE, "..", ".."))
MEASURE = os.path.join(ROOT, "tools", "cluster", "scs_dir_role_measure.py")
SPEC = os.path.join(ROOT, "docs", "cluster-protocol-spec.md")
DIR_C = os.path.join(ROOT, "src", "vmsscs", "scs_dir.c")
SCSD_C = os.path.join(ROOT, "src", "vmsscs", "scsd.c")

# Every file that may plausibly restate a refuted sentence.
SCANNED = [SPEC, DIR_C, SCSD_C,
           os.path.join(ROOT, "src", "vmsscs", "include", "scs_dir.h"),
           os.path.join(ROOT, "src", "vmsscs", "include", "scs_poll.h"),
           os.path.join(ROOT, "src", "vmsscs", "scs_poll.c"),
           os.path.join(ROOT, "tests", "vmsscs", "test_scs_dir.c"),
           os.path.join(ROOT, "tests", "vmsscs", "test_scs_poll.c"),
           MEASURE]

failures = []
checks = 0


def check(cond, what):
    global checks
    checks += 1
    if cond:
        print(f"  OK: {what}")
    else:
        print(f"  FAIL: {what}")
        failures.append(what)


def load_expected():
    spec = importlib.util.spec_from_file_location("scs_dir_role_measure", MEASURE)
    mod = importlib.util.module_from_spec(spec)
    # The module only reads a pcap inside main(); importing is side-effect free.
    spec.loader.exec_module(mod)
    return mod.EXPECTED


def read(path):
    with open(path, encoding="utf-8") as f:
        return f.read()


# ---------------------------------------------------------------------------
# (A) the figures the prose is allowed to state, read out of EXPECTED
# ---------------------------------------------------------------------------
print("[A] every stated figure is the measured figure")
E = load_expected()
spec_txt, dir_txt, scsd_txt = read(SPEC), read(DIR_C), read(SCSD_C)

# CENSUS B histograms, exactly as EXPECTED renders them.
req_hist = repr(E["request_credit_hist"])           # "{0: 2, 1: 4}"
rsp_hist = repr(E["response_credit_hist"])          # "{1: 6}"
for where, txt in (("spec sec 4(h)(2a)", spec_txt), ("scs_dir.c", dir_txt)):
    check(req_hist in txt, f"{where} states the REQUEST [48:50] histogram {req_hist}")
    check(rsp_hist in txt, f"{where} states the RESPONSE [48:50] histogram {rsp_hist}")

check(str(E["lookup_requests"]) in spec_txt and str(E["lookup_responses"]) in spec_txt,
      "the spec states the 6/6 request/response split")
check(f"{E['lookup_frames_total']}" in spec_txt and f"{E['lookup_frames_total']}" in dir_txt,
      f"both state the lookup-message total ({E['lookup_frames_total']})")

# CENSUS A frame numbers. These are the load-bearing ones -- they are what
# inverts the roles -- so both the spec and scsd.c must carry each of them.
#
# A BARE DIGIT SEARCH IS NOT ENOUGH and this was measured: with `str(47) in
# txt`, drifting EXPECTED from 47 to 48 SURVIVED the battery, because "48"
# occurs all over a spec full of byte offsets. Each frame must be named WITH
# ITS DIRECTION, which is the part that carries the claim.
ROLE_FIGURES = [
    (E["vaxcluster_connect_req_frames"][0], "the only VMS$VAXcluster CONNECT_REQ"),
    (E["vaxcluster_accept_req_frames"][0], "the joiner's ACCEPT_REQ"),
    (E["directory_connect_req_frames"][1], "the joiner's OWN directory CONNECT_REQ"),
]
spec_plain = spec_txt.replace("**", "")
for (frame, src, dst), what in ROLE_FIGURES:
    check(f"{frame}, {src} → {dst}" in spec_plain,
          f"the spec names frame {frame} as {src}->{dst} ({what})")
    check(f"frame {frame}, {src} -> {dst}" in scsd_txt,
          f"scsd.c names frame {frame} as {src}->{dst} ({what})")

check(E["vaxcluster_connect_req_from_joiner"] == 0,
      "EXPECTED still records ZERO joiner-sourced VMS$VAXcluster CONNECT_REQs")
check(E["lookup_length_hist"] == {94: 12},
      "the lookup census was not length-restricted a priori (vms-c11): it "
      "reports the length histogram it found")

# The credit reading has to be attributed to sec 4(d), not re-invented here.
check("sec 4(d)" in dir_txt,
      "scs_dir.c attributes [48:50] to the sec 4(d) credit grounding")
# No template body may still label [48:50] a "flag" -- that is the refuted
# reading, and leaving it on one of the five templates is how it comes back.
for body in dir_txt.split("static const uint8_t")[1:]:
    tmpl = body.split("};")[0]
    for ln in tmpl.splitlines():
        if "[48:50]" in ln:
            check("flag" not in ln.lower(),
                  f"template line for [48:50] does not call it a flag: {ln.strip()}")

# ---------------------------------------------------------------------------
# (B) the refuted sentences may appear only inside a refutation
# ---------------------------------------------------------------------------
print("[B] refuted sentences stay dead")
REFUTED_PHRASES = [
    "runs in ONE direction",
    "only answers",
    "having polled anybody",
    "a request; the response has 1",
]
QUOTE_BEGIN = "REFUTED-QUOTE-BEGIN"
QUOTE_END = "REFUTED-QUOTE-END"


def quote_spans(lines):
    """[(begin_line, end_line)] of every REFUTED-QUOTE block, 0-based, markers
    EXCLUDED. An unterminated BEGIN yields no span, so leaving one open to
    swallow the rest of a file buys no exemption."""
    spans, open_at = [], None
    for i, line in enumerate(lines):
        if QUOTE_BEGIN in line:
            open_at = i
        elif QUOTE_END in line and open_at is not None:
            spans.append((open_at, i))
            open_at = None
    return spans


def normalize(lines):
    """Whitespace-normalized text plus a char->line map.

    Line-by-line matching would MISS a wrapped occurrence, and every one of
    these sentences is prose that wraps: the spec's 'without having polled /
    anybody' straddles two lines. Comment leaders are stripped too so a phrase
    re-asserted across ' * ' continuation lines is still seen."""
    buf, owner = [], []
    for i, raw in enumerate(lines):
        s = raw.strip()
        for lead in ("* ", "*", "// ", "//", "# "):
            if s.startswith(lead):
                s = s[len(lead):]
                break
        s = s.strip()
        if not s:
            continue
        buf.append(s)
        owner.extend([i] * (len(s) + 1))
    return " ".join(buf) + " ", owner


for path in SCANNED:
    if not os.path.exists(path):
        continue
    lines = read(path).splitlines()
    norm, owner = normalize(lines)
    spans = quote_spans(lines)
    for phrase in REFUTED_PHRASES:
        start = 0
        while True:
            at = norm.find(phrase, start)
            if at < 0:
                break
            start = at + 1
            i = owner[at] if at < len(owner) else 0
            ok = any(b < i < e for b, e in spans)
            check(ok,
                  f"{os.path.relpath(path, ROOT)}:{i + 1} '{phrase}' is inside a "
                  f"{QUOTE_BEGIN}/{QUOTE_END} block")

# The refutation must actually exist -- a gate that passes because someone
# deleted the correction entirely is worthless. All three carriers must still
# hold a quoted refutation AND cite the tool that re-derives it.
for label, path, txt in (("the spec", SPEC, spec_txt),
                         ("scsd.c", SCSD_C, scsd_txt),
                         ("scs_dir.c", DIR_C, dir_txt)):
    check(len(quote_spans(txt.splitlines())) >= 1,
          f"{label} still carries a {QUOTE_BEGIN} block (the correction was not deleted)")
    check("scs_dir_role_measure.py" in txt,
          f"{label} cites the tool that re-derives the correction")
check(os.path.exists(MEASURE), "the measuring tool is checked in")

print(f"\ntest_scs_dir_figures: {checks} checks, {len(failures)} failure(s)")
sys.exit(1 if failures else 0)
