#!/usr/bin/env python3
"""
test_scs_env_mutants.py -- THE MUTATION BATTERY for scs_env_figures.

vms-3f4 (5th generation of this exact failure class in this file: vms-182 ->
vms-ab3 -> vms-c84 -> this). Every comparable figures gate in tests/vmsscs/
(t89, dir, disc, diskrun, reason, join_capability, credit_live, mscp_srv) has
a checked-in mutation battery except this one -- the ROOT CAUSE this item
closes. Without a checked-in battery, "the gate catches guessed names" was a
claim re-verified by hand each audit round and never became a measurement
that runs on every ctest invocation.

Method, the same discipline as test_scs_dir_mutants.py:

  1. copy src/vmsscs/ (the whole tree -- the gate's open-coded-offset check
     walks every .c file in it) and tools/cluster/scs_env_measure.py into a
     scratch tree that mirrors the repo layout,
  2. force the HOST-INDEPENDENT arm (OVMX_LAB_CAPTURES pointed at a path that
     cannot exist) so the battery scores the PROSE-vs-EXPECTED half
     deterministically, the same way test_scs_dir_mutants.py does -- the wire
     arm is scored by test_scs_figures_wire_mutants.py, which mutates the
     packets, not the prose,
  3. assert the UNMUTATED copy is GREEN (the control -- no kill below means
     anything without it),
  4. apply each mutant alone via OVMX_SCS_ENV_ROOT, run the gate, require a
     NON-ZERO exit,
  5. restore and RE-VERIFY THE CONTROL after every mutant, so a wedged tree
     cannot score the rest of the battery as kills,
  6. separately, BENIGN_MUTANTS must NOT red the gate -- a benign paragraph
     reflow is content-preserving and a gate that reds on it is over-fitted
     to line breaks, not content (vms-c84 M9).

A mutant whose anchor text is absent, or whose edit does not change the
file, is reported as a FAILURE and never scored as a kill.

WHAT EACH MUTANT COVERS (grouped by when it was found):

  M1-M4  (vms-ab3's original 3-mutant audit, split to one assertion each):
          guessed name in the T9 #define comment; the type-8 identification
          dropped from BOTH its prose and #define spots at once; the literal
          token UNIDENTIFIED reinstated; "unnamed" dropped from the T9 prose
          sentence.
  M5-M9  (vms-c84's 5-mutant audit): a guessed name in the T9 PROSE sentence
          (not just the comment); three single-carrier mutations (T8 prose
          only, T8 comment only, T9 comment only) that must still red now
          that the checks are prose-AND-comment conjunctions, not
          disjunctions; a benign paragraph reflow that must NOT red
          (BENIGN_MUTANTS, not this list).
  M10-M13 (vms-3f4 item 1): the four guessed-name SHAPES the original
          UPPER_SNAKE/PascalCase/camelCase-only pattern missed -- a
          VMS-style $-identifier, a bare ALL-CAPS word, a spaced Title Case
          phrase, and a kebab-case phrase.
  M14-M18 (vms-3f4 item 2): the carriers that were never scanned for a
          guessed name at all -- the pre-#define block comment, the
          MTYPE-namespace paragraph PREAMBLE, the T8 sentence, the T8
          #define comment, and scs_env_mtype_name()'s doc comment.
  M19-M21 (vms-3f4 item 3): scs_env_mtype_name()'s doc comment dropping its
          (now-fixed) identification of MTYPE 8; the doc comment's stale
          "NOT identified" claim reinstated in full; the hyphenated
          "UN-IDENTIFIED" spelling of the same stale claim.

The mutant count and the kill count are PRINTED by this script and
deliberately NOT restated in any comment or CMake entry (vms-6b3).
"""
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GATE = os.path.join(HERE, "test_scs_env_figures.py")
ENV_H_REL = os.path.join("src", "vmsscs", "include", "scs_env.h")

T9_SENT = ("9 is its paired response, one-per-8, but is DELIBERATELY left "
          "unnamed: no")
T8_SENT = ("8 is the p. 2-44 SPECIAL CREDIT MESSAGE, identified by\n"
          " * vms-f03/#128:")
T8_DEF = ("#define SCS_ENV_MTYPE_T8              8u  "
         "/* special credit message -- vms-f03/#128 */")
T9_DEF = ("#define SCS_ENV_MTYPE_T9              9u  "
         "/* paired response, deliberately UNNAMED -- vms-f03/#128 */")
PREDEF_SENT = ("8 is the special credit message and 9 is its unnamed\n"
              " * paired response (vms-f03/#128) -- see the MTYPE "
              "namespace comment above.")
PREAMBLE = ("Exactly {0..10} over ~1,000,000 envelope-conformant frames;")
MTYPENAME_DOC = (
    '8 is the special credit message, identified by\n'
    ' * vms-f03/#128, but still renders as "type 8" -- naming it while its '
    'paired\n'
    ' * response (9) stays a bare number would read as more confidence '
    'about 9\n'
    ' * than the record has. 9 renders as "type 9" because it is '
    'DELIBERATELY\n'
    ' * left unnamed: no public doc names a response to the special '
    'credit\n'
    ' * message, and guessing would repeat the vms-c11 guessed-name '
    'pattern.')

# (name, [(old, new), ...]). Every `old` must appear EXACTLY ONCE in the
# scratch scs_env.h. A mutant with more than one (old, new) pair applies all
# of them together, as one atomic edit.
MUTANTS = [
    # ---- M1-M4: vms-ab3's original audit ----------------------------------
    ("M1-guessed-name-in-T9-define-comment", [
        (T9_DEF, T9_DEF.replace("UNNAMED --", "UNNAMED (SpecialCreditRsp) --")),
    ]),
    ("M2-type8-identification-dropped-in-all-spots", [
        (T8_SENT, T8_SENT.replace("SPECIAL CREDIT MESSAGE", "UNKNOWN MESSAGE")),
        (T8_DEF, T8_DEF.replace("special credit message", "unknown message")),
    ]),
    ("M3-literal-UNIDENTIFIED-reinstated", [
        (T9_DEF, T9_DEF.replace("UNNAMED --", "UNNAMED (UNIDENTIFIED) --")),
    ]),
    ("M4-unnamed-dropped-from-T9-prose", [
        (T9_SENT, T9_SENT.replace("left unnamed:", "left as type 9:")),
    ]),

    # ---- M5-M9: vms-c84's audit (M9 is BENIGN_MUTANTS, below) -------------
    ("M5-guessed-name-in-T9-prose-sentence", [
        (T9_SENT, T9_SENT.replace("one-per-8,", "one-per-8 (SpecialCreditRsp),")),
    ]),
    ("M6-single-carrier-T8-prose-only", [
        (T8_SENT, T8_SENT.replace("SPECIAL CREDIT MESSAGE", "UNKNOWN MESSAGE")),
    ]),
    ("M7-single-carrier-T8-comment-only", [
        (T8_DEF, T8_DEF.replace("special credit message", "unknown message")),
    ]),
    ("M8-single-carrier-T9-comment-only", [
        (T9_DEF, T9_DEF.replace("deliberately UNNAMED", "deliberately type 9")),
    ]),

    # ---- M10-M13: vms-3f4 item 1, the four missed guessed-name SHAPES -----
    ("M10-guessed-name-shape-dollar-identifier", [
        (T9_SENT, T9_SENT.replace("one-per-8,", "one-per-8 (SCS$CREDRSP),")),
    ]),
    ("M11-guessed-name-shape-bare-allcaps", [
        (T9_SENT, T9_SENT.replace("one-per-8,", "one-per-8 (CREDRSP),")),
    ]),
    ("M12-guessed-name-shape-spaced-title-case", [
        (T9_SENT, T9_SENT.replace("one-per-8,", "one-per-8 (Special Credit Response),")),
    ]),
    ("M13-guessed-name-shape-kebab-case", [
        (T9_SENT, T9_SENT.replace("one-per-8,", "one-per-8 (special-credit-response),")),
    ]),

    # ---- M14-M18: vms-3f4 item 2, the previously-unscanned carriers -------
    ("M14-unscanned-carrier-predefine-block-comment", [
        (PREDEF_SENT, PREDEF_SENT.replace("its unnamed", "its CREDRSP unnamed")),
    ]),
    ("M15-unscanned-carrier-namespace-preamble", [
        (PREAMBLE, PREAMBLE.replace("frames;", "frames (CREDRSP);")),
    ]),
    ("M16-unscanned-carrier-T8-sentence", [
        (T8_SENT, T8_SENT.replace("identified by", "(CREDRSP) identified by")),
    ]),
    ("M17-unscanned-carrier-T8-define-comment", [
        (T8_DEF, T8_DEF.replace("special credit message --",
                                "special credit message (CREDRSP) --")),
    ]),
    ("M18-unscanned-carrier-mtype-name-doc-comment", [
        (MTYPENAME_DOC, MTYPENAME_DOC.replace(
            "guessed-name pattern.", "guessed-name pattern (CREDRSP).")),
    ]),

    # ---- M19-M21: vms-3f4 item 3, the stale doc-comment contradiction -----
    ("M19-mtypename-doc-loses-T8-identification", [
        (MTYPENAME_DOC, MTYPENAME_DOC.replace(
            '8 is the special credit message, identified by\n'
            ' * vms-f03/#128, but still renders as "type 8"',
            '8 is a control message type, but still renders as "type 8"')),
    ]),
    ("M20-mtypename-doc-stale-NOT-identified-reinstated", [
        (MTYPENAME_DOC,
         ' * scs_env_mtype_name - a static, never-NULL name for logs. The '
         'names of 0..7\n'
         ' * are spec sec 4(h)(1a)\'s; 8 and 9 render as "type 8"/"type 9" '
         'because they are\n'
         ' * NOT identified and naming them in a log would leak a guess '
         'into the record.'),
    ]),
    ("M21-mtypename-doc-stale-UN-hyphen-IDENTIFIED-reinstated", [
        (MTYPENAME_DOC, MTYPENAME_DOC.replace(
            "guessed-name pattern.", "guessed-name pattern (UN-IDENTIFIED).")),
    ]),
]

# A benign, content-PRESERVING paragraph reflow -- same words, different line
# break -- must NOT red the gate (vms-c84 M9: the gate's first draft anchored
# to raw lines and reflow read as content loss).
_REFLOW_OLD = (
    "rate falls, matching the credit-flush accounting identity p. 2-44 "
    "predicts.\n"
    " * 9 is its paired response, one-per-8, but is DELIBERATELY left "
    "unnamed: no")
_REFLOW_NEW = (
    "rate falls, matching the credit-flush accounting identity p. 2-44\n"
    " * predicts. 9 is its paired response, one-per-8, but is "
    "DELIBERATELY left\n"
    " * unnamed: no")
assert (" ".join(_REFLOW_OLD.replace(" * ", " ").split()) ==
       " ".join(_REFLOW_NEW.replace(" * ", " ").split())), \
    "the reflow mutant is not actually content-preserving"

BENIGN_MUTANTS = [
    ("M9-benign-paragraph-reflow-must-not-red", [(_REFLOW_OLD, _REFLOW_NEW)]),
]


def run_gate(root):
    env = dict(os.environ)
    env["OVMX_SCS_ENV_ROOT"] = root
    env["PYTHONDONTWRITEBYTECODE"] = "1"
    # Force the HOST-INDEPENDENT arm, same reasoning as test_scs_dir_mutants.py:
    # this battery scores the PROSE-vs-EXPECTED pinning, not the wire arm
    # (that is test_scs_figures_wire_mutants.py's job), and a path that
    # cannot exist makes every mutant here run against the same,
    # deterministic half of the gate regardless of whether this host has the
    # lab captures mounted.
    env["OVMX_LAB_CAPTURES"] = os.path.join(root, "no-such-capture-dir")
    env.pop("OVMX_SCS_REQUIRE_WIRE", None)
    p = subprocess.run([sys.executable, GATE], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main():
    all_ids = [m[0] for m in MUTANTS] + [m[0] for m in BENIGN_MUTANTS]
    dupes = sorted({i for i in all_ids if all_ids.count(i) > 1})
    if dupes:
        print("FAIL duplicate mutant ids: %r" % (dupes,))
        return 1

    tmp = tempfile.mkdtemp(prefix="scs_env_mutants.")
    try:
        # The whole src/vmsscs tree: the gate's open-coded-offset scan walks
        # every .c file in it, and scs_start.c/scs_hello.c are read directly.
        shutil.copytree(os.path.join(ROOT, "src", "vmsscs"),
                        os.path.join(tmp, "src", "vmsscs"))
        os.makedirs(os.path.join(tmp, "tools", "cluster"), exist_ok=True)
        shutil.copy2(
            os.path.join(ROOT, "tools", "cluster", "scs_env_measure.py"),
            os.path.join(tmp, "tools", "cluster", "scs_env_measure.py"))

        env_h_path = os.path.join(tmp, ENV_H_REL)
        with open(env_h_path, encoding="utf-8") as fh:
            pristine = fh.read()

        def restore():
            with open(env_h_path, "w", encoding="utf-8") as fh:
                fh.write(pristine)

        def apply(edits):
            text = pristine
            for old, new in edits:
                if old not in text:
                    return None
                mutated = text.replace(old, new, 1)
                if mutated == text:
                    return None
                text = mutated
            return text

        restore()
        rc, out = run_gate(tmp)
        if rc != 0:
            print("FAIL the CONTROL (unmutated scratch copy) is not green; "
                  "no kill below would mean anything.\n%s" % out)
            return 1
        print("control: unmutated scratch copy is GREEN (%s)"
              % out.strip().splitlines()[-1])

        killed, survivors, unapplied = [], [], []
        for name, edits in MUTANTS:
            mutated = apply(edits)
            if mutated is None:
                unapplied.append(name)
                restore()
                continue
            with open(env_h_path, "w", encoding="utf-8") as fh:
                fh.write(mutated)
            rc, out = run_gate(tmp)
            if rc != 0:
                killed.append(name)
            else:
                survivors.append((name, out.strip().splitlines()[-1]))
            restore()
            rc2, out2 = run_gate(tmp)
            if rc2 != 0:
                print("FAIL control did not come back green after mutant "
                      "%s -- the battery is unsound from here.\n%s"
                      % (name, out2))
                return 1

        benign_ok, benign_failed = [], []
        for name, edits in BENIGN_MUTANTS:
            mutated = apply(edits)
            if mutated is None:
                unapplied.append(name)
                restore()
                continue
            with open(env_h_path, "w", encoding="utf-8") as fh:
                fh.write(mutated)
            rc, out = run_gate(tmp)
            if rc == 0:
                benign_ok.append(name)
            else:
                benign_failed.append((name, out.strip().splitlines()[-1]))
            restore()
            rc2, out2 = run_gate(tmp)
            if rc2 != 0:
                print("FAIL control did not come back green after benign "
                      "mutant %s -- the battery is unsound from here.\n%s"
                      % (name, out2))
                return 1

        for name in unapplied:
            print("FAIL mutant %s did not apply: its anchor text is not in "
                  "scs_env.h (or one of its own edits is a no-op). A mutant "
                  "that does not change the file is not a mutant." % name)
        for name, out in survivors:
            print("FAIL mutant %s SURVIVED scs_env_figures: %s" % (name, out))
        for name, out in benign_failed:
            print("FAIL benign mutant %s REDDED scs_env_figures (it should "
                  "not have -- it changes no content, only line breaks): %s"
                  % (name, out))

        failures = len(survivors) + len(unapplied) + len(benign_failed)
        print("%d mutant(s) + %d benign check(s), %d killed, %d benign "
              "confirmed safe, %d survived, %d failed to apply"
              % (len(MUTANTS), len(BENIGN_MUTANTS), len(killed),
                 len(benign_ok), len(survivors), len(unapplied)))
        return 1 if failures else 0
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
