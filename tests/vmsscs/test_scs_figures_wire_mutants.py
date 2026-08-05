#!/usr/bin/env python3
"""
test_scs_figures_wire_mutants.py -- vms-371. THE BATTERY FOR THE NEW HALF.

Each of the six SCS figures gates now does two things: it pins the PROSE to the
measurement tool's EXPECTED table (mutation batteries for that half already
exist -- scs_disc_mutants, scs_reason_mutants, scs_dir_mutants,
scs_join_capability_mutants), and, since vms-371, it pins EXPECTED to the
PACKETS. This file is the battery for the second half, and for the loader the
first half turned out to depend on.

WHY IT EXISTS. `ctest -L scs` was 32/32 GREEN with scs_disc_figures,
scs_credit_figures, scs_reason_figures and scs_connect_data_figures all passing
while the four tools they cite were red -- 23, 11, 13 and 18 checks
respectively. Nothing in ctest opened a pcap, so "the prose matches the
measurement" was really "the prose matches a table, and the table also
drifted". A gate that cannot fail for the reason it exists is not a gate.

FIVE MUTANTS PER GATE, plus one control that is itself a measurement:

  CTRL    the unmutated scratch tree is GREEN *and its output proves the wire
          arm actually ran*. Without that second half the whole battery could
          be scoring the no-captures arm and reporting kills that mean nothing.

  WIRE    one capture the census reads is TRUNCATED to its 24-byte pcap global
          header: a well-formed capture holding zero records. The packets now
          say something else, EXPECTED and the prose are untouched, and the
          gate must red. This is the defect vms-371 was opened on, reproduced
          deliberately. (Halving the file was tried first and scs_dir_figures
          survived it -- see PCAP_GLOBAL_HEADER below.)

  PYCACHE the same-size, same-second edit. The scratch measure script's
          __pycache__ is PRIMED with importlib's file loader, then one figure
          in EXPECTED is changed to a string of EXACTLY the same length and the
          mtime is restored to the primed value -- so the cached .pyc still
          validates on (mtime-in-seconds, size) and any importlib file loader
          hands back the OLD table. The battery PROVES that staleness in-process
          (see prove_pycache_staleness) before scoring the mutant, then runs the
          gate WITH NO CAPTURES so the only thing that can kill the mutant is
          the gate reading the source text. That is the compile()+exec() form in
          scs_wire.load_source(), and this is the evidence it works.

  HOLLOW  the tool's rederive() is overridden to return no results. A gate that
          "re-derived" zero figures is not re-deriving; scs_wire must red.

  UNMEASURED  EXPECTED grows a figure that nothing measures. The prose could
          then be pinned to a number no packet supports. scs_wire's coverage
          check must red.

Nothing here writes to the repo: the measurement scripts are copied into a
scratch tree and the captures are mirrored as symlinks (with the one truncated
file a real copy). The headers and the spec are read in place, never written.

NO CAPTURES ON THIS HOST? CTRL/WIRE/HOLLOW are then unscorable and are reported
as SKIPPED by name -- the PYCACHE and UNMEASURED mutants still run, because
neither needs a packet. The exit status reflects only what was actually scored,
and the skip is printed, never swallowed.

Clean-room (CLAUDE.md rule 8): the only bytes read are our own lab captures and
our own source tree.
"""

import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)
import scs_wire                                                    # noqa: E402

ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
LAB1 = os.environ.get("OVMX_LAB_CAPTURES", "/data/training/vax/cluster/captures")
LAB2 = os.environ.get("OVMX_LAB2_CAPTURES",
                      "/data/training/vax/cluster/captures-lab2")

# name -> gate script, measurement script, env var naming the measurement,
#         capture library, capture to truncate, env var naming the library,
#         (old, new) SAME-LENGTH edit to a figure the PROSE is pinned to.
GATES = [
    dict(name="scs_disc_figures",
         gate="test_scs_disc_figures.py",
         measure="tools/cluster/scs_disc_measure.py",
         measure_env="OVMX_SCS_DISC_MEASURE",
         caps=LAB1, caps_env="OVMX_LAB_CAPTURES",
         truncate="formation-ci1.pcap",
         edit=('"n_captures": 47,', '"n_captures": 41,')),
    dict(name="scs_reason_figures",
         gate="test_scs_reason_figures.py",
         measure="tools/cluster/scs_reason_measure.py",
         measure_env="OVMX_SCS_REASON_MEASURE",
         caps=LAB1, caps_env="OVMX_LAB_CAPTURES",
         truncate="formation-ci1.pcap",
         edit=('"n_captures": 47,', '"n_captures": 41,')),
    dict(name="scs_credit_figures",
         gate="test_scs_credit_figures.py",
         measure="tools/scs_credit_measure.py",
         measure_env="OVMX_SCS_CREDIT_MEASURE",
         caps=LAB1, caps_env="OVMX_LAB_CAPTURES",
         truncate="formation-ci1.pcap",
         edit=('"grounding_190_total": 20459,', '"grounding_190_total": 20451,')),
    dict(name="scs_connect_data_figures",
         gate="test_scs_connect_data_figures.py",
         measure="tools/scs_connect_data_measure.py",
         measure_env="OVMX_SCS_CONNDATA_MEASURE",
         caps=LAB1, caps_env="OVMX_LAB_CAPTURES",
         truncate="formation-ci1.pcap",
         edit=('"connect_frames": 1425,', '"connect_frames": 1421,')),
    dict(name="scs_dir_figures",
         gate="test_scs_dir_figures.py",
         measure="tools/cluster/scs_dir_role_measure.py",
         measure_env="OVMX_SCS_DIR_MEASURE",
         caps=LAB1, caps_env="OVMX_LAB_CAPTURES",
         truncate="formation-ci1-joinwindow.pcap",
         edit=('"request_credit_hist": {0: 2, 1: 4},',
               '"request_credit_hist": {0: 2, 1: 7},')),
    # The vms-578 ACCEPTANCE bracket -- the three runs that JOIN, the evidence
    # the whole SCA layer rests on. Until vms-371 no gate pinned it at all, so
    # its mutants deliberately target EXPECTED_578 and the B1 capture.
    dict(name="scs_join_capability_figures",
         gate="test_scs_join_capability_figures.py",
         measure="tools/cluster/scs_join_capability_measure.py",
         measure_env="OVMX_SCS_JOINCAP_MEASURE",
         caps=LAB2, caps_env="OVMX_LAB2_CAPTURES",
         truncate="vms578-B1-lab2-vaxlab4-20260805.pcap",
         edit=('"cm_190_tx": 509,', '"cm_190_tx": 519,')),
]

problems = []
scored = 0
killed = 0
skipped = []


def note(ok, what):
    global scored, killed
    scored += 1
    if ok:
        killed += 1
        print("  kill %s" % what)
    else:
        problems.append(what)
        print("  SURVIVED %s" % what)


# A classic pcap global header is 24 bytes. Truncating a capture to exactly
# that leaves a WELL-FORMED file with ZERO records -- the strongest available
# wire mutation that is still a legal capture. Halving the file was tried first
# and scs_dir_figures SURVIVED it: that census reads one capture whose whole
# dialogue sits in the first half, so half the bytes still measured the same
# thing. A mutation a gate can survive for a boring reason is not a mutation.
PCAP_GLOBAL_HEADER = 24


def mirror_captures(src, dst, truncate):
    """Symlink every file under `src` into `dst`, preserving subdirectories.

    Every file, not just *.pcap: scs_reason_measure reads its SDA extract out
    of the capture directory and the lab-2 library carries the scsd logs beside
    the captures. Mirroring only pcaps made the scs_reason CONTROL red for a
    reason that had nothing to do with any mutant.

    scs_connect_data_measure globs `**/*.pcap` recursively, so the shape of the
    tree matters too, not just the file list.

    `truncate`, if named, is written as a REAL file holding only the pcap
    global header -- that is the wire mutation, and it must not be a link back
    into the read-only library.
    """
    n = 0
    for dirpath, _dirs, files in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        out = dst if rel == "." else os.path.join(dst, rel)
        os.makedirs(out, exist_ok=True)
        for f in files:
            sp, d = os.path.join(dirpath, f), os.path.join(out, f)
            if not os.path.isfile(sp):
                continue
            if truncate and f == truncate:
                open(d, "wb").write(open(sp, "rb").read()[:PCAP_GLOBAL_HEADER])
            elif f.endswith(".pcap"):
                os.symlink(sp, d)
                n += 1
            else:
                os.symlink(sp, d)
    return n


def scratch_tools(dst):
    """Copy every tools/ python file into `dst`, keeping relative layout.

    Whole directories, not single files: scs_reason_measure.py and
    scs_join_capability_measure.py import dissect_sca.py from beside
    themselves, so a lone copy would fail to import for a reason that has
    nothing to do with any mutant.
    """
    for sub in ("tools", "tools/cluster"):
        src = os.path.join(ROOT, sub)
        out = os.path.join(dst, sub)
        os.makedirs(out, exist_ok=True)
        for f in os.listdir(src):
            if f.endswith(".py") and os.path.isfile(os.path.join(src, f)):
                shutil.copyfile(os.path.join(src, f), os.path.join(out, f))


def run_gate(spec, tree, capdir, extra_env=None, want_bytecode=False):
    env = dict(os.environ)
    env["OVMX_SCS_REQUIRE_WIRE"] = "0"
    env[spec["measure_env"]] = os.path.join(tree, spec["measure"])
    # Both capture variables are set on every run: OVMX_LAB_CAPTURES is what
    # scs_wire reads, OVMX_LAB2_CAPTURES is what the join-capability gate reads
    # first. Setting only one leaves the other pointing at the real library and
    # the mutant is then scored against unmutated packets.
    env["OVMX_LAB_CAPTURES"] = capdir
    env["OVMX_LAB2_CAPTURES"] = capdir
    if want_bytecode:
        env.pop("PYTHONDONTWRITEBYTECODE", None)
    else:
        env["PYTHONDONTWRITEBYTECODE"] = "1"
    env.update(extra_env or {})
    r = subprocess.run([sys.executable, os.path.join(HERE, spec["gate"])],
                       env=env, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def prove_pycache_staleness(path, old, new):
    """The control FOR the PYCACHE mutant, run before the mutant is scored.

    Primes __pycache__ through importlib's source-file loader, applies the
    same-size edit, restores the mtime, and then shows -- in this process --
    that the file loader still returns the OLD figure while
    scs_wire.load_source() returns the NEW one. If that is not what happens,
    the mutant below is not testing what it claims and the battery says so
    instead of banking a meaningless kill.

    Returns (stale_confirmed, detail).
    """
    import importlib.util

    def tables(mod):
        """Every checked-in measurement record the module holds.

        Not just EXPECTED: scs_join_capability_measure keeps the vms-578
        ACCEPTANCE bracket in EXPECTED_578, and that is the table this
        battery mutates for that gate. Comparing EXPECTED alone reported
        'load_source returned the STALE table' when in truth it had returned
        a correctly-updated EXPECTED_578 that nothing was looking at.
        """
        return repr([(k, mod.__dict__[k]) for k in sorted(mod.__dict__)
                     if k.startswith("EXPECTED")])

    name = "ovmx_pycache_probe_%d" % abs(hash(path))
    dont = sys.dont_write_bytecode
    sys.dont_write_bytecode = False
    try:
        spec = importlib.util.spec_from_file_location(name, path)
        mod = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)
        before = tables(mod)
        st = os.stat(path)

        src = open(path, encoding="utf-8").read()
        assert len(old) == len(new), "the PYCACHE edit must not change the size"
        if src.count(old) != 1:
            return False, "the anchor %r appears %d times" % (old, src.count(old))
        open(path, "w", encoding="utf-8").write(src.replace(old, new))
        os.utime(path, (st.st_atime, st.st_mtime))
        if os.stat(path).st_size != st.st_size:
            return False, "the edit changed the file size"

        sys.modules.pop(name, None)
        spec = importlib.util.spec_from_file_location(name, path)
        mod2 = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod2)
        cached = tables(mod2)
        fresh = tables(scs_wire.load_source(path, name + "_fresh"))
        sys.modules.pop(name, None)
    finally:
        sys.dont_write_bytecode = dont

    if cached != before:
        return False, ("importlib did NOT serve a stale table here (this "
                       "filesystem may not have second-granularity mtimes), so "
                       "the mutant proves nothing about the loader")
    if fresh == cached:
        return False, "scs_wire.load_source() returned the STALE table"
    return True, "importlib served the pre-edit table; load_source() did not"


def main():
    print("test_scs_figures_wire_mutants: vms-371")
    tmp = tempfile.mkdtemp(prefix="scs_wire_mutants.")
    try:
        for spec in GATES:
            print("\n== %s" % spec["name"])
            have_caps = os.path.isdir(spec["caps"])

            # ---- the arms that need packets --------------------------------
            if not have_caps:
                skipped.append("%s CTRL/WIRE/HOLLOW (no captures under %s)"
                               % (spec["name"], spec["caps"]))
                print("  SKIP CTRL/WIRE/HOLLOW: no captures under %s" % spec["caps"])
            else:
                tree = os.path.join(tmp, spec["name"], "ctrl")
                scratch_tools(tree)
                caps = os.path.join(tmp, spec["name"], "caps")
                mirror_captures(spec["caps"], caps, None)
                rc, out = run_gate(spec, tree, caps)
                if rc != 0:
                    problems.append("%s: the CONTROL is not green; no kill below "
                                    "would mean anything\n%s" % (spec["name"], out))
                    print("  CONTROL NOT GREEN -- skipping this gate")
                    continue
                if "re-derived from the packets" not in out:
                    problems.append("%s: the CONTROL passed WITHOUT re-deriving "
                                    "from the packets -- this battery would be "
                                    "scoring the no-captures arm" % spec["name"])
                    print("  CONTROL did not read the wire -- skipping this gate")
                    continue
                print("  control: green AND the wire arm ran")

                # WIRE: the packets change, EXPECTED and the prose do not.
                wcaps = os.path.join(tmp, spec["name"], "caps-truncated")
                n = mirror_captures(spec["caps"], wcaps, spec["truncate"])
                rc, out = run_gate(spec, tree, wcaps)
                note(rc != 0, "WIRE: %s truncated (%d captures mirrored) -- the "
                              "gate must red when the packets stop supporting "
                              "the table" % (spec["truncate"], n))

                # HOLLOW: rederive() returns nothing at all.
                htree = os.path.join(tmp, spec["name"], "hollow")
                scratch_tools(htree)
                hm = os.path.join(htree, spec["measure"])
                with open(hm, "a", encoding="utf-8") as fh:
                    fh.write("\n\ndef rederive(capdir, **kw):\n"
                             "    return [], set(WIRE_KEYS)\n")
                rc, out = run_gate(spec, htree, caps)
                note(rc != 0, "HOLLOW: rederive() re-derives nothing -- the gate "
                              "must red rather than report a green wire arm")

            # ---- the arms that need no packets -----------------------------
            # PYCACHE runs with the wire arm OFF (a capture directory that
            # cannot exist), so the ONLY thing that can kill it is the gate
            # reading the measurement source rather than a cached .pyc. With
            # the wire arm on, a kill would not be attributable to the loader.
            nocaps = os.path.join(tmp, spec["name"], "no-such-captures")
            ptree = os.path.join(tmp, spec["name"], "pycache")
            scratch_tools(ptree)
            pm = os.path.join(ptree, spec["measure"])
            ok, detail = prove_pycache_staleness(pm, *spec["edit"])
            if not ok:
                problems.append("%s: the PYCACHE mutant could not be set up: %s"
                                % (spec["name"], detail))
                print("  PYCACHE SETUP FAILED: %s" % detail)
            else:
                print("  pycache control: %s" % detail)
                rc, out = run_gate(spec, ptree, nocaps, want_bytecode=True)
                note(rc != 0,
                     "PYCACHE: same-size, same-second edit %r -> %r behind a "
                     "primed __pycache__, wire arm OFF" % spec["edit"])

            # UNMEASURED: a figure the prose could be pinned to that no packet
            # supports. Appended AFTER WIRE_KEYS is computed, so it lands in
            # neither WIRE_KEYS nor NON_WIRE_KEYS however the tool builds them.
            utree = os.path.join(tmp, spec["name"], "unmeasured")
            scratch_tools(utree)
            um = os.path.join(utree, spec["measure"])
            with open(um, "a", encoding="utf-8") as fh:
                fh.write("\n\nEXPECTED = dict(EXPECTED)\n"
                         "EXPECTED['ovmx_unmeasured_figure_vms371'] = 1\n")
            rc, out = run_gate(spec, utree, nocaps)
            note(rc != 0, "UNMEASURED: EXPECTED grew a figure nothing re-derives")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print("\n%d mutants scored, %d killed, %d survived"
          % (scored, killed, scored - killed))
    for s in skipped:
        print("SKIPPED %s" % s)
    for p in problems:
        print("PROBLEM %s" % p)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
