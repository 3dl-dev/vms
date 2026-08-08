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

VMS-CF0 FOLLOW-UP -- 3 DEMONSTRATED BYPASSES, CLOSED. The v1 gate above (M-1
through M-4) scanned line by line and matched the assignment operator only
when it sat DIRECTLY against the declared identifier. Three independent PoCs,
each built as a scratch .c fixture and run against the v1 script, came back
GREEN on an undisclosed `.cdt =` write -- i.e. silently passed the exact
hazard the gate exists to catch:

  B-1  SPLIT-ACROSS-LINES. Both the declaration and the assignment can be
       broken across a newline and the per-line scanner loses them:
           struct scs_mscp_params
               mp;
           mp
               .cdt = &live_cdt;
       Root cause: `for line in code_lines: DECL_RE.finditer(line)` /
       `ASSIGN_RE.finditer(line)` never sees a match whose tokens straddle a
       line boundary, because each line is a separate string. FIX: scan the
       whole (comment/string-stripped) file as ONE string. A regex whitespace
       class already matches a newline character, so `struct scs_mscp_params`
       + newline + `mp` and `mp` + newline + `.cdt =` now match exactly as
       their single-line forms always did. Line numbers for the ACK-adjacency
       check are now recovered from the match offset via a newline-offset
       table, not from list index.
  B-2  TYPEDEF ALIAS. `DECL_RE` only recognizes the literal token sequence
       `struct scs_mscp_params`; a typedef'd alias never enters `names`:
           typedef struct scs_mscp_params mp_params_t;
           mp_params_t mp;
           mp.cdt = &live_cdt;   /* mp never collected -- not flagged */
       FIX: a new `TYPEDEF_RE` finds `typedef struct scs_mscp_params ALIAS;`
       in the same file and, for each alias found, runs a second decl scan
       (`ALIAS ident;` / `ALIAS *ident;`) to fold those identifiers into the
       same `names` set. Single-level only (an alias of an alias is out of
       scope, same class of bound as the pre-existing "completely different
       local variable name" note below) -- re-derive with
       `grep -n typedef.*scs_mscp_params src/vmsscs/scsd.c src/vmsscs/scs_mscp_srv.c`
       if that ever needs to grow.
  B-3  NON-ADJACENT LHS. `ASSIGN_RE` required the identifier immediately
       against `.`/`->` with only whitespace between; an array subscript or a
       parenthesized dereference breaks that adjacency without changing the
       meaning of the write:
           struct scs_mscp_params arr[2];
           arr[0].cdt = &live_cdt;          /* subscript between ident and . */
           struct scs_mscp_params *pmp = &mp;
           (*pmp).cdt = &live_cdt;          /* paren-deref instead of pmp->cdt */
       FIX: `ASSIGN_RE` now tolerates zero or more single-level `[...]`
       subscripts between the identifier and the `.`/`->`; a second regex,
       `PAREN_DEREF_ASSIGN_RE`, matches the `(*ident).cdt =` / `(*ident)->cdt =`
       shape directly. Both still require the captured identifier to be a
       name collected by the declaration scan above, so the scs_poller /
       scs_credit_waiter decoys (M-4) still do not fire, including through an
       array or a paren-deref of THOSE types.

Re-run against scsd.c and scs_mscp_srv.c as they stand today: still 0
failures (no production caller wires a live CDT through any of these shapes
either) -- B-1/B-2/B-3 are gate hardening, not a finding of a live hazard.
Regression coverage for all three, plus the M-1..M-4 battery re-run against
the whole-file scanner, lives in test_scs_mscp_cdt_hazard_bypasses.py
(ctest scs_mscp_cdt_hazard_bypasses).

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
# optional and may repeat on either side of `struct`. Matched against the
# WHOLE file (not line by line) so a declaration split across a newline
# (vms-cf0 B-1) is still found: `\s+` already matches `\n`.
DECL_RE = re.compile(
    r"\b(?:const\s+)?struct\s+scs_mscp_params\s+(?:const\s+)?\*?\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\b"
)

# vms-cf0 B-2: a typedef alias of struct scs_mscp_params, e.g.
# `typedef struct scs_mscp_params mp_params_t;`. Single-level only -- an
# alias of an alias is out of scope, same bound class as the name-typed scope
# note below.
TYPEDEF_RE = re.compile(
    r"\btypedef\s+(?:const\s+)?struct\s+scs_mscp_params\s+(?:const\s+)?\*?\s*"
    r"([A-Za-z_][A-Za-z0-9_]*)\s*;"
)

# A `.cdt =` / `->cdt =` write, captured with the identifier to its left.
# `(?!=)` excludes `==` (a comparison, not a write). vms-cf0 B-1: matched
# against the whole file so a write split across a newline
# (`mp\n    .cdt = x`) is still found. vms-cf0 B-3: tolerates zero or more
# single-level `[...]` subscripts between the identifier and the `.`/`->` so
# `arr[0].cdt = x` (arr declared `struct scs_mscp_params arr[N]`) is still
# found; nested/multi-level subscripts are out of scope (same bound class).
ASSIGN_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\b(?:\s*\[[^\[\]]*\])*\s*(?:\.|->)\s*cdt\b\s*=(?!=)"
)

# vms-cf0 B-3: the `(*ident).cdt = x` / `(*ident)->cdt = x` shape -- a
# parenthesized dereference used instead of `ident->cdt`. Same meaning, not
# caught by ASSIGN_RE because a `)` sits between the identifier and the dot.
PAREN_DEREF_ASSIGN_RE = re.compile(
    r"\(\s*\*\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)\s*(?:\.|->)\s*cdt\b\s*=(?!=)"
)


def strip_comments_and_strings(src):
    """Blank block/line comments and string contents, preserving line count and
    column positions so line numbers and (non-comment) match offsets stay
    accurate -- same technique as test_scsd_send_sites.py."""
    out = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
    out = re.sub(r"//[^\n]*", "", out)
    out = re.sub(r'"(?:\\.|[^"\\])*"', '""', out)
    return out


def _line_offsets(text):
    """Start offset of each line (0-based line index -> char offset), for
    recovering a line number from a whole-file regex match's .start()."""
    offsets = [0]
    for m in re.finditer("\n", text):
        offsets.append(m.end())
    return offsets


def _offset_to_line(offsets, pos):
    """0-based line index containing char offset `pos`, via binary search
    over the line-start offsets from _line_offsets()."""
    import bisect
    return bisect.bisect_right(offsets, pos) - 1


def scan(path):
    """Return (failures, hit_count) for one file."""
    with open(path, encoding="utf-8") as f:
        raw = f.read()
    raw_lines = raw.splitlines()
    code = strip_comments_and_strings(raw)

    # Collect every identifier declared as struct scs_mscp_params, plus (B-2)
    # every identifier declared as a same-file typedef alias of it. Scanned
    # as ONE string (not per line) so a split declaration (B-1) is found.
    names = set(m.group(1) for m in DECL_RE.finditer(code))

    for alias in set(m.group(1) for m in TYPEDEF_RE.finditer(code)):
        alias_decl_re = re.compile(
            r"\b(?:const\s+)?" + re.escape(alias) + r"\s+(?:const\s+)?\*?\s*"
            r"([A-Za-z_][A-Za-z0-9_]*)\b"
        )
        names.update(m.group(1) for m in alias_decl_re.finditer(code))

    offsets = _line_offsets(code)

    matches = []  # (start_offset, identifier)
    for m in ASSIGN_RE.finditer(code):
        matches.append((m.start(), m.group(1)))
    for m in PAREN_DEREF_ASSIGN_RE.finditer(code):
        matches.append((m.start(), m.group(1)))
    matches.sort(key=lambda t: t[0])

    failures = []
    hits = 0
    seen_offsets = set()
    for start, ident in matches:
        if ident not in names:
            continue
        if start in seen_offsets:
            continue
        seen_offsets.add(start)
        hits += 1
        line_idx = _offset_to_line(offsets, start)
        lineno = line_idx + 1
        window = raw_lines[max(0, line_idx - 1): line_idx + 2]  # prev, this, next
        if not any(ACK_TOKEN in w for w in window):
            failures.append(
                f"{os.path.relpath(path, ROOT)}:{lineno}: "
                f"`{ident}.cdt =` (or `->cdt =`) wires a live CDT into "
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
