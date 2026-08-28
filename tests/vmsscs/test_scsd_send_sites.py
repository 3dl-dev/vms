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
  0. THE TRANSMIT PRIMITIVE ITSELF is confined. Pre-vms-838: the only
     function in scsd.c containing a sendto/send/sendmsg/sendmmsg/write/
     writev call was send_frame_raw(), exactly once. vms-838 moved that
     primitive into src/vmsscs/scs_datalink.c (a Linux AF_PACKET backend and
     a NetBSD bpf(4) backend behind one header, since NetBSD -- the vax
     substrate -- has no AF_PACKET) so send_frame_raw() now calls
     scs_datalink_send() instead of holding the syscall itself. Check 0
     therefore now asserts TWO things: scsd.c contains ZERO transmit
     primitives (the abstraction is not bypassed), and scs_datalink.c
     contains exactly one in each of its two named backend functions
     (datalink_send_linux(), datalink_send_netbsd()) -- the same "one named
     function owns the syscall" discipline, one file further out. This is
     check 0 because checks 1-4 all key on the WRAPPER NAMES, and a raw
     socket call is invisible to a name check by construction.
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
  7. (vms-561) NO SITE BUILDS A CONNECTION-CONTROL FRAME BY HAND: the four
     connect/accept frame builders are called only from the three service
     emitters, those emitters all still build something, and scsd.c still
     reaches scs_connect()/scs_accept(). A re-added open-coded CONNECT-RESPONSE
     would otherwise keep every runtime test green while putting a connection on
     the wire that no CDT describes.

WHY CHECK 0 EXISTS, measured. Without it this census made a completeness claim
it could not support: main()'s HELLO beacon loop called sendto() on the
AF_PACKET socket directly -- a real send site (it increments rx.hello_sent and
is printed in the exit summary) that appeared in NEITHER half of the SEND SITE
TABLE, and that this script could not see because both of its attribution passes
keyed on the literals "send_frame_raw(" and "send_frame_vc(". The beacon now
goes through send_frame_channel(); check 0 is what makes the next one impossible
to add silently, wrapper or no wrapper.

CHECKS 0 AND 7 ARE PROVEN BY MUTATION, not asserted. Seven mutants applied to
scsd.c, each run against this script alone, each restored and the restore
verified with cmp:

  M-A  a stray sendto() added to main()'s timer loop          RED (killed)
  M-B  a stray write() in a new helper beside scsd_handle_frame  RED
  M-C  a SECOND sendto() added inside send_frame_raw itself   RED (count check)
  M-D  THE ORIGINAL DEFECT restored -- the beacon rebuilt to
       call sendto() on `sock` directly, exactly as it stood
       before this round                                      RED
  M-F  sendto taken as a VALUE (`... (*p)(...) = sendto;`) and
       never called by name -- the reason check 0 matches a
       bare identifier rather than `name(`                    RED
  M-J  a new function quietly calling send_frame_channel(),
       i.e. taking the HELLO exemption without being named    RED (check 7)
  M-E  control for the pre-existing table check: the
       send_frame_channel entry renamed out of the EXEMPT
       block                                                  RED

Zero survivors. M-D is the one that matters: this census was GREEN over that
source before check 0 existed.

WHAT IT DOES NOT ASSERT: that a choked site is *correct*, or that the exemptions
are *justified*. Those are arguments in the source and measurements in
tests/vmsscs/test_scsd_wire.c (test_a_broken_circuit_carries_no_traffic). This
test only guarantees that a new sender cannot be added silently.

THE BOUND ON 'EVERY', stated so the claim does not outrun the evidence: check 0
enumerates senders in THIS FILE, which is the only one in src/vmsscs/ that has a
socket to send on. Re-derive it with

    grep -n 'socket\\|sendto\\|sendmsg\\|AF_PACKET\\|sockaddr' src/vmsscs/*.c \\
        src/vmsscs/include/*.h

-- as of this round every hit outside scsd.c is inside a COMMENT (scs_depart.c,
scs_config.c and scs_conn.h each say they open no socket; scs_member.c quotes
lab capture labels). The sibling modules are pure frame builders and state
machines. If one is ever handed an fd, this census must grow to cover it.
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
SCSD = os.path.join(ROOT, "src", "vmsscs", "scsd.c")
# vms-838: the raw-L2 transport backend (AF_PACKET on Linux, bpf(4) on
# NetBSD) moved out of scsd.c into its own file, scs_datalink.c, so it can
# be shared with a future DECnet Phase IV sender (rd vms-30e) instead of
# being scsd-specific. That is exactly the primitive this census's check 0
# exists to keep confined -- see its updated header below for what changed
# and why the shape (one choke point, now one hop further out) is
# unchanged.
DATALINK = os.path.join(ROOT, "src", "vmsscs", "scs_datalink.c")

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
    "send_frame_channel":  "ALL NISCA HELLO traffic -- the directed/padded "
                           "replies AND main()'s periodic multicast beacon. "
                           "Channel maintenance BELOW the virtual circuit; the "
                           "only thing that re-establishes a channel after a "
                           "break, and (the beacon) how peers are discovered "
                           "before any Path Block exists at all.",
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
def function_owner_map(code_lines):
    """Map every (stripped) source line to its enclosing top-level function.
    A function definition starts at column 0 with a type and an identifier
    followed by '(' -- the style both scsd.c and scs_datalink.c use
    throughout. A line belongs to a function only from its definition line
    to the closing brace of its body. Lines BETWEEN functions (the doc
    comments, which were blanked to newlines by the caller) belong to
    nobody -- attributing them to the preceding function inflates every
    span and would make a size check lie."""
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
    return owner


owner = function_owner_map(code_lines)

# --- vms-838: the datalink backend file, parsed the same way ---
dl_src = open(DATALINK, encoding="utf-8").read()
dl_code = re.sub(r"/\*.*?\*/", lambda m: "\n" * m.group(0).count("\n"), dl_src, flags=re.S)
dl_code = re.sub(r"//[^\n]*", "", dl_code)
dl_code = re.sub(r'"(?:\\.|[^"\\])*"', '""', dl_code)
dl_code_lines = dl_code.splitlines()
dl_owner = function_owner_map(dl_code_lines)

# --- check 0: the transmit primitive is confined to ONE function ---
# Checks 1, 3 and 6 attribute calls to the WRAPPER NAMES. That is exactly the
# hole a raw socket call walks through, so this pass keys on the syscall
# wrappers themselves instead. Everything that can put a byte on the wire
# is listed; if a future one is added to libc, add it here.
#
# MATCHED AS A BARE IDENTIFIER, not as `name(`, so that TAKING one of these as a
# value -- `= sendto;`, `&sendmsg`, stashing it in a dispatch table -- is caught
# too. Comments and string literals were blanked above, so a mention in prose
# does not trip it. A future local named `send` would red here; renaming
# it (or, if it really transmits, entering it in the SEND SITE TABLE) is the
# intended response -- do NOT loosen the regex to make it pass.
TRANSMIT_PRIMITIVES = ("sendto", "sendmsg", "sendmmsg", "send",
                       "writev", "pwritev", "pwrite", "write", "syscall")
PRIM_RE = re.compile(r"\b(" + "|".join(TRANSMIT_PRIMITIVES) + r")\b")

# vms-838: THE PRIMITIVE MOVED. Before this item, scsd.c opened the AF_PACKET
# socket itself and send_frame_raw() called sendto() on it directly, so the
# primitive scan below (and the "must contain exactly 1" invariant) ran
# entirely against scsd.c. NetBSD (the vax substrate SCSD.EXE now also ships
# on, closing rd vms-e1d's last parity-drift image) has no AF_PACKET -- its
# raw-link facility is bpf(4), a materially different API -- so the actual
# platform primitive (sendto() on Linux, write() on NetBSD's bpf) now lives
# in src/vmsscs/scs_datalink.c instead, behind a header scsd.c's
# send_frame_raw() calls into (scs_datalink_send()). Two consequences,
# BOTH ENFORCED BELOW rather than merely asserted here:
#   1. scsd.c itself must contain ZERO transmit primitives -- not "exactly
#      1 inside send_frame_raw()" anymore. A raw sendto()/write() appearing
#      ANYWHERE in scsd.c, including inside send_frame_raw(), means the
#      abstraction was bypassed and this reds.
#   2. scs_datalink.c must contain the primitive(s), each confined to its
#      own per-backend function (datalink_send_linux/datalink_send_netbsd),
#      the same "one named function owns the syscall" discipline check 0
#      always enforced, just applied one file further out. Both backends'
#      source text is always present (the actual platform selection is a
#      compile-time #ifdef __linux__/__NetBSD__ inside scs_datalink.c), so
#      a plain per-file text scan -- which does not run the preprocessor,
#      same as the scsd.c scan never did -- legitimately finds both.
# scsd.c's own owner map (SCSD_PRIMITIVE_OWNERS) is intentionally gone: since
# vms-838 the correct count there is zero, unconditionally -- see the `check`
# on `prim_sites` just below, which asserts that directly rather than via an
# owners table (an empty table and a table asserting "zero of everything" are
# the same thing; the direct assertion says so without one).
DATALINK_PRIMITIVE_OWNERS = {
    "datalink_send_linux": 1,   # sendto() -- the pre-vms-838 scsd.c body, moved.
    "datalink_send_netbsd": 1,  # write() -- bpf(4) has no per-packet destination
                                # address, so no sockaddr_ll-equivalent is built.
}


def scan_primitives(code_lines, owner):
    sites = {}
    for i, line in enumerate(code_lines):
        n = len(PRIM_RE.findall(line))
        if n:
            sites.setdefault(owner[i], []).extend([i + 1] * n)
    return sites


prim_sites = scan_primitives(code_lines, owner)
dl_prim_sites = scan_primitives(dl_code_lines, dl_owner)

check(not prim_sites,
      f"scsd.c contains a transmit primitive directly ({prim_sites}) -- since "
      f"vms-838 the raw send lives in scs_datalink.c behind scs_datalink_send(), "
      f"not in this file. A primitive found here means the abstraction was "
      f"bypassed (e.g. a stray sendto()/write() added back inside "
      f"send_frame_raw() or elsewhere).")

check(bool(dl_prim_sites),
      "no transmit primitive found anywhere in scs_datalink.c -- the primitive "
      "scan is broken, not the source (datalink_send_linux()/"
      "datalink_send_netbsd() must each contain one)")
for fn, at in sorted(dl_prim_sites.items(), key=lambda kv: (kv[0] or "")):
    which = sorted({p for a in set(at) for p in PRIM_RE.findall(dl_code_lines[a - 1])})
    check(fn in DATALINK_PRIMITIVE_OWNERS,
          f"{fn or '<file scope>'}() names a transmit primitive "
          f"({'/'.join(which)}) "
          f"DIRECTLY at scs_datalink.c line(s) {at}, outside the two backend "
          f"send functions. Route it through datalink_send_linux()/"
          f"datalink_send_netbsd(), or -- if this is deliberately a new "
          f"backend -- add it to DATALINK_PRIMITIVE_OWNERS here.")
for fn, want in sorted(DATALINK_PRIMITIVE_OWNERS.items()):
    got = len(dl_prim_sites.get(fn, []))
    check(got == want,
          f"{fn}() contains {got} transmit primitive call(s) in scs_datalink.c, "
          f"expected {want} (lines {dl_prim_sites.get(fn, [])}). An EXTRA raw "
          f"send inside a backend transport function is still an unenumerated "
          f"sender. If the change is intended, update DATALINK_PRIMITIVE_OWNERS.")

print(f"  scsd.c: 0 transmit primitive call(s) (moved to scs_datalink.c, vms-838)")
print(f"  scs_datalink.c: {sum(len(v) for v in dl_prim_sites.values())} transmit "
      f"primitive call(s) in {len(dl_prim_sites)} function(s): "
      f"{', '.join(sorted(k or '<file scope>' for k in dl_prim_sites))}")

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

# --- check 7: the CALL SITES of the HELLO exemption are pinned too ---
# send_frame_channel() is exempt as a FUNCTION, so every one of its callers
# inherits the exemption. Check 1 cannot see that set grow -- it only sees
# send_frame_channel() itself. This pins who may take the HELLO exemption and
# how often, and it is what makes the "6 sites in 3 functions" figure in the
# EXEMPT entry of the SEND SITE TABLE a measurement rather than a claim.
CHANNEL_CALLERS = {
    "scsd_handle_frame": 4,       # padded-probe b4 ack, rate-limited directed
                                  # reply, one-shot proactive padded HELLO, and
                                  # (vms-f3e) the OVMX_MCAST_SOLICIT member-first
                                  # directed HELLO (kill-switch-gated)
    "scsd_hello_beacon_emit": 1,  # the periodic multicast beacon off main()'s
                                  # timer loop
    "scsd_emit_port_lastgasp": 1, # vms-708 (spec 4(O.30)): the port-level
                                  # clean-leave last gasp -- one final multicast
                                  # HELLO (abs-30 b1 + cluster nonce) at shutdown
}
chan_callers = {}
for i, line in enumerate(code_lines):
    if "send_frame_channel(" not in line:
        continue
    if re.match(r"^static\s+ssize_t\s+send_frame_channel\s*\(", line):
        continue
    if owner[i] == "send_frame_channel":
        continue
    chan_callers.setdefault(owner[i], []).append(i + 1)

check(set(chan_callers) == set(CHANNEL_CALLERS),
      f"send_frame_channel() is called from {sorted(k or '<file scope>' for k in chan_callers)}, "
      f"expected {sorted(CHANNEL_CALLERS)}. Calling it takes the HELLO exemption "
      f"-- 'this frame rides no virtual circuit'. A new caller must be justified "
      f"in the EXEMPT half of the SEND SITE TABLE and added here; if what it "
      f"sends is not a HELLO it belongs at send_frame_vc() instead.")
for fn, want in sorted(CHANNEL_CALLERS.items()):
    got = len(chan_callers.get(fn, []))
    check(got == want,
          f"{fn}() makes {got} send_frame_channel() call(s), expected {want} "
          f"(lines {chan_callers.get(fn, [])}). Update CHANNEL_CALLERS and the "
          f"per-function counts in the send_frame_channel() entry of the SEND "
          f"SITE TABLE together.")

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

# --- check 7 (vms-561): NO SITE BUILDS A CONNECTION-CONTROL FRAME BY HAND. ---
#
# The five SCS services (src/vmsscs/scs_svc.c) own connection formation. scsd.c
# is the port driver: it builds the frames, but only when a service asks it to,
# from the three emitters the SEND SITE TABLE names. That is a SHAPE claim of
# exactly the kind a runtime test cannot make -- a re-added open-coded
# CONNECT-RESPONSE somewhere else would keep every existing test green while
# putting a connection on the wire that no CDT describes and no state machine
# ever saw.
#
# So: the four connection-control frame BUILDERS may be called only from the
# emitters, and the emitters may be called only by name (they are handed to
# scs_connect/scs_accept as args.emit, never invoked directly).
CONN_BUILDERS = (
    "scs_connect_build_request",
    "scs_connect_build_response",
    "scs_dir_build_connect_echo",
    "scs_dir_build_connect_response",
)
#
# vms-7fe adds a FOURTH permitted caller, and it is not an emitter. The rule
# this check enforces is "no hand-built frame may put A CONNECTION on the wire
# that no CDT describes". scsd_send_sdir_refusal() builds the 66-byte
# CONNECT_RSP that DECLINES a connection -- p. 2-48's "no such SYSAP" and,
# in principle, p. 2-50's "busy ... try again later" (which scsd.c never asks
# it for: the daemon answers synchronously and cannot produce a busy listener --
# scs_sdir.h DESIGN CHOICE 3, measured by test_scsd_wire.c's end-of-run busy
# total). There is no CDT
# to allocate because the
# p. 2-48 SDIR scan failed before any SYSAP saw the request, and no Figure 2-14
# transition to take; routing it through scs_reject() would have made the
# service ask for SEND_REJECT_REQ while the emitter built a CONNECT_RSP. It is
# named here rather than exempted so that it, too, cannot be added silently.
BUILDER_CALLERS = {
    "scsd_svc_emit_connect_req",
    "scsd_svc_emit_dir_accept",
    "scsd_svc_emit_member_accept",
    "scsd_send_sdir_refusal",
}
builder_sites = {}
for i, line in enumerate(code_lines):
    for b in CONN_BUILDERS:
        if re.search(r"\b" + b + r"\s*\(", line):
            builder_sites.setdefault(owner[i], []).append((i + 1, b))

check(bool(builder_sites),
      "no connection-control frame builder is called anywhere in scsd.c -- the "
      "builder scan is broken, not the source")
for fn, at in sorted(builder_sites.items(), key=lambda kv: (kv[0] or "")):
    check(fn in BUILDER_CALLERS,
          f"{fn or '<file scope>'}() builds a connection-control frame by hand "
          f"at scsd.c line(s) {[a for a, _ in at]} ({sorted({b for _, b in at})}). "
          f"vms-561 requires connection formation to go through the SCS "
          f"services: allocate through scs_connect()/scs_accept() and build the "
          f"frame in the emitter they call. A hand-built connect/accept frame "
          f"puts a connection on the wire that no CDT describes.")
for fn in sorted(BUILDER_CALLERS):
    check(fn in builder_sites,
          f"{fn}() builds no connection-control frame -- it was renamed or "
          f"gutted, and this check has gone stale rather than passing")

# The services must actually be reached. A migration that deleted the calls
# would leave every check above green with nothing driving them.
for svc in ("scs_connect", "scs_accept"):
    n = sum(1 for line in code_lines if re.search(r"\b" + svc + r"\s*\(\s*scsd_svc\(\)", line))
    check(n > 0, f"nothing in scsd.c calls {svc}() -- connection formation is "
                 f"no longer going through the SCS services")

# ---------------------------------------------------------------------------
# 8. (vms-096) THE DEAD op6 BLOCK STAYS DEAD -- the negative control for a
#    deletion.
#
# A `if (cm_op == 6 && cm_rc == SCS_DIR_OVMX_CONID)` block sat INSIDE
# `if (cm_op == 8 && ...)`, where cm_op is 8 by construction. It was
# unreachable, and it carried the only call of scs_send_disconnect(), the only
# write to ps->psc_credit_done and one of the two disk-discovery triggers -- all
# of which therefore read as live code in every review and were not.
#
# Message type 6 IS the DISCONNECT_REQUEST of spec sec 4(h)(1a). Since vms-591
# it is answered by scs_disc_build_response(), driven off the CDT by the vms-dd5
# classifier -- which is why the credit-handshake region has no business
# comparing cm_op against 6 at all. Any reappearance of that comparison is this
# defect coming back, whether nested or not, so the check is on the comparison
# and not on the nesting.
dead_op6 = [i + 1 for i, line in enumerate(code_lines)
            if re.search(r"\bcm_op\s*==\s*6\b", line)]
check(not dead_op6,
      f"scsd.c compares cm_op against 6 at line(s) {dead_op6}. vms-096 deleted "
      f"exactly such a block: it was nested inside `if (cm_op == 8)` and could "
      f"never be true, and it was the only caller of scs_send_disconnect() and "
      f"the only writer of psc_credit_done. Message type 6 is DISCONNECT_REQ and "
      f"belongs to the vms-591 builders via the vms-dd5 classifier, not to the "
      f"credit-handshake branch.")

# And the symbols it kept alive must be gone, not merely uncalled -- an
# uncalled static function is what made the block look load-bearing.
for gone, why in (
        ("scs_send_disconnect", "the hand-built peer-directed op-6 teardown; "
                                "scs_disc_build_response() is the architected reply"),
        ("psc_credit_done", "a flag whose only writer was the unreachable block, "
                            "so it was structurally always 0")):
    hits = [i + 1 for i, line in enumerate(code_lines)
            if re.search(r"\b" + gone + r"\b", line)]
    check(not hits,
          f"scsd.c still uses `{gone}` at line(s) {hits} -- {why}. It was deleted "
          f"by vms-096 as proven-dead; re-adding it needs a live call path and a "
          f"test, not a re-declaration.")

for f in failures:
    print("FAIL " + f, file=sys.stderr)
print(f"test_scsd_send_sites: {checks} checks, {len(failures)} failures")
sys.exit(1 if failures else 0)
