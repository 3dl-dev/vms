#!/usr/bin/env python3
"""
test_scs_diskrun_figures.py -- vms-ebb: the disk-discovery-trigger ruling must
keep saying what the bracket measured.

NEEDS NO CAPTURES. It compares the figures written into
  docs/cluster-protocol-spec.md   (sec 4(O.4), the ruling and its table)
  src/vmsscs/scsd.c               (scsd_diskrun_ungate_tick()'s header)
against the EXPECTED table in tools/cluster/scs_diskrun_trigger_measure.py,
which is the checked-in record of what the lab-2 bracket measured. Only that
script, on a host with the captures, re-derives EXPECTED itself.

WHY THIS GATE EXISTS, in the specific terms of what went wrong.

  vms-096 deleted a dead disk-discovery trigger and wrote, in the comment that
  replaced it, that re-attaching one "would change a wire path the only passing
  acceptance bracket did not exercise". That was true and it was also unfalsifi-
  able: no test read it, sec 4(O.1)'s bracket ran the DEFAULT environment and so
  never entered the block at all, and the comment's supporting figures were
  measured on a DIFFERENT tree. A ruling in a comment decays into folklore.

  Worse, the first draft of the vms-ebb ruling asserted that the peer NEVER
  sends the DISCONNECT_REQ an immediate trigger would fire on. The bracket
  measured the opposite -- 2 per run, 3 of 3 arms. Had that draft shipped, the
  reason for "one trigger" would have been a false claim about the wire, and
  nothing in ctest would have noticed.

  So this test pins the three things a future reader would otherwise have to
  re-measure to trust the ruling:

  (1) THE COUNTS. Per arm: the peer DISCONNECT_REQs addressed to our
      SCS$DIRECTORY server Con.ID, and the PS SCS$DIRECTORY CONNECT_REQs. The
      control arm's zero is what makes the kill switch a measurement.

  (2) THE LEAD. The 2.1-2.9 s an immediate trigger would buy, and the four
      per-peer figures behind it. If the gate default moves, these move with it
      and the ruling has to be retaken.

  (3) THE NON-CLAIM. "The signal does not arrive" / "there is nothing to fire
      on" must not appear in the spec or in scsd.c. It is the false statement
      the bracket refuted, and it is exactly the kind of tidy sentence that gets
      re-invented by someone summarising the ruling.

WHAT THIS TEST DOES NOT DO: it does not read a pcap and cannot tell you the
measurement is right. It tells you the documents still say what the measurement
said.
"""
import importlib.util
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

SPEC = os.environ.get("OVMX_DISKRUN_SPEC",
                      os.path.join(ROOT, "docs/cluster-protocol-spec.md"))
DAEMON = os.environ.get("OVMX_DISKRUN_DAEMON",
                        os.path.join(ROOT, "src/vmsscs/scsd.c"))
MEASURE = os.environ.get("OVMX_DISKRUN_MEASURE",
                         os.path.join(ROOT,
                                      "tools/cluster/scs_diskrun_trigger_measure.py"))

failures = []


def check(cond, msg):
    if not cond:
        failures.append(msg)


def load_measure():
    spec = importlib.util.spec_from_file_location("scs_diskrun_trigger_measure",
                                                  MEASURE)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def read(path):
    with open(path, "r", encoding="utf-8") as f:
        return f.read()


def spec_section(text):
    """Sec 4(O.4), from its heading to the next heading at the same level."""
    m = re.search(r"^#### 4\(O\.4\).*$", text, re.M)
    check(m is not None, "docs/cluster-protocol-spec.md has no sec 4(O.4) -- the "
                         "vms-ebb ruling is not in the spec at all")
    if m is None:
        return ""
    rest = text[m.end():]
    n = re.search(r"^(#### |## )", rest, re.M)
    return rest[:n.start()] if n else rest


def daemon_section(text):
    """scsd_diskrun_ungate_tick()'s comment block plus its body."""
    m = re.search(r"scsd_diskrun_ungate_tick - ", text)
    check(m is not None, "src/vmsscs/scsd.c has no scsd_diskrun_ungate_tick() -- "
                         "the ONE trigger the ruling is about is gone or renamed")
    if m is None:
        return ""
    start = text.rfind("/*", 0, m.start())
    end = text.find("\n}\n", m.end())
    return text[start:end if end > 0 else len(text)]


def main():
    mod = load_measure()
    spec_text = read(SPEC)
    daemon_text = read(DAEMON)
    sec = spec_section(spec_text)
    fn = daemon_section(daemon_text)

    # ---- (1) THE COUNTS -------------------------------------------------
    # Each arm's row must carry its two counts. The table writes them bold, so
    # the row is located by identity and the digits read out of it.
    for name, exp in mod.EXPECTED.items():
        ident = exp["identity"]
        row = None
        for line in sec.splitlines():
            if line.startswith("|") and ident in line:
                row = line
                break
        check(row is not None,
              "sec 4(O.4) has no table row for arm %s (capture %s)" % (ident, name))
        if row is None:
            continue
        digits = [int(x) for x in re.findall(r"\*\*(\d+)\*\*", row)]
        check(exp["ps_dir_connect_req"] == 0 or exp["ps_dir_connect_req"] in digits
              or ("**%d**" % exp["ps_dir_connect_req"]) in row,
              "arm %s's row does not carry its PSC-UNGATED count %d: %r"
              % (ident, exp["ps_dir_connect_req"], row))
        check(str(exp["peer_disconnect_req_to_dir_server"]) in row,
              "arm %s's row does not carry its peer DISCONNECT_REQ count %d: %r"
              % (ident, exp["peer_disconnect_req_to_dir_server"], row))
        if exp["ungate"] is False:
            check("none" in row.lower(),
                  "the control arm %s's row does not record that NO PS "
                  "CONNECT_REQ reached the wire: %r" % (ident, row))

    # Every arm joined -- the fact the whole ruling turns on.
    check(len(re.findall(r"`CN_3`", sec)) >= len(mod.EXPECTED),
          "sec 4(O.4) does not record CLUSTER_NODES=3 for every arm; the ruling "
          "rests on the control joining too")

    # ---- (2) THE LEAD ---------------------------------------------------
    for lead in mod.EXPECTED_LEAD_S:
        check(("%.3f" % lead) in sec,
              "sec 4(O.4) does not carry the measured per-peer lead %.3f s"
              % lead)
    lo, hi = mod.EXPECTED_LEAD_RANGE
    band = "%.1f–%.1f s" % (lo, hi)
    check(band in sec, "sec 4(O.4) does not state the %s lead band" % band)
    check(band in fn or ("%.1f-%.1f s" % (lo, hi)) in fn,
          "scsd_diskrun_ungate_tick()'s header does not state the %s lead band, "
          "so the code does not carry the reason the ruling gives" % band)
    check(("%d" % mod.EXPECTED_GATE_MS) in fn or
          "SCSD_DISKRUN_GATE_MS_DEFAULT" in daemon_text,
          "the daemon no longer names the %d ms gate the bracket ran at"
          % mod.EXPECTED_GATE_MS)

    # ---- (3) THE NON-CLAIM ----------------------------------------------
    # The refuted sentence family: the signal is absent / there is nothing to
    # fire on. Matching is by claim family within one sentence, not by exact
    # wording, so a reworded revival is caught too.
    subject = r"(disconnect_req|op ?6|signal|frame)"
    absence = r"(never (arriv|sends|comes)|does not arriv|no(thing)? to fire on|"\
              r"absent|0 frames in every arm)"
    for label, body in (("spec sec 4(O.4)", sec),
                        ("scsd_diskrun_ungate_tick()", fn)):
        for sentence in re.split(r"(?<=[.!?])\s+", body):
            s = sentence.lower()
            if re.search(subject, s) and re.search(absence, s):
                # The refutation itself is allowed to quote the dead claim, but
                # only while denying it in the same sentence.
                if re.search(r"not dead|is there|does arrive|live|refut|would be false"
                             r"|must not|not a claim", s):
                    continue
                failures.append(
                    "%s re-asserts the REFUTED claim that the immediate "
                    "trigger's signal does not arrive: %r"
                    % (label, sentence.strip()[:160]))

    # And the positive statement must be present: the signal IS live.
    check(re.search(r"NOT DEAD FOR WANT OF A SIGNAL", fn) is not None,
          "scsd_diskrun_ungate_tick()'s header no longer says the immediate "
          "trigger's signal is live -- that sentence is the correction")

    # ---- the ruling itself ----------------------------------------------
    check("ONE" in fn or "one trigger" in fn.lower(),
          "the daemon no longer states the one-trigger ruling")
    check("vms-449" in sec and "vms-449" in fn,
          "the open rejoin case (vms-449) is not named in both places; a ruling "
          "whose scope limit is lost reads as settling more than it did")

    for f in failures:
        print("FAIL " + f)
    print("test_scs_diskrun_figures: %d failure(s)" % len(failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
