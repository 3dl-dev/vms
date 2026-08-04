#!/usr/bin/env python3
"""
test_scsd_send_sites.py -- vms-abc: the SEND SITE CENSUS.

WHY IT EXISTS. p. 2-31 says a broken virtual circuit carries no traffic. OVMX
enforces that at ONE choke point, src/vmsscs/scsd.c's send_frame_vc(), which
refuses when the connection's Path Block is not OPEN. That only holds while
every SCS-layer sender actually goes through it -- and the history of this item
is three rounds of review each finding one more sender that did not. A runtime
test can only prove the paths it happens to drive; this proves the SHAPE.

WHAT IT ASSERTS, against src/vmsscs/scsd.c itself:
  1. Every call to send_frame_raw() (the transport, which applies no policy)
     sits inside one of the functions the SEND SITE TABLE names as EXEMPT.
     A new direct caller anywhere else reds this test.
  2. The EXEMPT list in this script and the EXEMPT block of the SEND SITE TABLE
     in scsd.c name the same functions, so the table cannot drift away from what
     is enforced.
  3. send_frame_vc() is the only other caller of send_frame_raw(), i.e. there is
     exactly one choke point and not two.
  4. The choke point still consults SCS_VC_OPEN. (A guard that stopped checking
     would leave 1-3 all green.)

WHAT IT DOES NOT ASSERT: that a choked site is *correct*, or that the exemptions
are *justified*. Those are arguments in the source and measurements in
tests/vmsscs/test_scsd_wire.c (test_a_broken_circuit_carries_no_traffic). This
test only guarantees that a new sender cannot be added silently.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SCSD = os.path.join(ROOT, "src", "vmsscs", "scsd.c")

# The functions permitted to call send_frame_raw() directly, and why. Keep this
# in step with the EXEMPT block of the SEND SITE TABLE in scsd.c -- check 2
# below verifies that both lists name the same functions.
# EVERY EXEMPT FUNCTION MUST CONTAIN NOTHING BUT EXEMPT SENDS. That is the whole
# discipline: exempting scsd_handle_frame() (which is where the HELLO sends used
# to sit inline) measurably let four unguarded SCS senders inside it pass this
# census while transmitting on a CLOSED circuit. send_frame_channel() exists so
# the exemption covers three lines instead of a thousand.
EXEMPT = {
    "send_frame_vc":       "THE choke point: it is what consults the Path Block.",
    "scsd_vc_emit":        "0x41 VC-FORMATION frames (p. 2-14). Three of the four "
                           "formation states are not OPEN and the machine must "
                           "transmit from them, or no circuit could ever open.",
    "send_frame_channel":  "The NISCA directed/padded HELLO replies -- channel "
                           "maintenance BELOW the virtual circuit, and the only "
                           "thing that re-establishes a channel after a break.",
}

# How many send_frame_raw() calls each exempt function is allowed to make. This
# closes the one hole a name-only check leaves: an EXTRA raw send added inside a
# function that is already exempt. scsd_vc_emit() sends the round-2 ACK and the
# round-0/1 START-or-STACK, hence 2; the two wrappers send once each.
EXEMPT_CALLS = {
    "send_frame_vc": 1,
    "send_frame_channel": 1,
    "scsd_vc_emit": 2,
}

failures = []
checks = 0


def check(cond, msg):
    global checks
    checks += 1
    if not cond:
        failures.append(msg)


src = open(SCSD, encoding="utf-8").read()
lines = src.splitlines()

# --- strip comments and string literals so a mention in prose is not a call ---
code = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), src, flags=re.S)
code = re.sub(r"//[^\n]*", "", code)
code = re.sub(r'"(?:\\.|[^"\\])*"', '""', code)
code_lines = code.splitlines()

# --- map every line to its enclosing top-level function ---
# A function definition starts at column 0 with a type and an identifier
# followed by '(' -- the style this file uses throughout.
DEFN = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \*]*?([A-Za-z_][A-Za-z0-9_]*)\s*\(")
#
# A line belongs to a function only from its definition line to the closing
# brace of its body. Lines BETWEEN functions (the doc comments, which were
# blanked to newlines above) belong to nobody -- attributing them to the
# preceding function inflates every span and would make the size check below
# lie.
owner = [None] * len(code_lines)
current = None
depth = 0
started = False
for i, line in enumerate(code_lines):
    if current is None:
        m = DEFN.match(line)
        if m and not line.rstrip().endswith(";"):
            current = m.group(1)
            started = False
    if current is not None:
        owner[i] = current
        depth += line.count("{") - line.count("}")
        if depth > 0:
            started = True
        elif started:
            current = None
            depth = 0
            started = False

# --- check 2: the in-source table names the same exempt functions ---
tbl = re.search(r"SEND SITE TABLE ={5,}\n(.*?)\n \* ={20,}", src, flags=re.S)
check(tbl is not None, "the SEND SITE TABLE comment is gone from scsd.c")

# --- check 1 + 3: who calls send_frame_raw() ---
callers = {}
for i, line in enumerate(code_lines):
    if "send_frame_raw(" not in line:
        continue
    if re.match(r"^static\s+ssize_t\s+send_frame_raw\s*\(", line):
        continue  # the definition itself
    callers.setdefault(owner[i], []).append(i + 1)

check(bool(callers), "no send_frame_raw() call sites found -- the parser is broken, "
                     "not the source")
for fn, at in sorted(callers.items(), key=lambda kv: (kv[0] or "")):
    check(fn in EXEMPT,
          f"{fn or '<file scope>'}() calls send_frame_raw() directly at "
          f"scsd.c line(s) {at}. Every SCS-layer send must go through "
          f"send_frame_vc(), which refuses on a circuit that is not OPEN "
          f"(p. 2-31). If this site genuinely must transmit on a non-OPEN "
          f"circuit, add it to the EXEMPT block of the SEND SITE TABLE in "
          f"scsd.c with a reason, and to EXEMPT in this script.")

check("send_frame_vc" in callers,
      "send_frame_vc() does not call send_frame_raw() -- the choke point does "
      "not reach the transport")

for fn, want in sorted(EXEMPT_CALLS.items()):
    got = len(callers.get(fn, []))
    check(got == want,
          f"{fn}() makes {got} send_frame_raw() call(s), expected {want} "
          f"(lines {callers.get(fn, [])}). An EXTRA raw send inside an already "
          f"exempt function is an unguarded frame that a name-only check would "
          f"miss. If the change is intended, update EXEMPT_CALLS and the "
          f"count in the send_frame_raw() header comment in scsd.c together.")

# --- check 6: the CHOKED half of the table names the right functions. ---
# The number of choked sends is NOT pinned (adding one is safe by construction),
# but WHICH FUNCTIONS send is: a new sending function has to be written into the
# table, which is the moment someone has to think about whether it belongs in
# CHOKED or in EXEMPT.
vc_callers = {}
for i, line in enumerate(code_lines):
    if "send_frame_vc(" not in line:
        continue
    if re.match(r"^static\s+ssize_t\s+send_frame_vc\s*\(", line):
        continue
    if owner[i] == "send_frame_vc":
        continue
    vc_callers.setdefault(owner[i], []).append(i + 1)

check(bool(vc_callers), "nothing goes through the choke point at all")
if tbl:
    # Split on the block HEADINGS, not on the bare words: the prose above the
    # table mentions both "EXEMPT" and "CHOKED".
    body = tbl.group(1).split("\n *   CHOKED (", 1)
    choked_block = body[1].split("\n *   EXEMPT (", 1)[0] if len(body) == 2 else ""
    check(choked_block != "", "the SEND SITE TABLE has no CHOKED block")
    for fn in sorted(vc_callers):
        check(fn is not None and re.search(r"\b" + re.escape(fn or "") + r"\(\)",
                                           choked_block) is not None,
              f"{fn or '<file scope>'}() sends through send_frame_vc() at line(s) "
              f"{vc_callers[fn]} but is not named in the CHOKED half of the SEND "
              f"SITE TABLE in scsd.c. Add it -- the table is the census, and a "
              f"sender missing from it is a sender nobody reviewed.")
    for fn in re.findall(r"^ \*     ([a-z_][a-z0-9_]*)\(\)", choked_block, flags=re.M):
        check(fn in vc_callers,
              f"the CHOKED half of the SEND SITE TABLE names {fn}(), which sends "
              f"nothing through send_frame_vc(). Either it was renamed or its "
              f"send was removed; the table has gone stale.")

print(f"  {sum(len(v) for v in vc_callers.values())} send(s) routed through "
      f"send_frame_vc() from {len(vc_callers)} function(s); "
      f"{sum(len(v) for v in callers.values())} direct transport call(s)")

# --- check 5: an exempt function must be SMALL. ---
# This is the lesson of the measurement above, made mechanical. Exempting a
# function exempts EVERY send inside it, so an exempt function that grows into a
# general-purpose dispatcher silently re-opens the hole. 60 lines is comfortably
# above the largest legitimate one (scsd_vc_emit, which builds and sends the
# three 0x41 formation frames) and far below anything that could hide an
# unrelated sender.
MAX_EXEMPT_LINES = 60
for fn in EXEMPT:
    body = [i for i, o in enumerate(owner) if o == fn]
    check(bool(body), f"{fn}() is named EXEMPT but does not exist in scsd.c")
    if body:
        span = body[-1] - body[0] + 1
        check(span <= MAX_EXEMPT_LINES,
              f"{fn}() is {span} lines long, over the {MAX_EXEMPT_LINES}-line "
              f"limit for an EXEMPT function. Exempting it exempts every send "
              f"inside it -- hoist the exempt sends into their own small helper "
              f"instead of widening the exemption.")

if tbl:
    exempt_block = tbl.group(1).split("\n *   EXEMPT (", 1)
    check(len(exempt_block) == 2, "the SEND SITE TABLE has no EXEMPT block")
    if len(exempt_block) == 2:
        # Only the ENTRY LINES count, not any mention in the surrounding prose:
        # the exempt block's own explanatory paragraph names send_frame_channel(),
        # and matching that let deleting the real entry survive this check.
        named = set(re.findall(r"^ \*     ([a-z_][a-z0-9_]*)\(\)",
                               exempt_block[1], flags=re.M))
        # send_frame_vc is the choke point itself, named in the prose above the
        # table rather than as an EXEMPT entry.
        want = {f for f in EXEMPT if f != "send_frame_vc"}
        check(named == want,
              f"the EXEMPT block of the SEND SITE TABLE lists entries "
              f"{sorted(named)}, but this script exempts {sorted(want)} -- the "
              f"table has drifted from what is enforced")

# --- check 4: the choke point still tests for an OPEN circuit ---
guard = re.search(r"static int scsd_refuse_without_open_vc.*?\n\}", src, flags=re.S)
check(guard is not None, "scsd_refuse_without_open_vc() is gone")
if guard:
    check("SCS_VC_OPEN" in guard.group(0),
          "scsd_refuse_without_open_vc() no longer tests for SCS_VC_OPEN -- it "
          "would let every send through while this census stayed green")
    check("scs_config_path" in guard.group(0),
          "scsd_refuse_without_open_vc() no longer reads the circuit through "
          "CONFIG_PATH (p. 2-47)")

for f in failures:
    print("FAIL " + f, file=sys.stderr)
print(f"test_scsd_send_sites: {checks} checks, {len(failures)} failures")
sys.exit(1 if failures else 0)
