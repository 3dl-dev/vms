#!/usr/bin/env python3
"""
test_scs_diskrun_mutants.py -- vms-ebb: the mutation battery for
test_scs_diskrun_figures.py.

A documentation gate that passes proves nothing until something has been shown
to make it fail. This applies six mutants to COPIES of the two documents the
gate reads and requires the gate to red for every one. Nothing under src/,
docs/ or tools/ is written.

The mutants are the six ways this particular ruling would actually decay:

  M1  a measured per-peer lead figure drifts in the spec
  M2  the control arm stops recording that NO PS CONNECT_REQ reached the wire
      -- i.e. the kill switch stops being a measurement
  M3  the REFUTED claim ("the peer never sends it, there is nothing to fire
      on") comes back in the spec
  M4  the correction that says the signal IS live is dropped from scsd.c
  M5  the open rejoin case (vms-449) is dropped, so the ruling reads as
      settling more than it did
  M6  the lead band disappears from the daemon header, leaving the code with
      the ruling but not its reason

Same shape and reasoning as test_scs_disc_mutants.py.
"""
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
GATE = os.path.join(HERE, "test_scs_diskrun_figures.py")
SPEC = os.path.join(ROOT, "docs/cluster-protocol-spec.md")
DAEMON = os.path.join(ROOT, "src/vmsscs/scsd.c")


def run_gate(spec_path, daemon_path):
    env = dict(os.environ)
    env["OVMX_DISKRUN_SPEC"] = spec_path
    env["OVMX_DISKRUN_DAEMON"] = daemon_path
    p = subprocess.run([sys.executable, GATE], env=env,
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def main():
    spec = open(SPEC, encoding="utf-8").read()
    daemon = open(DAEMON, encoding="utf-8").read()

    rc, out = run_gate(SPEC, DAEMON)
    if rc != 0:
        print("BASELINE IS RED -- the battery cannot mean anything:\n" + out)
        return 1
    print("baseline green")

    mutants = [
        ("M1 lead figure drift (spec)", "spec",
         lambda s: s.replace("2.838 s", "2.938 s")),
        ("M2 control arm stops recording 'none' (spec)", "spec",
         lambda s: s.replace("| **0** | **none**", "| **2** | **two**")),
        ("M3 refuted 'never arrives' claim revived (spec)", "spec",
         lambda s: s.replace(
             "**THE IMMEDIATE TRIGGER IS NOT DEAD FOR WANT OF A SIGNAL.**",
             "The peer never sends that DISCONNECT_REQ, so there is nothing to"
             " fire on.")),
        ("M4 correction dropped (scsd.c)", "daemon",
         lambda s: s.replace("THE IMMEDIATE TRIGGER IS NOT DEAD FOR WANT OF A SIGNAL",
                             "THE IMMEDIATE TRIGGER IS UNNECESSARY")),
        ("M5 rejoin scope limit lost (scsd.c)", "daemon",
         lambda s: s.replace("vms-449", "vms-XXX")),
        ("M6 lead band dropped (scsd.c)", "daemon",
         lambda s: s.replace("2.1-2.9 s", "some seconds")),
    ]

    killed = 0
    survived = []
    with tempfile.TemporaryDirectory() as tmp:
        for i, (name, which, mutate) in enumerate(mutants):
            sp, dp = SPEC, DAEMON
            if which == "spec":
                sp = os.path.join(tmp, "m%d.md" % i)
                text = mutate(spec)
                if text == spec:
                    survived.append(name + " -- MUTANT DID NOT APPLY (the text it"
                                           " edits is gone)")
                    continue
                open(sp, "w", encoding="utf-8").write(text)
            else:
                dp = os.path.join(tmp, "m%d.c" % i)
                text = mutate(daemon)
                if text == daemon:
                    survived.append(name + " -- MUTANT DID NOT APPLY (the text it"
                                           " edits is gone)")
                    continue
                open(dp, "w", encoding="utf-8").write(text)
            rc, out = run_gate(sp, dp)
            if rc == 0:
                survived.append(name)
                print("SURVIVED  " + name)
            else:
                killed += 1
                print("killed    " + name)

    print("test_scs_diskrun_mutants: %d mutant(s), %d killed, %d survived"
          % (len(mutants), killed, len(survived)))
    for s in survived:
        print("FAIL mutant survived: " + s)
    return 1 if survived else 0


if __name__ == "__main__":
    sys.exit(main())
