#!/usr/bin/env python3
"""
test_scs_reason_mutants.py -- the MUTATION BATTERY for scs_reason_figures.

vms-6b3, review round 3. The round-2 artifact carried a mutation-coverage claim
in tests/vmsscs/CMakeLists.txt -- "26 of 26 mutants (drifted digits in either
document, deleted rows, and both refuted sentences reinstated) red it" -- and
the clause that mattered was FALSE. Re-asserting "Nothing after the Con.ID pair
varies." in three different natural sites left scs_reason_figures GREEN, because
its refuted-claim check was a proximity window over documents that are entirely
ABOUT the refutation, so the excuse words were everywhere. A comment claiming
coverage nobody had measured is worse than no gate: it stops the next reader
from checking.

So the number stops being a comment. This test IS the claim. It:

  1. copies the three inputs (scs_reason.h, cluster-protocol-spec.md,
     tools/cluster/scs_reason_measure.py) into a scratch tree,
  2. asserts the UNMUTATED copy is GREEN -- the control, without which every
     kill below could be an artifact of the copy,
  3. applies each mutant to the scratch copy, one at a time, runs
     test_scs_reason_figures.py against it via the OVMX_SCS_REASON_* path
     overrides, and requires a NON-ZERO exit,
  4. restores the scratch copy from the pristine bytes and re-verifies the
     control after EVERY mutant, so a mutant that fails to apply, or one that
     leaves the tree wedged, cannot be scored as a kill.

Every mutant must also be VERIFIED TO HAVE CHANGED THE FILE (the edit is a
literal replace and the battery asserts the text moved). A no-op edit scored as
a kill is precisely the defect class this item was rejected for twice.

The count is printed and re-derived on every ctest run. If you add a mutant,
the number in tests/vmsscs/CMakeLists.txt is a POINTER TO THIS TEST, not a
second copy of the figure.

ROUND 4 added the DATABLOCK-* group. The round-3 gate flattened the documents
with re.sub(r"\\s+", " ", ...), so a CENSUS table or a REFUTATION-FACT block --
neither of which ends in a full stop -- merged with the paragraph below it, and
a rescue token inside the DATA excused a dead claim written in that prose.
Sweeping the claim into every paragraph-leading site of both documents left
exactly four survivors, all of them the first paragraph after a data block.
Those four are now mutants. They are the regression pin for flatten(): checked
against the round-3 gate they all SURVIVE, against the current one they all
die, which is what makes them a measurement rather than four more green rows.
"""

import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GATE = os.path.join(HERE, "test_scs_reason_figures.py")

SRC = {
    "header": os.path.join(ROOT, "src", "vmsscs", "include", "scs_reason.h"),
    "spec": os.path.join(ROOT, "docs", "cluster-protocol-spec.md"),
    "measure": os.path.join(ROOT, "tools", "cluster", "scs_reason_measure.py"),
}

# ---------------------------------------------------------------------------
# The mutants. (id, which-file, old-text, new-text[, replace-count=1]).
# replace-count -1 means every occurrence.
#
# Grouped by the defect class each one stands for. The three REASSERT-* entries
# are the adversary's round-3 mutants reproduced verbatim, at the exact sites
# where the round-2 gate let them through.
# ---------------------------------------------------------------------------

REASSERT = "Nothing after the Con.ID pair varies."

MUTANTS = [
    # --- A. drifted digits in scs_reason.h CENSUS lines ---------------------
    ("hdr-censusA-frames", "header",
     "CENSUS-A type=6 name=DISCONNECT_REQ frames=220 pcaps=25",
     "CENSUS-A type=6 name=DISCONNECT_REQ frames=221 pcaps=25"),
    ("hdr-censusA-pcaps", "header",
     "CENSUS-A type=4 name=REJECT_REQ     frames=453 pcaps=19",
     "CENSUS-A type=4 name=REJECT_REQ     frames=453 pcaps=20"),
    ("hdr-censusA-off60-split", "header",
     "CENSUS-A type=6 off=60 values=0x0000:131,0x0001:89",
     "CENSUS-A type=6 off=60 values=0x0000:132,0x0001:88"),
    ("hdr-censusA-off58-nonzero", "header",
     "CENSUS-A type=4 off=58 values=0x0000:453",
     "CENSUS-A type=4 off=58 values=0x0000:452,0x0001:1"),
    ("hdr-censusB-acceptrsp", "header",
     "CENSUS-B len=62  type=3  name=ACCEPT_RSP      frames=258  pcaps=33 nonzero58=62",
     "CENSUS-B len=62  type=3  name=ACCEPT_RSP      frames=258  pcaps=33 nonzero58=63"),
    ("hdr-censusB-row-deleted", "header",
     "CENSUS-B len=110 type=2  name=ACCEPT_REQ      frames=324  pcaps=25 nonzero58=101\n",
     ""),
    ("hdr-censusC-slots", "header",
     "CENSUS-C type=4 zero_slots=28,32,36,48,58",
     "CENSUS-C type=4 zero_slots=28,32,36,58"),
    ("hdr-censusC-after", "header",
     "CENSUS-C common_at_or_after_payload=50 zero_slots=58",
     "CENSUS-C common_at_or_after_payload=50 zero_slots=48,58"),
    ("hdr-censusP-pcaps", "header",
     "CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=47",
     "CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=48"),
    ("hdr-censusP-classes", "header",
     "CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=47",
     "CENSUS-P sca_len_classes=62,66,106,110 pcaps_scanned=47"),
    ("hdr-censusD-cdts", "header",
     "CENSUS-D sda_file=sda-scs-extract-vax1.txt cdts=12 values=0:12",
     "CENSUS-D sda_file=sda-scs-extract-vax1.txt cdts=13 values=0:13"),
    ("hdr-censusD-file", "header",
     "CENSUS-D sda_file=sda-scs-extract-vax1.txt",
     "CENSUS-D sda_file=sda-scs-extract-vax2.txt"),

    # --- B. drifted digits / deleted rows in the spec tables ----------------
    ("spec-censusA-frames", "spec",
     "| `4` REJECT_REQ | 453 | 19 |",
     "| `4` REJECT_REQ | 454 | 19 |"),
    ("spec-censusA-off60-collapsed", "spec",
     "| `0x0000` × 131, `0x0001` × 89 |",
     "| `0x0000` × 220 |"),
    ("spec-censusA-off58", "spec",
     "| `6` DISCONNECT_REQ | 220 | 25 | `0x0000` × 220 |",
     "| `6` DISCONNECT_REQ | 220 | 25 | `0x0000` × 219 |"),
    ("spec-censusB-acceptrsp", "spec",
     "| 62 | `3` | ACCEPT_RSP | 258 | 33 | 62 |",
     "| 62 | `3` | ACCEPT_RSP | 258 | 33 | 0 |"),
    ("spec-censusB-row-deleted", "spec",
     "| 110 | `0` | CONNECT_REQ | 1101 | 35 | 809 |\n",
     ""),
    ("spec-censusC-slots", "spec",
     "| `6` | 28, 32, 36, 48, 58 |",
     "| `6` | 28, 32, 36, 48 |"),
    ("spec-censusC-after-row", "spec",
     "| both, at or after payload 50 | 58 |",
     "| both, at or after payload 50 | 48, 58 |"),
    ("spec-censusP", "spec",
     "CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=47",
     "CENSUS-P sca_len_classes=62,66,110 pcaps_scanned=46"),
    ("spec-censusD", "spec",
     "CENSUS-D sda_file=sda-scs-extract-vax1.txt cdts=12 values=0:12",
     "CENSUS-D sda_file=sda-scs-extract-vax1.txt cdts=12 values=0:11"),
    ("spec-acceptrsp-unnamed", "spec",
     "| 62 | `3` | ACCEPT_RSP | 258 | 33 | 62 |",
     "| 62 | `3` | TYPE_THREE | 258 | 33 | 62 |"),

    # --- C. drifted EXPECTED in the measure script --------------------------
    ("exp-carrier-frames", "measure",
     '6: {"name": "DISCONNECT_REQ", "frames": 220, "pcaps": 25,',
     '6: {"name": "DISCONNECT_REQ", "frames": 219, "pcaps": 25,'),
    ("exp-carrier-pcaps", "measure",
     '4: {"name": "REJECT_REQ", "frames": 453, "pcaps": 19,',
     '4: {"name": "REJECT_REQ", "frames": 453, "pcaps": 26,'),
    ("exp-off60-collapsed", "measure",
     '"off60": {0x0000: 131, 0x0001: 89}},',
     '"off60": {0x0000: 220}},'),
    ("exp-neighbour-nonzero", "measure",
     '(62, 3): {"name": "ACCEPT_RSP", "frames": 258, "pcaps": 33, "nonzero58": 62},',
     '(62, 3): {"name": "ACCEPT_RSP", "frames": 258, "pcaps": 33, "nonzero58": 0},'),
    ("exp-zero-slots", "measure",
     '"zero_slots": {4: (28, 32, 36, 48, 58), 6: (28, 32, 36, 48, 58)},',
     '"zero_slots": {4: (28, 32, 36, 48, 58), 6: (28, 32, 36, 58)},'),
    ("exp-after-counters", "measure",
     '"zero_slots_after_counters": (58,),',
     '"zero_slots_after_counters": (48, 58),'),
    ("exp-n-captures", "measure",
     '"n_captures": 47,',
     '"n_captures": 46,'),
    ("exp-carrier-off58-nonzero", "measure",
     '"off58": {0x0000: 220},',
     '"off58": {0x0000: 219, 0x0001: 1},'),
    ("exp-conid-payload-end", "measure",
     "CONID_PAYLOAD_END = 58",
     "CONID_PAYLOAD_END = 60"),

    # --- D. THE ROUND-3 DEFECT: refuted claims walking back in --------------
    #      Sites (a), (b) and (c) are the adversary's; (d) is the fourth site
    #      this round found in the header. All four were GREEN before the fix.
    ("REASSERT-a-spec-sec4h1a", "spec",
     "It is a live field with an undecoded meaning, not a\nreason code we can read.",
     "It is a live field with an undecoded meaning, not a\nreason code we can read. " + REASSERT),
    ("REASSERT-b-spec-disconnect-para", "spec",
     "**`6` = DISCONNECT — plausible, NOT grounded.** All 6 occurrences",
     "**`6` = DISCONNECT — plausible, NOT grounded.** " + REASSERT +
     " All 6 occurrences"),
    ("REASSERT-c-spec-sec5-refutation-para", "spec",
     "Census B applies the SAME population rule to the whole",
     REASSERT + " Census B applies the SAME population rule to the whole"),
    ("REASSERT-d-header-census-para", "header",
     " *   THE SECOND ORACLE AGREES.",
     " *   " + REASSERT + "\n *\n *   THE SECOND ORACLE AGREES."),

    # --- D2. THE ROUND-4 DEFECT: a DATA BLOCK excusing the prose after it ----
    #      Round 3's flatten was re.sub(r"\s+", " ", text). CENSUS table rows
    #      and REFUTATION-FACT lines carry no sentence-ending punctuation, so
    #      each data block merged with the paragraph BELOW it into one
    #      "sentence" -- and a rescue token inside the DATA then excused a dead
    #      claim re-asserted in that paragraph.
    #
    #      These are the FOUR sites that survived the round-4 sweep of every
    #      paragraph-leading site in both documents (246/248 spec, 46/48
    #      header). Each is the first prose paragraph after a data block:
    #        a) sec 4(h)(1a), after the CENSUS-A table   -- rescued by 0x0000 x 131
    #        b) sec 5, after the REFUTATION-FACT lines   -- rescued by 0x0000:131
    #        c) scs_reason.h, after the CENSUS-A lines   -- rescued by 0x0000:131
    #        d) scs_reason.h, after REFUTATION-FACT, immediately ABOVE (not
    #           inside) the quarantine block -- the cheapest of the four.
    #      They stay here permanently so the merge cannot come back.
    ("DATABLOCK-a-spec-after-censusA-table", "spec",
     "So `[58:60]` never carries a nonzero value in either frame",
     REASSERT + " So `[58:60]` never carries a nonzero value in either frame"),
    ("DATABLOCK-b-spec-after-refutation-facts", "spec",
     "**The SDA oracle agrees.** `SHOW CONNECTIONS` prints a per-CDT field literally",
     REASSERT + " **The SDA oracle agrees.** `SHOW CONNECTIONS` prints a "
     "per-CDT field literally"),
    ("DATABLOCK-c-header-after-censusA-lines", "header",
     " *   Read off those six lines: payload [58:60] is 0x0000 in EVERY frame of BOTH",
     " *   " + REASSERT + "\n"
     " *   Read off those six lines: payload [58:60] is 0x0000 in EVERY frame of BOTH"),
    ("DATABLOCK-d-header-after-refutation-facts", "header",
     " *   REFUTED-QUOTE-BEGIN\n"
     " *   (An earlier revision of the spec said",
     " *   " + REASSERT + "\n"
     " *   REFUTED-QUOTE-BEGIN\n"
     " *   (An earlier revision of the spec said"),

    # --- E. the same claims REWORDED, which a literal-string gate misses ----
    ("REWORD-postconid-does-not-vary", "spec",
     "**`6` = DISCONNECT — plausible, NOT grounded.** All 6 occurrences",
     "**`6` = DISCONNECT — plausible, NOT grounded.** The two words following "
     "the Con.ID pair do not vary. All 6 occurrences"),
    ("REWORD-postconid-invariant", "header",
     " *   THE SECOND ORACLE AGREES.",
     " *   Payload [60:62] is invariant across the population.\n *\n"
     " *   THE SECOND ORACLE AGREES."),
    ("REWORD-only-always-zero", "spec",
     "**`6` = DISCONNECT — plausible, NOT grounded.** All 6 occurrences",
     "**`6` = DISCONNECT — plausible, NOT grounded.** `[58:60]` is the only "
     "always-zero slot in the frame. All 6 occurrences"),
    ("REWORD-only-varying-seq-ack", "header",
     " *   THE SECOND ORACLE AGREES.",
     " *   The only varying bytes are the sequence/ack fields and the Con.ID\n"
     " *   pair.\n *\n *   THE SECOND ORACLE AGREES."),
    ("REWORD-only-slot-zero-100pct", "spec",
     "**`6` = DISCONNECT — plausible, NOT grounded.** All 6 occurrences",
     "**`6` = DISCONNECT — plausible, NOT grounded.** It is the only 16-bit "
     "slot in either frame that is zero in 100% of observed VMS frames. "
     "All 6 occurrences"),

    # --- F. attacks on the quarantine itself --------------------------------
    ("QUARANTINE-unbalanced", "spec",
     "<!-- REFUTED-QUOTE-END --> Census B applies",
     "Census B applies"),
    ("QUARANTINE-oversized", "spec",
     "<!-- REFUTED-QUOTE-END --> Census B applies the SAME population rule to the whole\n"
     "  connection-control envelope (every message type) and finds `[58:60]` in live\n"
     "  use by neighbouring types — including `3` = ACCEPT_RSP, which shares the\n"
     "  **identical 62-byte layout** with `4` and `6`:",
     "Census B applies the SAME population rule to the whole\n"
     "  connection-control envelope (every message type) and finds `[58:60]` in live\n"
     "  use by neighbouring types — including `3` = ACCEPT_RSP, which shares the\n"
     "  **identical 62-byte layout** with `4` and `6`: <!-- REFUTED-QUOTE-END -->"),
    # The quarantine must not become a parking space: strip the refutation
    # label from inside the block while leaving the dead claim in it.
    ("QUARANTINE-no-refutation-label", "header",
     " *   The FIRST revision of this file justified the slot as \"the only 16-bit\n"
     " *   slot in either frame that is zero in 100% of observed VMS frames\". THAT\n"
     " *   CLAIM IS FALSE IN BOTH HALVES,",
     " *   The slot is \"the only 16-bit\n"
     " *   slot in either frame that is zero in 100% of observed VMS frames\". THAT\n"
     " *   IS THE WHOLE STORY,"),
    # The cheapest evasion: park a two-word labelled block next to the dead
    # claim so the sentence "overlaps" quarantine. The claim's own extent must
    # be inside the block, not merely near it -- otherwise this is the round-2
    # proximity window with extra steps.
    ("QUARANTINE-tiny-inline-licence", "spec",
     "**`6` = DISCONNECT — plausible, NOT grounded.** All 6 occurrences",
     "**`6` = DISCONNECT — plausible, NOT grounded.** " + REASSERT +
     " <!-- REFUTED-QUOTE-BEGIN --> (refuted) <!-- REFUTED-QUOTE-END -->"
     " All 6 occurrences"),
    # Drop the quarantine entirely and leave the quotation behind: the dead
    # claim is then simply asserted in the document.
    ("QUARANTINE-markers-removed-spec", "spec",
     "<!-- REFUTED-QUOTE-BEGIN --> Revision 1 of this entry", "Revision 1 of this entry"),
    ("QUARANTINE-markers-removed-hdr", "header",
     " *   REFUTED-QUOTE-BEGIN\n *   The FIRST revision of this file",
     " *   The FIRST revision of this file"),
    ("QUARANTINE-wraps-a-census-line", "header",
     " *     CENSUS-C common_at_or_after_payload=50 zero_slots=58",
     " *   REFUTED-QUOTE-BEGIN  (refuted, quoting to kill it)\n"
     " *     CENSUS-C common_at_or_after_payload=50 zero_slots=58\n"
     " *   REFUTED-QUOTE-END"),

    # --- G. attacks on the pinned POSITIVE facts ----------------------------
    ("FACT-hdr-deleted", "header",
     " *     REFUTATION-FACT off=60 type=6 distinct_values=2 values=0x0000:131,0x0001:89\n",
     ""),
    ("FACT-spec-deleted", "spec",
     "      REFUTATION-FACT off=58 len=62 type=3 name=ACCEPT_RSP nonzero=62\n",
     ""),
    ("FACT-hdr-collapsed-to-one-value", "header",
     "REFUTATION-FACT off=60 type=6 distinct_values=2 values=0x0000:131,0x0001:89",
     "REFUTATION-FACT off=60 type=6 distinct_values=1 values=0x0000:220"),
    ("FACT-spec-neighbour-zeroed", "spec",
     "REFUTATION-FACT off=58 len=62 type=3 name=ACCEPT_RSP nonzero=62",
     "REFUTATION-FACT off=58 len=62 type=3 name=ACCEPT_RSP nonzero=0"),
    ("FACT-spec-names-a-carrier", "spec",
     "REFUTATION-FACT off=58 len=62 type=3 name=ACCEPT_RSP nonzero=62",
     "REFUTATION-FACT off=58 len=62 type=4 name=REJECT_REQ nonzero=62"),
    ("FACT-hdr-digits-drifted", "header",
     "REFUTATION-FACT off=60 type=6 distinct_values=2 values=0x0000:131,0x0001:89",
     "REFUTATION-FACT off=60 type=6 distinct_values=2 values=0x0000:130,0x0001:90"),

    # --- H. the structural claims the placement rationale rests on ----------
    ("STRUCT-payload-off-drift", "header",
     "#define SCS_REASON_PAYLOAD_OFF 58u",
     "#define SCS_REASON_PAYLOAD_OFF 60u"),
    ("STRUCT-hdr-label-removed", "header",
     "/* LABELED OVMX DESIGN CHOICE (see the header comment): payload-relative",
     "/* Decoded VMS field (see the header comment): payload-relative"),
    ("STRUCT-spec-label-removed", "spec",
     "(`src/vmsscs/include/scs_reason.h`) is a LABELED OVMX DESIGN CHOICE, not a",
     "(`src/vmsscs/include/scs_reason.h`) is a decoded VMS field, not a"),
    # These two need replace-ALL: the spec names ACCEPT_RSP and the header
    # names the script in several places, so removing ONE occurrence is not a
    # defect and must not be scored as a mutant. Round 3 wrote them as
    # single-occurrence edits, they survived, and that is exactly right --
    # the gate's claim is that the fact is stated SOMEWHERE, not everywhere.
    ("STRUCT-spec-acceptrsp-erased-everywhere", "spec",
     "ACCEPT_RSP", "TYPE_THREE", -1),
    ("STRUCT-hdr-script-pointer-erased-everywhere", "header",
     "tools/cluster/scs_reason_measure.py", "the measurement script", -1),
]


def run_gate(work):
    env = dict(os.environ)
    env["OVMX_SCS_REASON_HEADER"] = work["header"]
    env["OVMX_SCS_REASON_SPEC"] = work["spec"]
    env["OVMX_SCS_REASON_MEASURE"] = work["measure"]
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    # vms-371: force the HOST-INDEPENDENT arm. This battery scores the gate's
    # PROSE-vs-EXPECTED pinning; the wire arm is scored by
    # test_scs_figures_wire_mutants.py, which mutates the packets themselves.
    # Pointing OVMX_LAB_CAPTURES at a path that cannot exist makes every mutant
    # here run against the same, deterministic half of the gate -- and keeps a
    # scratch copy of a measure script (which has no dissector beside it) from
    # reddening the CONTROL for a reason that has nothing to do with the mutant.
    env["OVMX_LAB_CAPTURES"] = os.path.join(os.path.dirname(work["measure"]), "no-such-capture-dir")
    env.pop("OVMX_SCS_REQUIRE_WIRE", None)
    p = subprocess.run([sys.executable, GATE], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main():
    ids = [m[0] for m in MUTANTS]
    dupes = sorted({i for i in ids if ids.count(i) > 1})
    if dupes:
        print("FAIL duplicate mutant ids: %r" % (dupes,))
        return 1

    tmp = tempfile.mkdtemp(prefix="scs_reason_mutants.")
    try:
        pristine = {k: open(v, "rb").read() for k, v in SRC.items()}
        # scs_reason_measure.py does sys.path.insert(0, dirname(__file__)) and
        # imports dissect_sca at module scope, so the scratch copy needs its
        # neighbour beside it. Copying it keeps the whole battery out of the
        # repo tree -- nothing under src/, docs/ or tools/ is ever written.
        shutil.copy2(os.path.join(os.path.dirname(SRC["measure"]),
                                  "dissect_sca.py"),
                     os.path.join(tmp, "dissect_sca.py"))
        work = {
            "header": os.path.join(tmp, "scs_reason.h"),
            "spec": os.path.join(tmp, "cluster-protocol-spec.md"),
            "measure": os.path.join(tmp, "scs_reason_measure.py"),
        }

        def restore():
            for k in work:
                with open(work[k], "wb") as f:
                    f.write(pristine[k])

        restore()

        rc, out = run_gate(work)
        if rc != 0:
            print("FAIL the CONTROL (unmutated scratch copy) is not green; "
                  "no kill below would mean anything.\n%s" % out)
            return 1
        print("control: unmutated scratch copy is GREEN -- %s" % out.strip())

        killed, survivors, unapplied = [], [], []
        for entry in MUTANTS:
            mid, which, old, new = entry[:4]
            count = entry[4] if len(entry) > 4 else 1
            restore()
            text = pristine[which].decode("utf-8")
            if old not in text:
                unapplied.append((mid, which))
                continue
            mutated = text.replace(old, new, count)
            if mutated == text:
                unapplied.append((mid, which))
                continue
            with open(work[which], "wb") as f:
                f.write(mutated.encode("utf-8"))
            rc, out = run_gate(work)
            if rc != 0:
                killed.append(mid)
            else:
                survivors.append((mid, out.strip()))
            # Re-verify the control after every mutant: a wedged scratch tree
            # would otherwise score the rest of the battery as kills.
            restore()
            rc2, out2 = run_gate(work)
            if rc2 != 0:
                print("FAIL control did not come back green after mutant %s -- "
                      "the battery is unsound from here.\n%s" % (mid, out2))
                return 1

        for mid, which in unapplied:
            print("FAIL mutant %s did not apply: its anchor text is not in the "
                  "%s. A mutant that does not change the file is not a mutant."
                  % (mid, which))
        for mid, out in survivors:
            print("FAIL mutant %s SURVIVED scs_reason_figures: %s" % (mid, out))

        print("%d mutants, %d killed, %d survived, %d failed to apply"
              % (len(MUTANTS), len(killed), len(survivors), len(unapplied)))
        return 1 if (survivors or unapplied) else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
