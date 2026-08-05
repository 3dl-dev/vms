#!/usr/bin/env python3
"""
scs_wire.py -- the shared spine of the SCS *figures* gates (vms-371).

WHAT WENT WRONG, AND WHAT THIS FIXES
------------------------------------
`ctest -L scs` was 32/32 GREEN with scs_disc_figures, scs_credit_figures,
scs_reason_figures and scs_connect_data_figures all passing, while the four
measurement tools those gates cite were FAILING (23, 11, 13 and 18 checks red
respectively -- six lab-2 captures had been deposited in the lab-1 grounding
library and moved every census). The gates could not see it: each one read the
checked-in EXPECTED table out of the measurement script and string-matched it
against the prose. Table and prose agreed, so the gate was green; neither
matched the wire.

A figures gate has exactly one job -- "the prose still says what the captures
say" -- and it was doing the half that cannot fail for the reason that matters.

THE RULE THIS MODULE ENFORCES
-----------------------------
1. On a host WITH the captures, the gate RE-DERIVES from the packets by calling
   the measurement tool's own `rederive(capdir)` and reds on any disagreement.
   Because the gate separately pins EXPECTED to the prose, a green gate on such
   a host now means wire == EXPECTED == prose.

2. That transitivity is only worth something if every figure the prose is held
   to is actually re-derived. `require_coverage()` makes an unmeasured figure a
   RED rather than a silent gap: each tool declares WIRE_KEYS (re-derived from
   packets) and NON_WIRE_KEYS (declarations a capture cannot show, e.g. how
   many system roots the lab has), and any EXPECTED key in neither is a
   failure.

3. On a host WITHOUT the captures the gate says so LOUDLY -- a banner, not a
   parenthetical -- and still runs every host-independent check. It never
   silently passes. Set OVMX_SCS_REQUIRE_WIRE=1 (a lab host, or a CI job that
   has mounted the library) to turn the absence itself into a failure.

WHY compile()+exec() AND NEVER __import__
-----------------------------------------
`load_source()` is the pycache-proof loader. importlib's source-file loader
validates its cached .pyc on (mtime-in-SECONDS, size), so an edit that keeps a
file the same length and lands in the same second as a previous run silently
reuses the OLD bytecode -- i.e. the OLD EXPECTED. Every gate reads EXPECTED out
of the measurement module, so such a mutation would survive the gate entirely.
test_scs_figures_wire_mutants.py performs exactly that same-size, same-second
edit against a primed __pycache__ and requires each gate to red.

Clean-room (CLAUDE.md rule 8): nothing here reads a VSI/HPE artifact. The only
inputs are our own lab captures and our own source tree.
"""

import glob
import importlib.util
import os
import sys

# Where a gate looks for the lab-1 grounding library, if the tool's own default
# is not where this host keeps it. The gates' mutation batteries also use it to
# force the no-captures arm.
ENV_CAPTURES = "OVMX_LAB_CAPTURES"
# Turn "no captures here" from a loud pass into a failure.
ENV_REQUIRE = "OVMX_SCS_REQUIRE_WIRE"

BAR = "!" * 76


def load_source(path, name):
    """Execute a Python file FROM SOURCE, never from cached bytecode.

    See the module docstring: `__import__` / spec_from_file_location would
    accept a stale __pycache__ entry for a same-size, same-second edit, and the
    figures gates read their EXPECTED tables out of the module this returns.
    """
    with open(path, encoding="utf-8") as fh:
        src = fh.read()
    mod = importlib.util.module_from_spec(
        importlib.util.spec_from_loader(name, loader=None))
    mod.__file__ = path
    exec(compile(src, path, "exec"), mod.__dict__)
    return mod


def capture_dir(default, need=("*.pcap",)):
    """Resolve the capture library, or None.

    OVMX_LAB_CAPTURES wins over the tool's compiled-in default so that a lab
    host with the library somewhere else, and a mutation battery that wants the
    no-captures arm, both work without editing a tool.

    A directory that exists but holds none of `need` counts as ABSENT: an empty
    mount point must not be mistaken for a measurable library.
    """
    d = os.environ.get(ENV_CAPTURES) or default
    if not d or not os.path.isdir(d):
        return None
    for pat in need:
        if not glob.glob(os.path.join(d, pat)):
            return None
    return d


def announce_absent(gate, default, check):
    """The loud arm. Returns False so callers can `if not announce_absent(...)`.

    `check` is the gate's own failure recorder; under OVMX_SCS_REQUIRE_WIRE the
    absence becomes a recorded failure rather than a banner.
    """
    where = os.environ.get(ENV_CAPTURES) or default
    required = os.environ.get(ENV_REQUIRE, "") not in ("", "0")
    print(BAR)
    print("!! %s: THE WIRE WAS NOT READ." % gate)
    print("!! No lab captures under %s" % where)
    print("!! (host-only, not in git -- CLAUDE.md rule 8). This run checked the")
    print("!! checked-in measurement record against the prose ONLY. It CANNOT")
    print("!! tell you the record still matches the packets. Re-run on a lab")
    print("!! host, or point %s at the library." % ENV_CAPTURES)
    print(BAR)
    check(not required,
          "%s: %s is set and there are no captures under %s -- this host was "
          "declared able to re-derive from the wire and cannot"
          % (gate, ENV_REQUIRE, where))
    return False


class _KeyRecorder(dict):
    """A dict that remembers which keys were READ.

    Substituted for a tool's module-global EXPECTED while its re-derivation
    runs, so `covered` is a MEASUREMENT of which figures the tool actually
    compared against the packets, not a claim the tool makes about itself. Add
    a figure to EXPECTED without wiring it into the comparison and
    require_coverage() reds -- the hand-maintained-list version of this would
    not have noticed.
    """

    def __init__(self, base):
        super().__init__(base)
        self.seen = set()

    def __getitem__(self, key):
        self.seen.add(key)
        return super().__getitem__(key)

    def get(self, key, default=None):
        self.seen.add(key)
        return super().get(key, default)


def rederive(gate, mod, capdir, check, **kw):
    """Re-derive every figure from the packets. Returns the covered key set.

    `mod.rederive(capdir, **kw)` is the measurement tool's own entry point and
    returns ([(ok, label), ...], covered_expected_keys). Every non-ok result
    becomes a gate failure, so a gate can no longer be green while the tool it
    cites is red -- which is the exact defect vms-371 was opened on.

    The returned coverage is the INTERSECTION of what the tool says it covers
    and what it was observed to read (see _KeyRecorder): a tool may narrow its
    own claim -- scs_credit_measure.rederive(quick=True) does -- but it cannot
    widen it past the keys it actually touched.
    """
    if not hasattr(mod, "rederive"):
        check(False, "%s: %s has no rederive() entry point, so this gate cannot "
                     "re-derive from the wire" % (gate, os.path.basename(mod.__file__)))
        return set()
    recorder = _KeyRecorder(mod.EXPECTED)
    saved = mod.EXPECTED
    mod.EXPECTED = recorder
    try:
        results, covered = mod.rederive(capdir, **kw)
        covered = set(covered) & recorder.seen
    except Exception as exc:                      # a broken tool is a red gate,
        check(False, "%s: re-derivation from %s raised %s: %s"      # not a skip
              % (gate, capdir, type(exc).__name__, exc))
        return set()
    finally:
        mod.EXPECTED = saved
    bad = [label for ok, label in results if not ok]
    check(not bad,
          "%s: the measurement re-derived from %s disagrees with the checked-in "
          "record in %d of %d checks -- the prose is pinned to a table the "
          "packets no longer support:\n    %s"
          % (gate, capdir, len(bad), len(results), "\n    ".join(bad[:20])))
    check(len(results) > 0,
          "%s: re-derivation ran zero checks against %s" % (gate, capdir))
    print("[%s: %d figures re-derived from the packets in %s, %d disagreements]"
          % (gate, len(results), capdir, len(bad)))
    return set(covered)


def require_coverage(gate, mod, covered, check):
    """Every EXPECTED key must be re-derived, or declared as not measurable.

    Without this, adding an unmeasured figure to EXPECTED silently reopens the
    hole: the prose would be pinned to it and nothing would ever check it
    against a packet.
    """
    expected_keys = set(mod.EXPECTED)
    declared = set(getattr(mod, "NON_WIRE_KEYS", ()))
    wire = set(getattr(mod, "WIRE_KEYS", ()))
    check(wire | declared >= expected_keys,
          "%s: EXPECTED carries figure(s) %s that are in neither WIRE_KEYS nor "
          "NON_WIRE_KEYS -- a figure nobody re-derives cannot be gated"
          % (gate, sorted(expected_keys - wire - declared)))
    check(not (wire & declared),
          "%s: %s are declared both measurable and not measurable"
          % (gate, sorted(wire & declared)))
    if covered is not None:
        check(covered >= wire,
              "%s: WIRE_KEYS claims %s is re-derived from the packets, but "
              "rederive() never read it -- an unmeasured figure the prose is "
              "pinned to" % (gate, sorted(wire - covered)))
    return expected_keys - declared


def gate(name, mod, default_capdir, check, **kw):
    """The whole protocol in one call. Returns True if the wire was read."""
    capdir = capture_dir(default_capdir)
    if capdir is None:
        require_coverage(name, mod, None, check)
        announce_absent(name, default_capdir, check)
        return False
    covered = rederive(name, mod, capdir, check, **kw)
    require_coverage(name, mod, covered, check)
    return True


if __name__ == "__main__":                        # pragma: no cover
    print(__doc__)
    sys.exit(0)
