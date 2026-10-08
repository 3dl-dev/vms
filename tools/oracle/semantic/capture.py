#!/usr/bin/env python3
"""capture.py - run a semantic-oracle probe on a REAL OpenVMS lab node and write
its golden transcript (rd vms-8d1).

  capture.py <vax|alpha> <pod> <family> [--login PASSWORD] [--out DIR]

Drives a live ovmx-lab pod's console through its FIFO (never a direct console
connection: AXPbox powers the machine off when a console client disconnects):
generates SP_<FAMILY>.MAR with spgen.py, types it into the node with DCL CREATE,
assembles it with the node's own MACRO, LINKs, RUNs it, and writes the transcript
to docs/oracle/semantics/<family>/<vax73|alpha84>.txt with a provenance header.

The pod must be one you own (spin your own: AGENTS.md lab model), booted to a
DCL prompt; `--login` logs SYSTEM in first (a fresh Alpha clone asks for a new
password: the same one is reused). Typing speed is bounded by the console pump:
the stock alphalab pump sends one line a second -- start the pod with
`srmdrv.py -d 0.15` (see docs/oracle/semantics/README.md) or a probe takes ~15 min.

Clean-room (AGENTS.md Rule 8): the golden is what the real system printed.
"""
import base64
import datetime
import hashlib
import os
import re
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", ".."))
ARCH = {"vax": ("vax1", "vax73", "OpenVMS VAX V7.3 (SIMH MicroVAX 3900)"),
        "alpha": ("alpha1", "alpha84", "OpenVMS Alpha V8.4 (AXPbox ES40)")}

PUSHER = r'''
# Types a file into the console as DCL CREATE input, ONE LINE AT A TIME, waiting
# for the node to ECHO each line before sending the next: an emulated node under
# CPU contention reads its console slowly, and typing ahead of it overflows the
# terminal's type-ahead buffer (SYSTEM-W-DATAOVERUN) or wedges the console.
import os, sys, time
fifo, log, src, spec, eol = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5]
eol = {"cr": "\r", "lf": "\n"}[eol]
def echoed(off, text, limit):
    end = time.time() + limit
    while time.time() < end:
        with open(log, "rb") as f:
            f.seek(off)
            if text.encode("latin-1") in f.read().replace(b"\0", b""):
                return True
        time.sleep(0.05)
    return False
with open(fifo, "w") as f:
    off = os.path.getsize(log)
    f.write("CREATE " + spec + eol); f.flush()
    echoed(off, spec, 30); time.sleep(1)
    for i, ln in enumerate(open(src).read().splitlines()):
        off = os.path.getsize(log)
        f.write(ln + eol); f.flush()
        key = ln.strip()[-24:]
        if key and not echoed(off, key, 60):
            sys.exit("no echo for line %d: %r" % (i + 1, ln))
    time.sleep(1); f.write("\x1a" + ("\n" if eol == "\n" else "")); f.flush(); time.sleep(1)
'''


class Lab:
    def __init__(self, arch, pod):
        self.arch, self.pod = arch, pod
        node = ARCH[arch][0]
        base = "/lab/k8s-labs/%s/logs" % pod if arch == "vax" else "/lab/k8s-labs/%s/%s/logs" % (pod, node)
        self.log, self.fifo = "%s/%s.log" % (base, node), "%s/%s.log.in" % (base, node)
        self.eol = "printf '\\r';" if arch == "vax" else "echo;"

    def kx(self, sh):
        return subprocess.run(["kubectl", "-n", "ovmx-lab", "exec", self.pod, "--", "sh", "-c", sh],
                              capture_output=True).stdout

    def send(self, line):
        b = base64.b64encode(line.encode()).decode()
        self.kx("{ echo %s | base64 -d; %s } > %s" % (b, self.eol, self.fifo))

    def size(self):
        return int(self.kx("stat -c %%s %s" % self.log).strip() or 0)

    def since(self, off):
        return self.kx("tail -c +%d %s" % (off + 1, self.log)).decode("latin-1").replace("\r", "").replace("\x00", "")

    def wait(self, pat, t, off):
        end = time.time() + t
        while time.time() < end:
            s = self.since(off)
            if re.search(pat, s):
                return s
            time.sleep(1.5)
        return None

    def login(self, pw, newpw="semprobe2026"):
        off = self.size(); self.send("")
        s = self.wait(r"Username: *$|\n\$ $", 300, off)
        if s and s.rstrip().endswith("$"):
            return
        off = self.size(); self.send("SYSTEM")
        if not self.wait("Password:", 300, off):
            sys.exit("capture: no Password: prompt")
        time.sleep(1)
        off = self.size(); self.send(pw)
        s = self.wait(r"\n\$ $|New password:", 600, off)
        if s and "New password:" in s:
            # an expired password (a fresh clone of a golden disk) must be
            # changed to a DIFFERENT one (SET-E-PWDNOTDIF)
            off = self.size(); self.send(newpw); self.wait("Verification:", 120, off); time.sleep(1)
            off = self.size(); self.send(newpw); s = self.wait(r"\n\$ $", 600, off)
            print("capture: SYSTEM password on %s changed to %s" % (self.pod, newpw))
        if not s:
            sys.exit("capture: login failed")
        self.cmd("SET TERMINAL/WIDTH=511/PAGE=0/NOWRAP")

    def cmd(self, dcl, t=120):
        mark = "@@E%d" % int(time.time() * 1000)
        off = self.size(); self.send(dcl); time.sleep(0.5); self.send('WRITE SYS$OUTPUT "%s"' % mark)
        s = self.wait(mark + r"\n", t, off)
        if s is None:
            sys.exit("capture: TIMEOUT on %r:\n%s" % (dcl, self.since(off)[-2000:]))
        return s

    def push(self, text, spec):
        with tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False) as f:
            f.write(text); src = f.name
        with tempfile.NamedTemporaryFile("w", suffix=".py", delete=False) as f:
            f.write(PUSHER); pusher = f.name
        for a, b in ((src, "/tmp/sp_src.txt"), (pusher, "/tmp/sp_push.py")):
            subprocess.run(["kubectl", "-n", "ovmx-lab", "cp", a, "%s:%s" % (self.pod, b)], check=True)
        os.unlink(src); os.unlink(pusher)
        n = len(text.splitlines())
        # a typist left running by an interrupted earlier capture would
        # interleave its lines with these ('[s]' keeps pkill off its own shell)
        self.kx("pkill -f '[s]p_push.py'; true")
        off = self.size()
        r = subprocess.run(["kubectl", "-n", "ovmx-lab", "exec", self.pod, "--", "python3", "/tmp/sp_push.py",
                            self.fifo, self.log, "/tmp/sp_src.txt", spec, "cr" if self.arch == "vax" else "lf"],
                           capture_output=True, text=True)
        if r.returncode:
            sys.exit("capture: typing %s failed: %s" % (spec, (r.stderr or r.stdout)[-500:]))
        if not self.wait(r"\*EXIT\*|Exit\n", 180, off):
            sys.exit("capture: CREATE of %s never finished" % spec)
        time.sleep(2)
        out = self.cmd('SEARCH/STATISTICS/NOOUTPUT %s ""' % spec, 60)
        m = re.search(r"Records searched:\s+(\d+)", out)
        if not m or int(m.group(1)) != n:
            sys.exit("capture: %s arrived with %s records, sent %d" % (spec, m and m.group(1), n))


def main(a):
    if len(a) < 4 or a[1] not in ARCH:
        print(__doc__); return 2
    arch, pod, fam = a[1], a[2], a[3]
    pw = a[a.index("--login") + 1] if "--login" in a else None
    outdir = a[a.index("--out") + 1] if "--out" in a else os.path.join(ROOT, "docs", "oracle", "semantics", fam)
    spec = os.path.join(HERE, "specs", fam + ".py")
    mar = subprocess.run([sys.executable, os.path.join(HERE, "spgen.py"), "--mar", spec],
                         capture_output=True, text=True, check=True).stdout
    F = "SP_" + fam.upper()
    lab = Lab(arch, pod)
    if pw:
        lab.login(pw)
    lab.cmd("SET TERMINAL/WIDTH=511/PAGE=0/NOWRAP")
    lab.cmd("SET DEFAULT SYS$LOGIN")
    lab.cmd("DELETE %s.*;*" % F)
    lab.push(mar, F + ".MAR")
    out = lab.cmd("MACRO %s" % F, 600)
    if "-E-" in out or "-F-" in out:
        sys.exit("capture: MACRO failed:\n" + out)
    out = lab.cmd("LINK %s" % F, 300)
    if "-E-" in out or "-F-" in out:
        sys.exit("capture: LINK failed:\n" + out)
    out = lab.cmd("RUN %s" % F, 600)
    lines = out.splitlines()
    try:
        b = lines.index("=== SEMPROBE %s BEGIN ===" % fam)
    except ValueError:
        sys.exit("capture: no transcript:\n" + out)
    t = []
    for ln in lines[b:]:
        t.append(ln)
        if ln in ("=== SEMPROBE %s END ===" % fam, "=== SEMPROBE aborted END ==="):
            break
    sha = hashlib.sha256(open(spec, "rb").read()).hexdigest()
    node, key, desc = ARCH[arch]
    os.makedirs(outdir, exist_ok=True)
    path = os.path.join(outdir, key + ".txt")
    with open(path, "w") as f:
        f.write("# provenance: %s, ovmx-lab pod %s node %s, captured %s by\n" % (
            desc, pod, node.upper(), datetime.date.today().isoformat()))
        f.write("#   tools/oracle/semantic/capture.py: spgen.py --mar, the node's own MACRO + LINK, RUN as SYSTEM.\n")
        f.write("#   Observed output of the real system (clean-room Rule 8); nothing disassembled.\n")
        f.write("# spec: tools/oracle/semantic/specs/%s.py sha256=%s\n" % (fam, sha))
        f.write("\n".join(t) + "\n")
    print("capture: %s (%d cases) -> %s" % (fam, len(t) - 2, os.path.relpath(path, ROOT)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
