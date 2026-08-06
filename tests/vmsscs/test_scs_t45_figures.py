#!/usr/bin/env python3
"""
test_scs_t45_figures.py -- vms-754: the MTYPE 4/5 decode must stay pinned to
the packets, and the refuted "MSCP connect-ACCEPT/CONFIRM" reading must stay
dead.

WHAT WAS IN DISPUTE. docs/cluster-protocol-spec.md sec 4(h)(1a)/(1b) reads
content[46:48] == 4/5 as REJECT_REQ/REJECT_RSP, grounded on a decisive
behavioural partition (a REJECT_REQ dialogue never gets an ACCEPT_REQ, and
vice versa). src/vmsscs/scs_dir.c's SCS_DIR_OP_ACCEPT / SCS_DIR_OP_MSCP_CONFIRM
constants read the SAME field, at the SAME offset, as an MSCP connect-ACCEPT /
CONFIRM (vms-760, "336 op-5 frames, 4 sender nodes, 15 captures"). Both
readings shared one namespace (src/vmsscs/include/scs_env.h) and disagreed
about it -- src/vmsscs/scs_dir.c's dir_build_common() said so out loud:
"Both readings cannot be right. Nothing here picks one."

WHAT THIS GATE HOLDS. tools/cluster/scs_t45_measure.py re-derives the decisive
test: an ACCEPT that binds a working connection must be followed, on that SAME
Con.ID pair, by application traffic (MTYPE 10); a REJECT cannot be, because
there is no connection left. Over the 47-capture lab-1 library, 733/733 type-4
dialogues are terminal (0 ever carry follow-up traffic) against 388/394 for
the undisputed ACCEPT_REQ positive control -- and the EXACT frame vms-760
cited as its own grounding evidence turns out to be a real-VAX-to-real-VAX
exchange (no OVMX participant in that capture at all) that is one of nine
identical rejections of a retried connect, immediately followed by a tenth
attempt that switches message type and succeeds. The spec's reading is
correct; scs_dir.c's is not.

This test:

  (1) PINS every figure tools/cluster/scs_t45_measure.py measured to
      src/vmsscs/include/scs_env.h, src/vmsscs/include/scs_dir.h and
      docs/cluster-protocol-spec.md. A digit that drifts in any of them reds.

  (2) KEEPS THE REFUTED READING DEAD. "vms-760 grounded op 4 as an MSCP
      connect-ACCEPT and op 5 as its CONFIRM" and its restatement in
      src/vmsscs/include/scs_dir.h's SCS_DIR_OP_ACCEPT / SCS_DIR_OP_MSCP_CONFIRM
      doc comments may appear ONLY inside a REFUTED-QUOTE-BEGIN/END pair (the
      same delimiter vms-6b3/vms-591 use for exactly this), anywhere in
      src/vmsscs/scs_dir.c, src/vmsscs/include/scs_dir.h or
      docs/cluster-protocol-spec.md.

  (3) REQUIRES THE FOLLOW-UP BUG TO BE ON THE RECORD. Resolving the decode
      does NOT fix src/vmsscs/scsd.c's server-first MSCP accept path, which
      still emits and consumes these bytes believing they mean ACCEPT/CONFIRM
      -- i.e. still calls a genuine REJECT_REQ/REJECT_RSP exchange a bound
      connection. Fixing that is out of scope for vms-754 (a wire-behaviour
      change needs its own item), but scs_dir.c must say so, not go silent.

AND SINCE vms-371, IT READS THE WIRE WHEN THE WIRE IS HERE (see
tests/vmsscs/scs_wire.py): on a host with the lab captures this gate calls
scs_t45_measure.rederive() and reds on any figure the packets no longer
support; on a host without them it announces loudly that the wire was not
read and still runs every prose-only check.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SPEC = os.environ.get("OVMX_SCS_T45_SPEC",
                      os.path.join(ROOT, "docs/cluster-protocol-spec.md"))
ENV_H = os.environ.get("OVMX_SCS_T45_ENV_H",
                       os.path.join(ROOT, "src/vmsscs/include/scs_env.h"))
DIR_H = os.environ.get("OVMX_SCS_T45_DIR_H",
                       os.path.join(ROOT, "src/vmsscs/include/scs_dir.h"))
DIR_C = os.environ.get("OVMX_SCS_T45_DIR_C",
                       os.path.join(ROOT, "src/vmsscs/scs_dir.c"))
SCSD_C = os.environ.get("OVMX_SCS_T45_SCSD_C",
                        os.path.join(ROOT, "src/vmsscs/scsd.c"))
MEASURE = os.environ.get("OVMX_SCS_T45_MEASURE",
                         os.path.join(ROOT, "tools/cluster/scs_t45_measure.py"))

failures = 0
checks = 0


def check(cond, msg):
    global failures, checks
    checks += 1
    if not cond:
        failures += 1
        print(f"  FAIL {msg}")


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def load_measure():
    d = os.path.dirname(MEASURE)
    if d not in sys.path:
        sys.path.insert(0, d)
    return scs_wire.load_source(MEASURE, "scs_t45_measure")


MEASURE_MOD = load_measure()
EXPECTED = MEASURE_MOD.EXPECTED

spec = read(SPEC)
env_h = read(ENV_H)
dir_h = read(DIR_H)
dir_c = read(DIR_C)
scsd_c = read(SCSD_C)

print("test_scs_t45_figures: vms-754 MTYPE 4/5 decode vs the prose")


def num_in(n, text):
    """Word-boundary digit match (vms-c66/vms-d04): a bare substring search
    for a 2-3 digit figure would also match inside an unrelated larger
    number anywhere in a multi-thousand-line spec."""
    return re.search(r"(?<!\d)%d(?!\d)" % n, text) is not None


# ===========================================================================
# 1. THE TERMINAL-DIALOGUE CENSUS -- the decisive figures, pinned in scs_env.h
# ===========================================================================
t2 = EXPECTED["terminal_census"][2]
t4 = EXPECTED["terminal_census"][4]

check(num_in(t4["frames"], env_h) or num_in(t4["frames"], dir_h),
      f"neither scs_env.h nor scs_dir.h states the type-4 population "
      f"({t4['frames']} frames)")
check(num_in(t4["terminal"], env_h) or num_in(t4["terminal"], dir_h),
      f"neither scs_env.h nor scs_dir.h states that ALL {t4['terminal']} "
      f"type-4 dialogues are terminal (0 ever carry follow-up MTYPE-10 "
      f"traffic)")
check(t4["followed"] == 0,
      "EXPECTED itself no longer records zero type-4 dialogues followed by "
      "application traffic -- the whole decode rests on this being zero")
check(num_in(t2["frames"], env_h) or num_in(t2["frames"], dir_h),
      f"neither scs_env.h nor scs_dir.h states the ACCEPT_REQ positive-control "
      f"population ({t2['frames']} frames)")
check(num_in(t2["followed"], env_h) or num_in(t2["followed"], dir_h),
      f"neither scs_env.h nor scs_dir.h states the positive-control followed "
      f"count ({t2['followed']} of {t2['frames']})")

check(num_in(EXPECTED["t4_vms_origin_frames"], spec),
      f"the spec no longer states the VMS-origin type-4 population "
      f"({EXPECTED['t4_vms_origin_frames']}) -- this is the figure that "
      f"cross-checks against scs_reason_measure.py's independently-measured "
      f"REJECT_REQ count over the SAME 47-pcap library")

# ===========================================================================
# 2. THE af2 EXHIBIT -- pinned wherever scs_dir.c cites its own grounding
# ===========================================================================
g = EXPECTED["af2_grounding_frame"]
check(str(g["idx"]) in dir_c or str(g["idx"]) in spec,
      f"neither scs_dir.c nor the spec cites frame {g['idx']}, the exact "
      f"frame scs_dir.c's op-4 builder claims as its byte-exact template")
check(("143.758" in dir_c) or ("143.758" in spec),
      "the 'rel~143.758' citation scs_dir.c's op-4 builder used to ground "
      "itself is gone -- vms-754 re-identifies this exact frame, so the "
      "citation must survive for a reader to find it")

thread = EXPECTED["af2_retry_thread"]
NINE_WORDS = {9: "nine"}
nine_word = NINE_WORDS.get(thread["rejected_attempts"])
check((num_in(thread["rejected_attempts"], dir_c) or
       num_in(thread["rejected_attempts"], spec) or
       (nine_word and (nine_word in dir_c.lower() or nine_word in spec.lower()))),
      f"neither scs_dir.c nor the spec records the {thread['rejected_attempts']}"
      f"-attempts-rejected-before-one-succeeds thread found in the SAME "
      f"af2 capture vms-760 cited")

# ===========================================================================
# 3. THE REFUTED READING STAYS DEAD (REFUTED-QUOTE quarantine, vms-6b3 style)
# ===========================================================================
QBEGIN = "REFUTED-QUOTE-BEGIN"
QEND = "REFUTED-QUOTE-END"
MAX_QUARANTINE_CHARS = 1200


def quarantine_spans(text, complain=True):
    toks = sorted([(m.start(), QBEGIN) for m in re.finditer(re.escape(QBEGIN), text)] +
                  [(m.start(), QEND) for m in re.finditer(re.escape(QEND), text)])
    spans = []
    open_at = None
    for pos, kind in toks:
        if kind == QBEGIN:
            if open_at is not None and complain:
                check(False, f"a REFUTED-QUOTE-BEGIN at offset {pos} opens inside "
                             f"a block already open at {open_at}")
            if open_at is None:
                open_at = pos
        else:
            if open_at is None:
                if complain:
                    check(False, f"a REFUTED-QUOTE-END at offset {pos} with no "
                                 f"open block")
                continue
            end = pos + len(QEND)
            if complain:
                check(end - open_at <= MAX_QUARANTINE_CHARS,
                      f"a REFUTED-QUOTE block is {end - open_at} characters "
                      f"long (cap {MAX_QUARANTINE_CHARS})")
            spans.append((open_at, end))
            open_at = None
    if open_at is not None and complain:
        check(False, "an unbalanced REFUTED-QUOTE-BEGIN with no matching END")
    return spans


def outside_quarantine(text):
    spans = quarantine_spans(text, complain=False)
    out, prev = [], 0
    for b, e in spans:
        out.append(text[prev:b])
        prev = e
    out.append(text[prev:])
    return "".join(out)


for doc_name, doc in (("scs_dir.c", dir_c), ("scs_dir.h", dir_h), ("spec", spec)):
    quarantine_spans(doc, complain=True)

# The dead claim, as a family: op 4 (or "SCS_DIR_OP_ACCEPT") described as an
# MSCP accept, or op 5 (or "SCS_DIR_OP_MSCP_CONFIRM") described as its
# confirm, WITHOUT a refusal/correction marker in the same sentence.
SUBJECT = (r"(?:op[\s=-]*4|SCS_DIR_OP_ACCEPT|op[\s=-]*5|"
           r"SCS_DIR_OP_MSCP_CONFIRM)")
CLAIM = (r"(?:MSCP\s+connect-ACCEPT|MSCP\s+CONNECT-ACCEPT|MSCP\s+connect-CONFIRM|"
         r"\bACCEPT4\b|\bCONFIRM5\b|BINDS?\s+(?:OVMX'?s?\s+)?MSCP)")
RESCUE = (r"(?:\bnot\b|\bno\b|\bnever\b|\bneither\b|refut|eliminat|weaken|"
          r"supersed|overturn|dead|misidentif|do\s+not|cannot|originally|"
          r"used\s+to|earlier|previously|actually|really|vms-754|"
          r"REJECT_REQ|REJECT_RSP)")


def sentences(doc):
    doc = re.sub(r"\b(pp?|sec|fig|Fig|no|vs|cf|e\.g|i\.e)\.\s+(?=[\dA-Za-z])",
                 lambda m: m.group(1) + ".", doc)
    return re.split(r"(?<=[.!?])\s+|\n\n", doc)


DEAD = []
for doc_name, doc in (("scs_dir.c", dir_c), ("scs_dir.h", dir_h), ("spec", spec)):
    body = outside_quarantine(doc)
    for sentence in sentences(body):
        flat = " ".join(sentence.split())
        if not re.search(SUBJECT, flat, re.I):
            continue
        if not re.search(CLAIM, flat, re.I):
            continue
        if re.search(RESCUE, flat, re.I):
            continue
        DEAD.append((doc_name, flat[:200]))
check(not DEAD,
      "the REFUTED reading of message type 4/5 (MSCP connect-ACCEPT/CONFIRM) "
      "is asserted as fact, unqualified, outside a REFUTED-QUOTE block:\n" +
      "\n".join(f"      {d}: {s}" for d, s in DEAD))

# The resolution itself must be on the record, not just the absence of the
# old claim.
check("vms-754" in dir_c, "scs_dir.c no longer cites vms-754 for the resolution")
check("vms-754" in env_h or "vms-754" in dir_h,
      "neither scs_env.h nor scs_dir.h cites vms-754 for the resolution")
check(re.search(r"REJECT_REQ", dir_c) is not None,
      "scs_dir.c no longer states that op 4/5 are actually REJECT_REQ/REJECT_RSP")

# ===========================================================================
# 4. THE FOLLOW-UP BUG STAYS ON THE RECORD (not fixed here -- vms-754 scope)
# ===========================================================================
check(re.search(r"vms-abd|follow-?up|separate item|out of scope", dir_c, re.I)
      is not None or re.search(r"vms-abd|follow-?up|separate item|out of scope",
                                scsd_c, re.I) is not None,
      "neither scs_dir.c nor scsd.c records that the server-first MSCP accept "
      "path still emits/consumes these bytes as if they meant ACCEPT/CONFIRM "
      "-- a genuine wire-behaviour question this item deliberately did not fix")

# ===========================================================================
# 5. THE LAB FENCE (vms-096)
# ===========================================================================
check(hasattr(MEASURE_MOD, "lab1_only"),
      "scs_t45_measure.py has lost its lab1_only() fence")
if hasattr(MEASURE_MOD, "lab1_only"):
    clean = ["/x/cd0-baseline-current-20260728.pcap", "/x/formation-01.pcap"]
    check(MEASURE_MOD.lab1_only(list(clean)) == clean,
          "lab1_only() rejected a clean lab-1 capture list")
    try:
        MEASURE_MOD.lab1_only(clean + ["/x/vms578-B1-lab2-vaxlab4-20260805.pcap"])
        check(False, "lab1_only() accepted a lab-2 capture in the lab-1 library")
    except SystemExit as exc:
        check("vms578-B1-lab2-vaxlab4-20260805.pcap" in str(exc),
              "lab1_only() refused the mixed list without naming the offender")
src = read(MEASURE)
check("lab1_only(sorted(glob.glob(" in src,
      "scs_t45_measure.py defines lab1_only() but does not wrap its glob with it")

# ===========================================================================
# 6. THE WIRE ITSELF (vms-371)
# ===========================================================================
scs_wire.gate("scs_t45_figures", MEASURE_MOD, MEASURE_MOD.DEFAULT_CAPDIR, check)

print(f"{'FAIL' if failures else 'PASS'}: {checks} checks, {failures} failure(s)")
sys.exit(1 if failures else 0)
