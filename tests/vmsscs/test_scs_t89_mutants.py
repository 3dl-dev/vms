#!/usr/bin/env python3
"""
test_scs_t89_mutants.py -- vms-a58: the mutation battery for
test_scs_t89_figures.py.

A documentation gate that passes proves nothing until something has been shown
to make it fail. This applies nine mutants to COPIES of the three documents the
gate reads and requires the gate to red for every one. Nothing under src/,
docs/ or tools/ is written. (The WIRE / HOLLOW / PYCACHE / UNMEASURED arms of
the same gate are scored by test_scs_figures_wire_mutants.py, which this does
not duplicate: these are the PROSE arms.)

The mutants are the nine ways this particular finding would actually decay --
and it is a finding with an unusual shape, because half of it is a REFUSAL:

  M1  a census figure drifts in the length-unrestricted table
  M2  the OVMX-origin column loses its measured ZERO for type 8 -- the one
      figure the emission ruling rests on ("OVMX has never sent one")
  M3  a row of the owning-SYSAP table drifts
  M4  the honest limit is deleted: the sentence saying the SYSAP census CANNOT
      discriminate, without which 131/131 reads as proof of something it is not
  M5  a structural invariant drifts (the sender-identity row, which is what
      makes the emission ruling actionable)
  M6  the credit ledger grows a residual it did not have -- i.e. the
      measurement that answers the constant-1 lead stops being exact
  M7  the p. 2-44 special credit message is written back in as an
      IDENTIFICATION rather than an eliminated candidate
  M8  types 8 and 9 are called connection-control messages again (the reading
      their own credit behaviour refutes)
  M9  the emission ruling is removed from scsd.c, leaving the answering half
      implemented and the missing half undocumented

Same shape and reasoning as test_scs_diskrun_mutants.py.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GATE = os.path.join(HERE, "test_scs_t89_figures.py")
SPEC = os.path.join(ROOT, "docs/cluster-protocol-spec.md")
DAEMON = os.path.join(ROOT, "src/vmsscs/scsd.c")
DESIGN = os.path.join(ROOT, "docs/design-mscp-direction.md")


def run_gate(spec_path, daemon_path, design_path):
    env = dict(os.environ)
    env["OVMX_SCS_T89_SPEC"] = spec_path
    env["OVMX_SCS_T89_DAEMON"] = daemon_path
    env["OVMX_SCS_T89_DESIGN"] = design_path
    p = subprocess.run([sys.executable, GATE], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


MUTANTS = [
    ("M1 census figure drift (spec)", "spec",
     lambda s: s.replace("| 58 | `8` | 131 | 0 |", "| 58 | `8` | 132 | 0 |")),
    ("M2 OVMX-origin zero for type 8 lost (spec)", "spec",
     lambda s: s.replace("| 58 | `8` | 131 | 0 |", "| 58 | `8` | 131 | 3 |")),
    ("M3 owning-SYSAP row drift (spec)", "spec",
     lambda s: s.replace("| `MSCP$DISK` | 1645 | 101 | 0 | 0 |",
                         "| `MSCP$DISK` | 1645 | 101 | 4 | 4 |")),
    ("M4 the honest limit deleted (spec)", "spec",
     lambda s: s.replace("**So the\nSYSAP split cannot discriminate an SCA-level exchange from an\n`SCS$DIRECTORY`-level one**",
                         "**So the SYSAP census settles it**")),
    ("M5 sender-identity invariant drift (spec)", "spec",
     lambda s: s.replace("| type `8` sender is the first `DISCONNECT_REQ` sender | 131 |",
                         "| type `8` sender is the first `DISCONNECT_REQ` sender | 97 |")),
    ("M6 credit ledger grows a residual (spec)", "spec",
     lambda s: s.replace("| residual | 0 | 0 | 0 |", "| residual | 0 | 0 | 7 |")),
    ("M7 special credit message written back as an identification (spec)", "spec",
     lambda s: s.replace(
         "**One residual, kept because it is evidence.**",
         "Types `8` and `9` are the p. 2-44 special credit message.\n\n"
         "**One residual, kept because it is evidence.**")),
    ("M8 called connection-control messages again (spec)", "spec",
     lambda s: s.replace(
         "**One residual, kept because it is evidence.**",
         "Message types `8` and `9` are the ninth and tenth connection-control "
         "messages.\n\n**One residual, kept because it is evidence.**")),
    ("M9 emission ruling removed (scsd.c)", "daemon",
     lambda s: s.replace("Emitting the type 8 first is REQUIRED",
                         "Emitting the type 8 first is optional")),
]


def main():
    docs = {"spec": open(SPEC, encoding="utf-8").read(),
            "daemon": open(DAEMON, encoding="utf-8").read(),
            "design": open(DESIGN, encoding="utf-8").read()}
    paths = {"spec": SPEC, "daemon": DAEMON, "design": DESIGN}
    suffix = {"spec": ".md", "daemon": ".c", "design": ".md"}

    rc, out = run_gate(SPEC, DAEMON, DESIGN)
    if rc != 0:
        print("BASELINE IS RED -- the battery cannot mean anything:\n" + out)
        return 1
    print("baseline green")

    killed = 0
    survived = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, which, mutate) in enumerate(MUTANTS):
            text = mutate(docs[which])
            if text == docs[which]:
                survived.append(name + " -- MUTANT DID NOT APPLY (the text it "
                                       "edits is gone)")
                print("SURVIVED  " + survived[-1])
                continue
            use = dict(paths)
            use[which] = os.path.join(tmp, "m%d%s" % (i, suffix[which]))
            with open(use[which], "w", encoding="utf-8") as fh:
                fh.write(text)
            rc, out = run_gate(use["spec"], use["daemon"], use["design"])
            if rc == 0:
                survived.append(name)
                print("SURVIVED  " + name)
            else:
                killed += 1
                print("killed    " + name)

    print("test_scs_t89_mutants: %d mutant(s), %d killed, %d survived"
          % (len(MUTANTS), killed, len(survived)))
    for s in survived:
        print("FAIL mutant survived: " + s)
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
