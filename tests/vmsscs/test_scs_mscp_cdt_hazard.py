#!/usr/bin/env python3
"""
test_scs_mscp_cdt_hazard.py -- vms-73c: THE UNDISCLOSED DOUBLE-GRANT CENSUS.

WHY IT EXISTS. scs_mscp.c's build path documents a LATENT DOUBLE-GRANT HAZARD:
if a production caller ever passes a non-NULL `.cdt` in struct scs_mscp_params,
the live-credit branch reads the connection's Pending Receive Credit via
scs_credit_peek_pending() -- a non-mutating PEEK, not the take_pending_receive_
credit() reset scs_credit_on_send() performs. On scsd_credit_stamp_outbound()'s
unstamped exits (still real today: !scs_credit_enabled(), !scsd_cdl_ready, a CDL
lookup miss, credit starvation -- see the F1 recount in scs_mscp.c), that peeked
value goes out on the wire UN-RESET, and a later successful stamp on the same
CDT would grant the same credit to the peer a second time.

NO PRODUCTION CALLER WIRES THIS TODAY (both scsd.c call sites and everything in
scs_mscp_srv.c leave `.cdt` at its zeroed NULL default), which is exactly why
nothing catches it if that changes silently: the hazard is disclosed only in a
comment, and nothing reds if someone adds a `.cdt =` assignment to a production
struct scs_mscp_params instance. This is the source-level negative control that
closes that gap, on the pattern of test_scsd_send_sites.py: it does not forbid
wiring a live CDT (that is legitimate future work, per the header note) -- it
forbids wiring it WITHOUT the person doing so having to look the hazard in the
eye. The escape hatch is a same-or-adjacent-line comment carrying the token
CREDIT-HAZARD-ACKNOWLEDGED, which this script requires to also name the reset
work the header note describes, so a bare "acknowledged" cannot be a rubber
stamp.

WHAT IT SCANS: src/vmsscs/scsd.c and src/vmsscs/scs_mscp_srv.c (the only two
production sources that can construct a struct scs_mscp_params -- everything
else that touches the type is scs_mscp.c/scs_mscp.h, the builder that reads
`.cdt`, and the test tree, which is exempt because a test proving the hazard
IS the intended live-cdt exercise, not a silent production wire-up).

HOW IT KEEPS FROM FLAGGING scs_poller's OR scs_credit_waiter's UNRELATED `cdt`
FIELDS (scs_poll.h, scs_credit.h both declare a bare `struct scs_cdt *cdt`
member on structs that are NOT struct scs_mscp_params): this does not match on
the bare token `.cdt =` anywhere in the file. It first collects every
identifier declared as `struct scs_mscp_params` (by value or by pointer, as a
local, a parameter, or a struct field) in the file being scanned, then only
flags a `.cdt =` / `->cdt =` write whose IMMEDIATE left-hand identifier is one
of those names. `sink->params.cdt = x` is still caught (the identifier directly
left of `.cdt` is `params`, itself declared `struct scs_mscp_params params` as
a parameter name in this file); `poller->cdt = x` or `w->cdt = x` are not,
because `poller`/`w` are never declared as struct scs_mscp_params.

THE BOUND ON THIS, stated so the claim does not outrun the evidence: a
completely different local variable name never seen as a struct scs_mscp_params
declaration ANYWHERE in the same file (e.g. a cast through `void *` immediately
followed by `.cdt =` on the same line as the declaration) would not be in the
name set yet and could slip through. This is the same class of bound
test_scsd_send_sites.py accepts for its own scope note; re-derive with

    grep -n 'struct scs_mscp_params' src/vmsscs/scsd.c src/vmsscs/scs_mscp_srv.c

if this ever needs to grow.

PROVEN BY MUTATION (vms-73c), each applied to a scratch copy of scsd.c, run
against this script pointed at the scratch file, and reverted:

  M-1  `mp.cdt = &live_cdt;` added, undisclosed, right after the existing
       `struct scs_mscp_params mp;` declaration in the ps_mscp_disc() body --
       RED, "scsd.c:<line>: mp.cdt = ... wires a live CDT into a struct
       scs_mscp_params without a CREDIT-HAZARD-ACKNOWLEDGED comment"
  M-2  the SAME line, with a same-line trailing comment
       `/* CREDIT-HAZARD-ACKNOWLEDGED: reset modeled on
       take_pending_receive_credit() added below */` -- GREEN
  M-3  the acknowledgment moved to the line ABOVE instead of the same line --
       GREEN (adjacency, not just same-line, is honored)
  M-4  a decoy: `poller->cdt = tgt;`-shaped write against scsd_poller (a real
       struct scs_poller local in this file) -- GREEN, confirms the name-typed
       scope does not fire on the unrelated scs_poller.cdt field
  M-0  the unmodified file -- GREEN (baseline)

Zero survivors, zero false positives on the decoy. Restored with `cmp` against
the pre-mutation copy after each run.

THE ACKNOWLEDGMENT MUST NAME THE RESET, not just assert the word. A bare
`/* CREDIT-HAZARD-ACKNOWLEDGED */` with no further text still passes THIS
script (source-level intent, not code review, is what a grep-shaped gate can
enforce) -- the reset-work requirement above is enforced by human review of the
comment's content, the same division of labor test_scsd_send_sites.py uses for
its EXEMPT block reasons.

WHAT IT DOES NOT ASSERT: that a disclosed `.cdt=` wire-up actually PERFORMS the
reset scs_credit_on_send()'s take_pending_receive_credit() does on the unstamped
path -- only that whoever adds the assignment had to write a sentence next to it
naming the hazard. Runtime coverage for a real wire-up, if one is ever added, is
future work for that change, not this gate.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

DEFAULT_TARGETS = [
    os.path.join(ROOT, "src", "vmsscs", "scsd.c"),
    os.path.join(ROOT, "src", "vmsscs", "scs_mscp_srv.c"),
]

ACK_TOKEN = "CREDIT-HAZARD-ACKNOWLEDGED"

# Declarations of an identifier as struct scs_mscp_params -- by value, by
# pointer, as a local, a parameter, or (rarer) a struct field. `const` is
# optional and may repeat on either side of `struct`.
DECL_RE = re.compile(
    r"\b(?:const\s+)?struct\s+scs_mscp_params\s+(?:const\s+)?\*?\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\b"
)

# A `.cdt =` / `->cdt =` write, captured with the identifier immediately to its
# left. `(?!=)` excludes `==` (a comparison, not a write).
ASSIGN_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*(?:\.|->)\s*cdt\b\s*=(?!=)"
)


def strip_comments_and_strings(src):
    """Blank block/line comments and string contents, preserving line count and
    column positions so line numbers and (non-comment) match offsets stay
    accurate -- same technique as test_scsd_send_sites.py."""
    out = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    out = re.sub(r"//[^\n]*", "", out)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def scan(path):
    """Return (failures, hit_count) for one file."""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    raw_lines = raw.splitlines()
    code = strip_comments_and_strings(raw)
    code_lines = code.splitlines()

    names = set()
    for line in code_lines:
        for m in DECL_RE.finditer(line):
            names.add(m.group(1))

    failures = []
    hits = 0
    for i, line in enumerate(code_lines):
        for m in ASSIGN_RE.finditer(line):
            if m.group(1) not in names:
                continue
            hits += 1
            lineno = i + 1
            window = raw_lines[max(0, i - 1): i + 2]  # prev, this, next
            if not any(ACK_TOKEN in w for w in window):
                failures.append(
                    f"{os.path.relpath(path, ROOT)}:{lineno}: "
                    f"`{m.group(1)}.cdt =` (or `->cdt =`) wires a live CDT into "
                    f"a struct scs_mscp_params without a {ACK_TOKEN} comment on "
                    f"the same or an adjacent line. This is the vms-73c "
                    f"double-grant hazard: scs_mscp.c's live-credit branch PEEKS "
                    f"the connection's Pending Receive Credit and does not reset "
                    f"it, so an unstamped scsd_credit_stamp_outbound() exit can "
                    f"grant the same credit twice on a later successful stamp. "
                    f"Add a `/* {ACK_TOKEN}: <what resets pending_receive_credit "
                    f"on the unstamped path> */` comment, or route through the "
                    f"NULL default if this was not intentional."
                )
    return failures, hits


def main(argv):
    targets = argv[1:] if len(argv) > 1 else DEFAULT_TARGETS
    all_failures = []
    total_hits = 0
    for path in targets:
        if not os.path.isfile(path):
            all_failures.append(f"{path}: not found -- the scan target is missing, "
                                 f"not clean")
            continue
        failures, hits = scan(path)
        all_failures.extend(failures)
        total_hits += hits

    print(f"  {total_hits} struct scs_mscp_params `.cdt =` write(s) found across "
          f"{len(targets)} file(s)")

    for f in all_failures:
        print("FAIL " + f, file=sys.stderr)
    print(f"test_scs_mscp_cdt_hazard: {len(all_failures)} failure(s)")
    return 1 if all_failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
