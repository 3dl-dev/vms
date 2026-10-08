#!/usr/bin/env python3
"""ksplay.py - the keystroke oracle player (rd vms-370).

Drives a scripted INTERACTIVE terminal session -- keystrokes with timing -- into
a terminal and records every output byte with a timestamp. The SAME case script
runs against a real OpenVMS console (the ovmx-lab SIMH VAX V7.3 / AXPbox Alpha
V8.4 nodes, OPA0:) and against a booted OVMX (QEMU serial console, OPA0:); the
recorded transcripts are diffed by ksdiff.py.

What it measures is what a person at the keyboard sees: WHEN a typed character
is echoed (on receipt, or only when a read consumes it), what DEL / ^U / ^R / ^X
put on the screen, what ^C ^Y ^O ^T ^S ^Q do to output in flight, how a read
prompt, a timed read and a broadcast arriving mid-read look. Clean-room (AGENTS.md
Rule 8): the goldens are the observed bytes of the real system's terminal.

Pure stdlib; runs inside a lab pod (python3 is there) and inside the ovmx-boot
CI image.

  ksplay.py run   --transport fifo:<console.log>       CASE... --out DIR [--user U --password P]
  ksplay.py run   --transport spawn -- <cmd...>       CASE... --out DIR
  ksplay.py check CASE...                              parse-only (CI lint)

Case scripts: tools/oracle/keystroke/cases/<ID>.ks -- see cases/README for the
language. Every case starts from a logged-in DCL prompt ("$ ") unless it says
`@start loggedout`, and the player returns the session to a prompt after it.

Output per case: <out>/<ID>.ks.txt (the rendered, step-segmented transcript the
diff compares) and <out>/<ID>.timing.jsonl (every in/out byte run with its time
offset -- provenance, not compared).
"""
import hashlib
import json
import os
import re
import select
import shlex
import subprocess
import sys
import threading
import time

HERE = os.path.dirname(os.path.abspath(__file__))
CASES = os.path.join(HERE, "cases")

KEYS = {
    "UP": b"\x1b[A", "DOWN": b"\x1b[B", "RIGHT": b"\x1b[C", "LEFT": b"\x1b[D",
    "CR": b"\r", "LF": b"\n", "DEL": b"\x7f", "ESC": b"\x1b", "TAB": b"\t",
}


def keybytes(s):
    """Case-script key string -> bytes. ^X = control-X, {UP} etc = named keys,
    \\r \\xNN = escapes, ^^ = a literal caret."""
    out, i = bytearray(), 0
    while i < len(s):
        c = s[i]
        if c == "^" and i + 1 < len(s):
            n = s[i + 1]
            if n == "^":
                out += b"^"
            elif n == "?":
                out += b"\x7f"
            else:
                out.append(ord(n.upper()) & 0x1F)
            i += 2
        elif c == "{" and "}" in s[i:]:
            j = s.index("}", i)
            name = s[i + 1:j]
            if name in ("USER", "PASSWORD"):
                out += ("{%s}" % name).encode()       # substituted per target
            elif name not in KEYS:
                raise ValueError("unknown key {%s}" % name)
            else:
                out += KEYS[name]
            i = j + 1
        elif c == "\\" and i + 1 < len(s):
            n = s[i + 1]
            if n == "x":
                out.append(int(s[i + 2:i + 4], 16)); i += 4
            else:
                out += {"r": b"\r", "n": b"\n", "t": b"\t", "\\": b"\\", "e": b"\x1b"}.get(n, n.encode())
                i += 2
        else:
            out += c.encode("latin-1"); i += 1
    return bytes(out)


# --------------------------------------------------------------------------
# case language
# --------------------------------------------------------------------------
# @case ID / @title TEXT / @start loggedin|loggedout / @mask REGEX REPLACEMENT
# STEP-ID ACTION [ARGS] [options]
#   ACTION: send KEYS | type KEYS | wait SECS | login | loginkeys KEYS | logout
#   options: expect=PAT  timeout=SECS  settle=SECS  gap=SECS  quiet  tail=PAT
#            after=SECS (pause before the action)
class Step:
    def __init__(self, sid, action, arg, opts):
        self.sid, self.action, self.arg, self.o = sid, action, arg, opts

    @property
    def quiet(self):
        return "quiet" in self.o


class Case:
    def __init__(self, path):
        self.path = path
        self.cid, self.title, self.start, self.masks, self.steps = None, "", "loggedin", [], []
        self.stream = None
        # sha256 of what is PLAYED (@start + the steps), not of compare-time
        # directives (@title @mask @stream), which may evolve without re-capture
        played = [ln.strip() for ln in open(path, encoding="utf-8")
                  if ln.strip() and not ln.strip().startswith(("#", "@title", "@mask", "@stream"))]
        self.sha = hashlib.sha256("\n".join(played).encode()).hexdigest()
        for n, raw in enumerate(open(path, encoding="utf-8"), 1):
            ln = raw.strip()
            if not ln or ln.startswith("#"):
                continue
            try:
                tok = shlex.split(ln, posix=True)
            except ValueError as e:
                raise SystemExit("%s:%d: %s" % (path, n, e))
            if tok[0] == "@case":
                self.cid = tok[1]
            elif tok[0] == "@title":
                self.title = " ".join(tok[1:])
            elif tok[0] == "@start":
                self.start = tok[1]
            elif tok[0] == "@stream":
                self.stream = re.compile(tok[1])
            elif tok[0] == "@mask":
                self.masks.append((re.compile(tok[1]), tok[2]))
            else:
                sid, action = tok[0], tok[1]
                arg, opts = None, {}
                rest = tok[2:]
                if action in ("send", "type", "loginkeys") and rest:
                    arg, rest = keybytes(rest[0]), rest[1:]
                elif action == "wait" and rest:
                    arg, rest = float(rest[0]), rest[1:]
                elif action not in ("login", "logout", "send", "type", "wait", "loginkeys"):
                    raise SystemExit("%s:%d: unknown action %r" % (path, n, action))
                for r in rest:
                    if "=" in r:
                        k, v = r.split("=", 1)
                        opts[k] = v
                    else:
                        opts[r] = True
                self.steps.append(Step(sid, action, arg, opts))
        if not self.cid or not self.steps:
            raise SystemExit("%s: no @case or no steps" % path)
        if self.start not in ("loggedin", "loggedout"):
            raise SystemExit("%s: @start must be loggedin|loggedout" % path)
        ids = [s.sid for s in self.steps]
        if len(set(ids)) != len(ids):
            raise SystemExit("%s: duplicate step id" % path)


# --------------------------------------------------------------------------
# transports
# --------------------------------------------------------------------------
class Recorder:
    """Shared output buffer + event log; the transport's reader feeds it."""

    def __init__(self):
        self.buf = bytearray()
        self.events = []
        self.t0 = time.monotonic()
        self.lock = threading.Lock()
        self.last = time.monotonic()

    def got(self, data):
        with self.lock:
            self.buf += data
            now = time.monotonic()
            self.last = now
            self.events.append((round(now - self.t0, 4), "out", data.hex()))

    def sent(self, data):
        with self.lock:
            self.events.append((round(time.monotonic() - self.t0, 4), "in", data.hex()))

    def size(self):
        with self.lock:
            return len(self.buf)

    def since(self, off):
        with self.lock:
            return bytes(self.buf[off:])


class FifoTransport:
    """An ovmx-lab node console: write <log>.in (nodedrv/srmdrv FIFO), read
    <log> as it grows. Never opens a second console connection."""

    def __init__(self, log, rec):
        self.log, self.rec = log, rec
        # <log>.raw (nodedrv/srmdrv raw FIFO) passes bytes verbatim; the line
        # FIFO <log>.in turns LF into CR, so ^J cannot be typed through it.
        raw = log + ".raw"
        self.raw = os.path.exists(raw)
        if not self.raw:
            sys.stderr.write("[ksplay] WARNING: no %s -- LF will arrive as CR\n" % raw)
        self.fd = os.open(raw if self.raw else log + ".in", os.O_WRONLY)
        self.off = os.path.getsize(log)
        self.stop = False
        self.th = threading.Thread(target=self._pump, daemon=True)
        self.th.start()

    def _pump(self):
        with open(self.log, "rb") as f:
            f.seek(self.off)
            while not self.stop:
                d = f.read()
                if d:
                    self.rec.got(d)
                else:
                    time.sleep(0.005)

    def write(self, b):
        os.write(self.fd, b)
        self.rec.sent(b)

    def close(self):
        self.stop = True


class SpawnTransport:
    """A child process whose stdin/stdout IS the terminal line (QEMU -serial
    stdio on a booted OVMX)."""

    def __init__(self, argv, rec):
        self.rec, self.argv = rec, argv
        self._start()

    def _start(self):
        self.p = subprocess.Popen(self.argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, bufsize=0)
        self.th = threading.Thread(target=self._pump, args=(self.p,), daemon=True)
        self.th.start()

    def restart(self):
        """Power-cycle the guest (same disk): a case that wedged the line must
        not take every later case down with it."""
        self.close()
        try:
            self.p.wait(30)
        except subprocess.TimeoutExpired:
            pass
        self.rec.got(b"\r\n[ksplay: guest restarted]\r\n")
        self._start()

    def _pump(self, p):
        fd = p.stdout.fileno()
        while True:
            try:
                d = os.read(fd, 4096)
            except OSError:
                break
            if not d:
                break
            self.rec.got(d)

    def write(self, b):
        try:
            self.p.stdin.write(b)
            self.p.stdin.flush()
        except (BrokenPipeError, OSError):
            pass
        self.rec.sent(b)

    def alive(self):
        return self.p.poll() is None

    def close(self):
        try:
            self.p.kill()
        except OSError:
            pass


# --------------------------------------------------------------------------
# player
# --------------------------------------------------------------------------
class Player:
    def __init__(self, tp, rec, user, password, eol=b"\r"):
        self.tp, self.rec, self.user, self.pw, self.eol = tp, rec, user, password, eol

    def wait_for(self, pat, off, timeout):
        rx = re.compile(pat.encode("latin-1"), re.S)
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            if rx.search(self.rec.since(off).replace(b"\0", b"")):
                return True
            time.sleep(0.02)
        return False

    def settle(self, quiet_for, cap):
        """Return once no output has arrived for `quiet_for` seconds (or `cap`),
        counting from now at the earliest."""
        t0 = time.monotonic()
        end = t0 + cap
        while time.monotonic() < end:
            with self.rec.lock:
                idle = time.monotonic() - max(self.rec.last, t0)
            if idle >= quiet_for:
                return
            time.sleep(0.02)

    def keys(self, b, gap):
        if gap <= 0:
            self.tp.write(b)
            return
        for i in range(len(b)):
            self.tp.write(b[i:i + 1])
            time.sleep(gap)

    def prompt(self, timeout=60):
        """Get back to a fresh DCL prompt from wherever a case left the line."""
        for attempt in range(3):
            off = self.rec.size()
            self.tp.write(b"\x15" + self.eol)          # ^U + RETURN: empty command
            if self.wait_for(r"\$ $", off, timeout / 3):
                self.settle(0.5, 5)
                return True
            self.tp.write(b"\x11\x19")                 # ^Q (unpause) ^Y (interrupt)
            time.sleep(1)
        return False

    def login(self, timeout=180, at_username=False):
        if not at_username:
            off = self.rec.size()
            self.tp.write(self.eol)
            if not self.wait_for(r"Username: ?$", off, timeout):
                return False
        off = self.rec.size()
        self.keys(self.user.encode() + self.eol, 0.03)
        if not self.wait_for(r"Password: ?$", off, 60):
            return False
        off = self.rec.size()
        self.keys(self.pw.encode() + self.eol, 0.03)
        if not self.wait_for(r"\n\r?\$ $", off, timeout):
            return False
        self.settle(1.0, 10)
        return True

    def ensure(self, state):
        """Bring the line to @start: a fresh DCL prompt, or logged out."""
        for attempt in range(4):
            off = self.rec.size()
            self.tp.write(b"\x15" + self.eol)
            end = time.monotonic() + float(os.environ.get("KS_ENSURE_WAIT", "20"))
            seen = None
            while time.monotonic() < end and not seen:
                seg = self.rec.since(off).replace(b"\0", b"")
                if re.search(rb"Username: ?$", seg):
                    seen = "user"
                elif re.search(rb"\$ $", seg):
                    seen = "dcl"
                else:
                    time.sleep(0.05)
            if seen is None:
                self.tp.write(b"\x11\x19")          # ^Q (unpause) ^Y (interrupt)
                time.sleep(2)
                continue
            if state == "loggedin":
                if seen == "dcl":
                    self.settle(0.8, 5)
                    return True
                if self.login(at_username=True):
                    return True
                continue
            # loggedout
            if seen == "user":
                self.tp.write(b"\x15")
                time.sleep(0.5)
                self.tp.write(b"\x1a")                # ^Z abandons a login prompt
                self.wait_for(r"logged out|Error reading", self.rec.size(), 30)
                self.settle(3.0, 60)
                return True
            off = self.rec.size()
            self.tp.write(b"LOGOUT" + self.eol)
            ok = self.wait_for(r"logged out at", off, 60)
            self.settle(3.0, 60)
            return ok
        return False

    def subst(self, b):
        return b.replace(b"{USER}", self.user.encode()).replace(b"{PASSWORD}", self.pw.encode())

    def run_step(self, st):
        o = st.o
        if "after" in o:
            time.sleep(float(o["after"]))
        off = self.rec.size()
        t_start = time.monotonic()
        gap = float(o.get("gap", 0.05))
        if st.action == "send":
            self.tp.write(self.subst(st.arg))
        elif st.action == "type":
            self.keys(self.subst(st.arg), gap)
        elif st.action == "wait":
            time.sleep(st.arg)
        elif st.action == "login":
            self.login()
        elif st.action == "loginkeys":
            self.keys(st.arg, gap)
        elif st.action == "logout":
            self.tp.write(b"LOGOUT" + self.eol)
        if "expect" in o:
            ok = self.wait_for(o["expect"], off, float(o.get("timeout", 30)))
            if not ok:
                sys.stderr.write("  [%s] expect %r timed out\n" % (st.sid, o["expect"]))
        settle = float(o.get("settle", 1.5))
        if settle > 0:
            self.settle(settle, float(o.get("cap", 30)))
        return off, round(t_start - self.rec.t0, 4)


def render(b):
    """Output bytes -> the visible, diffable text form. Printables are
    themselves; CR LF ESC BEL BS NUL DEL and other controls are <NAMES>; a line
    break follows every LF so the file reads like the screen."""
    names = {0x0d: "<CR>", 0x0a: "<LF>\n", 0x1b: "<ESC>", 0x07: "<BEL>",
             0x08: "<BS>", 0x00: "<NUL>", 0x7f: "<DEL>", 0x09: "<TAB>"}
    out = []
    for c in b:
        if c in names:
            out.append(names[c])
        elif 0x20 <= c < 0x7f:
            out.append(chr(c))
        else:
            out.append("<%02X>" % c)
    return "".join(out)


def apply_masks(text, masks):
    for rx, rep in masks:
        text = rx.sub(rep, text)
    return text


def normalize(case, lines):
    """The SYMMETRIC mask, applied by ksdiff.py to the golden and to OVMX alike:
    the case's @mask substitutions (volatile fields: times, PIDs, node names),
    then @stream REGEX -- every line of a steady output stream (a line the
    regex finds, complete or cut off by the step boundary) becomes "<stream>"
    and a run of them collapses to one, so how MANY stream lines fell in a
    window (timing, not behavior) never decides a step. Transcripts on disk
    stay unmasked."""
    out = []
    for ln in lines:
        ln = apply_masks(ln, case.masks)
        if case.stream is not None and case.stream.search(ln):
            if out and out[-1] == "<stream>":
                continue
            ln = "<stream>"
        out.append(ln)
    return out


def boot_wait(tp, rec, player, secs):
    """Wait (pressing RETURN) for the console to offer Username:."""
    end = time.monotonic() + secs
    while time.monotonic() < end:
        off = rec.size()
        tp.write(player.eol)
        if player.wait_for(r"Username: ?$", off, 3):
            return True
    return False


def play_case(case, tp, rec, player, outdir, label):
    ok = player.ensure(case.start)
    if not ok and hasattr(tp, "restart"):
        sys.stderr.write("  [%s] line wedged -- restarting the guest\n" % case.cid)
        tp.restart()
        boot_wait(tp, rec, player, player.boot_secs)
        ok = player.ensure(case.start)
    lines = ["# keystroke oracle transcript (unmasked) -- case %s (%s)" % (case.cid, label),
             "# %s" % case.title,
             "# case-sha256 %s" % case.sha,
             "@start %s %s" % (case.start, "OK" if ok else "FAILED")]
    offs = []
    for st in case.steps:
        off, t = player.run_step(st)
        offs.append(off)
    offs.append(rec.size())
    carry = None
    for k, st in enumerate(case.steps):
        if "merge" in st.o:                 # reported with the next step
            carry = offs[k] if carry is None else carry
            lines.append("=== STEP %s (merged into next)" % st.sid)
            continue
        a = offs[k] if carry is None else carry
        carry = None
        seg = rec.since(a)[:offs[k + 1] - a]
        if "tail" in st.o:
            i = seg.find(keybytes(st.o["tail"]))
            seg = seg[i:] if i >= 0 else b""
            hdr = "=== STEP %s (from %s)" % (st.sid, st.o["tail"])
        else:
            hdr = "=== STEP %s" % st.sid
        if st.quiet:
            lines.append(hdr + " quiet")
            continue
        lines.append(hdr)
        for ln in render(seg).split("\n"):
            if ln:
                lines.append("| " + ln)
    with open(os.path.join(outdir, case.cid + ".ks.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    return ok


def main(argv):
    if not argv or argv[0] not in ("run", "check"):
        sys.exit(__doc__)
    cmd, argv = argv[0], argv[1:]
    spawn = None
    if "--" in argv:
        i = argv.index("--")
        spawn, argv = argv[i + 1:], argv[:i]
    opts, cases = {}, []
    it = iter(argv)
    for a in it:
        if a.startswith("--"):
            opts[a[2:]] = next(it)
        else:
            cases.append(a)
    if not cases:
        cases = sorted(os.path.join(CASES, f) for f in os.listdir(CASES) if f.endswith(".ks"))
    parsed = [Case(c if os.path.exists(c) else os.path.join(CASES, c + ".ks")) for c in cases]
    if cmd == "check":
        for c in parsed:
            print("ok %s (%d steps)" % (c.cid, len(c.steps)))
        return 0
    outdir = opts.get("out", "ks-out")
    os.makedirs(outdir, exist_ok=True)
    rec = Recorder()
    tr = opts.get("transport", "spawn")
    if tr.startswith("fifo:"):
        tp = FifoTransport(tr[5:], rec)
    elif tr == "spawn":
        if not spawn:
            sys.exit("ksplay: spawn transport needs -- <cmd...>")
        tp = SpawnTransport(spawn, rec)
    else:
        sys.exit("ksplay: unknown transport %r" % tr)
    eol = b"\n" if opts.get("eol") == "lf" else b"\r"
    player = Player(tp, rec, opts.get("user", "SYSTEM"), opts.get("password", "MANAGER"), eol)
    label = opts.get("label", tr)
    player.boot_secs = float(opts.get("boot-wait", 180))
    if "boot-wait" in opts:
        boot_wait(tp, rec, player, player.boot_secs)
    bad = 0
    for c in parsed:
        n0 = len(rec.events)
        sys.stderr.write("[ksplay] %s\n" % c.cid)
        if not play_case(c, tp, rec, player, outdir, label):
            sys.stderr.write("  [%s] could not reach @start %s\n" % (c.cid, c.start))
            bad += 1
        with open(os.path.join(outdir, c.cid + ".timing.jsonl"), "w") as f:
            for ev in rec.events[n0:]:
                f.write(json.dumps(ev) + "\n")
    with open(os.path.join(outdir, "_session.raw"), "wb") as f:
        f.write(bytes(rec.buf))
    tp.close()
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
