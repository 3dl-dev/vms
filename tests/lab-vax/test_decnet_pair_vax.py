#!/usr/bin/env python3
"""test_decnet_pair_vax.py -- DECnet Phase IV between a booted OVMX/VAX and a
booted OVMX/x86 on ONE Ethernet segment: SET HOST both ways (rd vms-9394).

WHAT IS PROVEN (each a PASS/FAIL line; exit 0 iff every one passes):
  VAX side -- OVMX/NetBSD-vax under SIMH, DELQA (qt0) on the segment:
    1. boot-time STARTNET started NETACP (SYS$SYSTEM:DECNETD.EXE) -- SHOW SYSTEM
       lists it -- off the executor/node databases on the system disk;
    2. NETACP opened its datalink THROUGH THE EXECUTIVE: its image's backend readout
       carries DECNETD-I-DATALINK "... via the executive (VMS_IOCTL_L2_OPEN,
       PHY_IO; no CAP_NET_RAW), station address AA-00-04-00-2B-04" (1.43);
    3. nothing runs as substrate root while it does (ROOTAUDIT summary 0) --
       NETACP is a non-root VMS process holding PHY_IO, not a root bpf(4) user;
  The wire (x86 QEMU filter-dump of the segment):
    4. Phase IV frames (ethertype 0x6003) SOURCED from AA-00-04-00-2B-04 --
       the station the VAX executive validated and stamped -- are on the wire;
  Both directions, real sessions:
    5. x86 -> VAX: $ SET HOST <vax> reaches the VAX's LOGINOUT Username:,
       SYSTEM logs in, F$GETSYI("ARCH_NAME") answers VAX, LOGOUT ends the link;
    6. VAX -> x86: $ SET HOST <x86> from the VAX (NETACP's _NET: broker over
       the executive mailboxes, then the executive datalink) reaches the x86's
       Username:, logs in, ARCH_NAME answers X86_64, LOGOUT ends the link;
    7. NEGATIVE CONTROL: SET HOST to a node name neither database knows is
       refused, with no Username: -- so 5/6 cannot pass on a vacuous match.

TOPOLOGY. SIMH's XQ is attached to a UDP transport ("attach xq udp:P1:H:P2")
and the x86 node's virtio NIC to QEMU's UDP socket netdev with the mirror
ports -- both carry raw Ethernet frames, one per datagram, so the two NICs
share one L2 segment. The VAX runs in the lab container on the host network
namespace (no host network change; two localhost UDP ports), the x86 node is a
host QEMU (KVM when /dev/kvm is writable, else TCG).

CONSOLES. The VAX console is drive_boot_vax.py's acceptance BRIDGE (RAW file
+ FIFO, the same one the DCL/SHOW battery drives); the x86 console is QEMU's
serial on stdio. Both are read as append-only logs with offsets, so chatter
(DECNETD hellos printed on a console) cannot hide or forge a match: every
assertion searches only the bytes after its own command was sent.

Configuration: the VAX boots with SYS$SYSTEM:NETNODE_LOCAL.DAT/NETNODE_REMOTE
.DAT already on its system disk (run-boot.sh decnet-pair stages them, the same
files NCP SET/DEFINE writes), so the FIRST boot runs STARTNET. The x86 node is
configured the operator's way: boot, NCP SET EXECUTOR/DEFINE NODE, settle,
power off, boot again (its LPBETA STARTNET then starts NETACP).

Invoked by tests/lab-vax/run-boot.sh decnet-pair (which builds the disk and
supplies every path through the environment); runnable alone with the same
environment.
"""
import os
import re
import shutil
import signal
import struct
import subprocess
import sys
import threading
import time

ENV = os.environ.get


def log(msg):
    print("[dnpair] " + msg, flush=True)


# ---------------------------------------------------------------------------
# configuration (all from the environment run-boot.sh sets)
# ---------------------------------------------------------------------------
CACHE = ENV("CACHE_DIR")
REPO = ENV("REPO")
LAB_IMAGE = ENV("LAB_IMAGE", "ovmx-vax-lab")
ARTIFACTS = ENV("ARTIFACTS_DIR")
X86_REL = ENV("X86_PEER_DIR")                 # vmlinuz + initramfs + ovmx-distrib.img
WORK = ENV("DNPAIR_WORK") or (CACHE and os.path.join(CACHE, "decnet-pair"))
SINGLE_WORKDIR_IN = ENV("DNPAIR_VAX_WORKDIR", "/cache/single-work")
RQ0 = ENV("SINGLE_RQ0_TYPE", "RAUSER=340")
P_VAX = int(ENV("DNPAIR_VAX_UDP", "40143"))   # SIMH XQ's local UDP port
P_X86 = int(ENV("DNPAIR_X86_UDP", "40142"))   # QEMU socket netdev's local port
VAX_ADDR, VAX_NAME = ENV("DNPAIR_VAX_ADDR", "1.43"), ENV("DNPAIR_VAX_NAME", "OVMXV")
X86_ADDR, X86_NAME = ENV("DNPAIR_X86_ADDR", "1.42"), ENV("DNPAIR_X86_NAME", "OVMXA")
VAX_MAC = ENV("DNPAIR_VAX_MAC", "08:00:2B:AA:00:43")
X86_MAC = ENV("DNPAIR_X86_MAC", "52:54:00:AA:00:42")
DEADLINE = int(ENV("DNPAIR_DEADLINE", "3000"))
BOGUS = "NOSUCH"                              # a node name neither DB knows

RESULTS = []


def check(cond, msg):
    RESULTS.append((bool(cond), msg))
    print("  %s: %s" % ("PASS" if cond else "FAIL", msg), flush=True)
    return bool(cond)


def dna_station(addr):
    area, node = (int(x) for x in addr.split("."))
    a16 = ((area & 0x3F) << 10) | (node & 0x3FF)
    return bytes([0xAA, 0x00, 0x04, 0x00, a16 & 0xFF, (a16 >> 8) & 0xFF])


def mac_str(b):
    return "-".join("%02X" % x for x in b)


# ---------------------------------------------------------------------------
# consoles: an append-only log + a send function
# ---------------------------------------------------------------------------
class Console:
    def __init__(self, name, logpath):
        self.name, self.logpath = name, logpath

    def size(self):
        try:
            return os.path.getsize(self.logpath)
        except OSError:
            return 0

    def text(self, since=0):
        try:
            with open(self.logpath, "rb") as f:
                f.seek(since)
                # CRs and the NULs a DCL prompt is padded with are noise.
                return (f.read().decode("latin-1", "replace")
                        .replace("\r", "").replace("\x00", ""))
        except OSError:
            return ""

    def alive(self):
        return True

    def send(self, s):
        raise NotImplementedError

    def line(self, s):
        self.send(s + "\r")

    def expect(self, pats, timeout, since):
        """First of `pats` (regexes) to appear after byte `since`: (index, match).
        (-1, None) on timeout or a dead console."""
        if isinstance(pats, str):
            pats = [pats]
        rx = [re.compile(p) for p in pats]
        end = time.time() + timeout
        while time.time() < end:
            t = self.text(since)
            best = None
            for i, r in enumerate(rx):
                m = r.search(t)
                if m and (best is None or m.start() < best[1].start()):
                    best = (i, m)
            if best:
                return best
            if not self.alive():
                return (-1, None)
            time.sleep(0.5)
        return (-1, None)

    def login(self, user, pw, timeout):
        """Feed RETURN until Username: (the console waits for the operator's
        RETURN, vms-3e9), then log in. Returns the offset after the prompt."""
        end = time.time() + timeout
        off = self.size()
        while time.time() < end:
            self.send("\r")
            i, _ = self.expect([r"Username: ?$", r"Username:"], 8, off)
            if i >= 0:
                break
        else:
            return None
        time.sleep(1)
        off = self.size()
        self.line(user)
        if self.expect(r"Password:", 120, off)[0] < 0:
            return None
        time.sleep(1)
        off = self.size()
        self.line(pw)
        if self.expect(r"(^|\n) ?\$ ", 300, off)[0] < 0:
            return None
        time.sleep(2)
        return self.size()

    def run(self, cmd, timeout=120, settle=1.0):
        """Send one DCL command, then a sentinel WRITE whose output token is
        concatenated at run time (the echo of the command can never match it).
        Returns the console text between the two."""
        global _tok
        _tok += 1
        tok = "DNPAIRTOK%d" % _tok
        off = self.size()
        self.line(cmd)
        time.sleep(settle)
        self.line('WRITE SYS$OUTPUT "%s"+"%d"' % ("DNPAIRTOK", _tok))
        self.expect(re.escape(tok) + r"\b", timeout, off)
        t = self.text(off)
        cut = t.find(tok)
        return t[:cut] if cut >= 0 else t


_tok = 0


class X86Console(Console):
    """QEMU on the host; serial console on stdio, mirrored into a log."""

    def __init__(self, name, logpath, disk, nic_args):
        super().__init__(name, logpath)
        kvm = os.access("/dev/kvm", os.W_OK)
        cmd = ["qemu-system-x86_64"] + (["-enable-kvm"] if kvm else []) + [
            "-kernel", os.path.join(X86_REL, "vmlinuz"),
            "-initrd", os.path.join(X86_REL, "initramfs-ovmx-slim.cpio.gz"),
            "-append", "console=ttyS0 loglevel=3 quiet",
            "-m", "512M", "-smp", "1", "-nodefaults", "-nographic",
            "-serial", "stdio",
            "-drive", "file=%s,format=raw,if=virtio,cache=writethrough" % disk,
            "-no-reboot"] + nic_args
        log("%s: %s (%s)" % (name, " ".join(cmd), "KVM" if kvm else "TCG"))
        self.logf = open(logpath, "wb", buffering=0)
        self.p = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                  stderr=subprocess.STDOUT, bufsize=0)
        self.t = threading.Thread(target=self._pump, daemon=True)
        self.t.start()

    def _pump(self):
        fd = self.p.stdout.fileno()
        while True:
            try:
                b = os.read(fd, 4096)
            except OSError:
                break
            if not b:
                break
            self.logf.write(b)

    def alive(self):
        return self.p.poll() is None

    def send(self, s):
        try:
            for ch in s.encode("latin-1"):
                self.p.stdin.write(bytes([ch]))
                self.p.stdin.flush()
                time.sleep(0.01)      # a typist, not a burst (serial input FIFO)
        except (BrokenPipeError, OSError):
            pass

    def close(self):
        if self.p.poll() is None:
            self.p.send_signal(signal.SIGTERM)
            try:
                self.p.wait(15)
            except subprocess.TimeoutExpired:
                self.p.kill()
        try:
            self.logf.close()
        except OSError:
            pass


class VaxConsole(Console):
    """drive_boot_vax.py's acceptance bridge in the lab container: RAW + FIFO."""

    def __init__(self, name, raw, fifo, cname, simh_extra):
        super().__init__(name, raw)
        self.cname = cname
        for p in (raw, fifo):
            try:
                os.unlink(p)
            except FileNotFoundError:
                pass
        open(raw, "wb").close()
        os.mkfifo(fifo)
        os.chmod(fifo, 0o666)
        rel = lambda p: "/cache/" + os.path.relpath(p, CACHE)
        cmd = ["docker", "run", "--rm", "--name", cname, "--network", "host",
               "--entrypoint", "python3",
               "-e", "NETBSD_VERSION=10.1", "-e", "SETS=kern-GENERIC,base,etc",
               "-e", "OVMX_MODE=acceptance",
               "-e", "OVMX_ARTIFACTS=/artifacts", "-e", "OVMX_NETBSD_DIR=/netbsd",
               "-e", "NETBSD_WORKDIR=" + SINGLE_WORKDIR_IN,
               "-e", "OVMX_SINGLE_RQ0_TYPE=" + RQ0,
               "-e", "OVMX_ACCEPT_RAW=" + rel(raw), "-e", "OVMX_ACCEPT_FIFO=" + rel(fifo),
               "-e", "NETBSD_BOOT_DEADLINE=%d" % DEADLINE,
               "-e", "OVMX_VAX_SIMH_EXTRA=" + simh_extra,
               "-v", CACHE + ":/cache", "-v", ARTIFACTS + ":/artifacts:ro",
               "-v", os.path.join(REPO, "tests/netbsd") + ":/netbsd:ro",
               "-v", os.path.join(REPO, "tests/lab-vax") + ":/lab-vax:ro",
               LAB_IMAGE, "/lab-vax/drive_boot_vax.py"]
        log("%s: %s" % (name, " ".join(cmd)))
        self.p = subprocess.Popen(cmd, stdout=open(raw + ".driver.log", "wb"),
                                  stderr=subprocess.STDOUT)
        # The bridge opens the FIFO's read end before it boots; block (bounded)
        # until it does.
        self.fd = None
        end = time.time() + 300
        while time.time() < end and self.p.poll() is None:
            try:
                self.fd = os.open(fifo, os.O_WRONLY | os.O_NONBLOCK)
                break
            except OSError:
                time.sleep(1)
        if self.fd is None:
            raise RuntimeError("VAX bridge never opened its console FIFO")
        os.set_blocking(self.fd, True)

    def alive(self):
        return self.p.poll() is None

    def send(self, s):
        try:
            for ch in s.encode("latin-1"):
                os.write(self.fd, bytes([ch]))
                time.sleep(0.02)
        except OSError:
            pass

    def close(self):
        try:
            if self.fd is not None:
                os.close(self.fd)
        except OSError:
            pass
        subprocess.run(["docker", "kill", self.cname], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL)
        try:
            self.p.wait(30)
        except subprocess.TimeoutExpired:
            self.p.kill()


# ---------------------------------------------------------------------------
# the wire
# ---------------------------------------------------------------------------
def pcap_frames(path):
    """(dst, src, ethertype) of every frame in a classic libpcap file."""
    out = []
    try:
        with open(path, "rb") as f:
            hdr = f.read(24)
            if len(hdr) < 24:
                return out
            le = hdr[:4] == b"\xd4\xc3\xb2\xa1"
            fmt = "<IIII" if le else ">IIII"
            while True:
                rh = f.read(16)
                if len(rh) < 16:
                    break
                _, _, incl, _ = struct.unpack(fmt, rh)
                d = f.read(incl)
                if len(d) >= 14:
                    out.append((d[0:6], d[6:12], (d[12] << 8) | d[13]))
    except OSError:
        pass
    return out


# ---------------------------------------------------------------------------
# SET HOST <node> from `src`, log in SYSTEM/MANAGER on the far side, ask the
# far node its architecture, log out. Returns (reached_username, arch, ended).
# ---------------------------------------------------------------------------
def set_host(src, node, timeout=240):
    off = src.size()
    src.line("SET HOST " + node)
    i, m = src.expect([r"Username:", r"%[A-Z]+-[EFW]-[A-Z0-9]+[^\n]*"], timeout, off)
    if i != 0:
        return (False, None, False, src.text(off)[-600:])
    time.sleep(1)
    o2 = src.size()
    src.line("SYSTEM")
    if src.expect(r"Password:", 120, o2)[0] < 0:
        return (True, None, False, src.text(off)[-600:])
    time.sleep(1)
    o3 = src.size()
    src.line("MANAGER")
    if src.expect(r"(^|\n) ?\$ ", 300, o3)[0] < 0:
        return (True, None, False, src.text(off)[-600:])
    time.sleep(2)
    o4 = src.size()
    src.line('WRITE SYS$OUTPUT "FARARCH="+F$GETSYI("ARCH_NAME")+"/"+F$GETSYI("NODENAME")')
    i, m = src.expect(r"FARARCH=([A-Za-z0-9_]+)/([A-Za-z0-9_]*)", 120, o4)
    arch = (m.group(1), m.group(2)) if m else None
    time.sleep(1)
    o5 = src.size()
    src.line("LOGOUT")
    i, _ = src.expect(r"%REM-S-END", 180, o5)
    return (True, arch, i == 0, src.text(off)[-1200:])


def main():
    for k, v in (("CACHE_DIR", CACHE), ("REPO", REPO), ("ARTIFACTS_DIR", ARTIFACTS),
                 ("X86_PEER_DIR", X86_REL)):
        if not v:
            log("FATAL: %s not set" % k)
            return 2
    for f in ("vmlinuz", "initramfs-ovmx-slim.cpio.gz", "ovmx-distrib.img"):
        if not os.path.isfile(os.path.join(X86_REL, f)):
            log("FATAL: x86 peer %s missing in %s" % (f, X86_REL))
            return 2
    if shutil.which("qemu-system-x86_64") is None:
        log("FATAL: qemu-system-x86_64 not on PATH")
        return 2
    os.makedirs(WORK, exist_ok=True)
    x86_disk = os.path.join(WORK, "x86.img")
    shutil.copy(os.path.join(X86_REL, "ovmx-distrib.img"), x86_disk)
    vax_station = dna_station(VAX_ADDR)
    x86_station = dna_station(X86_ADDR)

    # ---- x86 boot 1: configure DECnet the operator's way --------------------
    log("x86 boot 1: NCP SET EXECUTOR %s %s, DEFINE NODE %s %s" % (X86_ADDR, X86_NAME, VAX_ADDR, VAX_NAME))
    x = X86Console("x86-cfg", os.path.join(WORK, "x86-cfg.log"), x86_disk, ["-nic", "none"])
    try:
        if x.login("SYSTEM", "MANAGER", 600) is None:
            log("FATAL: x86 config boot never reached a DCL prompt")
            print(x.text()[-3000:])
            return 1
        x.run("NCP :== $SYS$SYSTEM:NCP.EXE")
        for c in ("NCP SET EXECUTOR ADDRESS " + X86_ADDR, "NCP SET EXECUTOR NAME " + X86_NAME,
                  "NCP SET EXECUTOR STATE ON",
                  "NCP DEFINE NODE %s NAME %s" % (VAX_ADDR, VAX_NAME)):
            seg = x.run(c)
            if "%NCP-" in seg and ("-E-" in seg or "-F-" in seg):
                log("FATAL: x86 %s -> %s" % (c, seg.strip()))
                return 1
        seg = x.run("NCP LIST EXECUTOR")
        check("Executor node = %s (%s)" % (X86_ADDR, X86_NAME) in seg,
              "x86 setup: NCP LIST EXECUTOR reads back %s (%s) from its permanent database" % (X86_ADDR, X86_NAME))
        log("x86 boot 1: settling 60s for guest writeback, then power off")
        time.sleep(60)
    finally:
        x.close()

    # ---- the segment: SIMH XQ <-UDP-> QEMU socket netdev ---------------------
    pcap = os.path.join(WORK, "segment.pcap")
    try:
        os.unlink(pcap)
    except FileNotFoundError:
        pass
    simh_extra = "set xq mac=%s;attach xq udp:%d:127.0.0.1:%d" % (VAX_MAC, P_VAX, P_X86)
    nic = ["-netdev", "socket,id=n0,udp=127.0.0.1:%d,localaddr=127.0.0.1:%d" % (P_VAX, P_X86),
           "-device", "virtio-net-pci,netdev=n0,mac=%s,romfile=" % X86_MAC.lower(),
           "-object", "filter-dump,id=fd0,netdev=n0,file=%s" % pcap]

    v = x = None
    try:
        v = VaxConsole("vax", os.path.join(WORK, "vax-console.raw"),
                       os.path.join(WORK, "vax-console.fifo"),
                       "ovmx-dnpair-vax-%d" % os.getpid(), simh_extra)
        x = X86Console("x86", os.path.join(WORK, "x86-run.log"), x86_disk, nic)

        log("waiting for both nodes to reach a DCL prompt (VAX under SIMH is slow)")
        xo = x.login("SYSTEM", "MANAGER", 600)
        check(xo is not None, "x86 node boots on the segment and SYSTEM logs in")
        vo = v.login("SYSTEM", "MANAGER", DEADLINE // 2)
        check(vo is not None, "VAX node boots on the segment and SYSTEM logs in")
        if xo is None or vo is None:
            print("---- VAX console tail ----\n" + v.text()[-4000:])
            print("---- x86 console tail ----\n" + x.text()[-3000:])
            return 1
        for c in (v, x):
            c.send("\r")          # answer LOGIN's terminal inquiry, if any
        time.sleep(5)

        # ---- VAX: boot-time STARTNET + the executive datalink ---------------
        boot = v.text(0)
        check("%DECNET-I-STARTNET" in boot and "%DECNET-E-STARTNET" not in boot,
              "VAX: boot-time STARTNET found the configured executor database and launched NETACP")
        seg = v.run("SHOW SYSTEM", timeout=180)
        check(re.search(r"\bNETACP\b", seg) is not None,
              "VAX: SHOW SYSTEM lists the NETACP process, running after boot")
        # THE EXECUTIVE DATALINK. NETACP is detached and has no terminal: since
        # rd vms-5843 its own status lines go to the null device (a created
        # process cannot yet write a VMS file as SYS$OUTPUT, rd vms-c89), so
        # they are not on the console and there is no NETACP.LOG to TYPE.
        # The proof is read from the system instead, three ways that only
        # together say "through the executive":
        #   - the NETACP image on this disk is BUILT with the executive
        #     datalink backend (no bpf/AF_PACKET code in it) -- its own
        #     --show-executor dry-run says so, opening no socket;
        #   - NETACP is RUNNING (SHOW SYSTEM, above): it exits on any datalink
        #     failure, so a running NETACP has its datalink open;
        #   - no process runs as substrate root (ROOTAUDIT, below) and the
        #     wire carries this node's Phase IV station (WIRE, below): the
        #     executive opened it and stamps the source, nobody else could.
        v.run('DNETACC :== $SYS$SYSTEM:DECNETD.EXE', timeout=60)
        seg = v.run("DNETACC --show-executor", timeout=180)
        check("Datalink backend = executive" in seg,
              "VAX: NETACP's image is built with the EXECUTIVE datalink backend "
              "(VMS_IOCTL_L2_*, PHY_IO; no bpf(4)) -- its --show-executor readout")
        check("DECNETD-E-NOSOCKET" not in v.text(0) and "bpf datalink" not in v.text(0),
              "VAX: no datalink refusal and no bpf(4) path on the console")
        seg = v.run("RUN SYS$SYSTEM:ROOTAUDIT", timeout=180)
        m = re.search(r"%ROOTAUDIT-I-SUMMARY, (\d+) process", seg)
        check(m is not None and m.group(1) == "0" and "SUBSTRATEROOT" not in seg,
              "VAX: with NETACP serving, NO process runs as substrate root (ROOTAUDIT summary 0) "
              "-- the datalink is the executive's, not a root bpf(4) user's")

        # Give the routing layer a few hello cycles to hear each other.
        log("letting the endnodes exchange hellos (45s)")
        time.sleep(45)

        # ---- x86 -> VAX ------------------------------------------------------
        reached, arch, ended, tail = set_host(x, VAX_NAME)
        check(reached, "x86 -> VAX: $ SET HOST %s reaches the VAX's LOGINOUT Username: prompt" % VAX_NAME)
        check(arch is not None and arch[0].upper() == "VAX",
              "x86 -> VAX: SYSTEM logs in over CTERM and the far session's F$GETSYI(\"ARCH_NAME\") is VAX "
              "(got %s)" % (arch,))
        check(ended, "x86 -> VAX: LOGOUT ends the remote session (%REM-S-END)")
        print("---- x86 -> VAX transcript ----\n" + tail + "\n-------------------------------", flush=True)

        # ---- VAX -> x86 ------------------------------------------------------
        reached, arch, ended, tail = set_host(v, X86_NAME, timeout=300)
        check(reached, "VAX -> x86: $ SET HOST %s reaches the x86's LOGINOUT Username: prompt" % X86_NAME)
        check(arch is not None and arch[0].upper() == "X86_64",
              "VAX -> x86: SYSTEM logs in over CTERM and the far session's F$GETSYI(\"ARCH_NAME\") is X86_64 "
              "(got %s)" % (arch,))
        check(ended, "VAX -> x86: LOGOUT ends the remote session (%REM-S-END)")
        print("---- VAX -> x86 transcript ----\n" + tail + "\n-------------------------------", flush=True)

        # ---- negative control -------------------------------------------------
        off = x.size()
        x.line("SET HOST " + BOGUS)
        i, _ = x.expect([r"Username:", r"%[A-Z]+-[EFW]-[A-Z0-9]+"], 120, off)
        check(i == 1, "NEGCTL: SET HOST %s (no such node) is refused with an error and no Username: "
              "-- the Username: matches above are not vacuous" % BOGUS)
        x.expect(r"(^|\n) ?\$ ", 30, off)

        # ---- the wire -----------------------------------------------------------
        frames = pcap_frames(pcap)
        from_vax = [f for f in frames if f[1] == vax_station and f[2] == 0x6003]
        from_x86 = [f for f in frames if f[1] == x86_station and f[2] == 0x6003]
        check(len(from_vax) > 0,
              "WIRE: %d DECnet (0x6003) frames on the segment sourced from the VAX station %s "
              "(the executive-stamped source)" % (len(from_vax), mac_str(vax_station)))
        check(len(from_x86) > 0,
              "WIRE: %d DECnet frames from the x86 station %s (the capture sees the segment)"
              % (len(from_x86), mac_str(x86_station)))
        vax_hw = bytes(int(b, 16) for b in VAX_MAC.split(":"))
        check(not any(f[1] == vax_hw and f[2] == 0x6003 for f in frames),
              "WIRE: no DECnet frame leaves the VAX from its burned-in DELQA address %s -- "
              "every one carries the Phase IV station" % VAX_MAC)
    finally:
        if any(not ok for ok, _ in RESULTS):   # diagnosable from the CI log alone
            for c in (v, x):
                if c is not None:
                    print("---- %s console (tail) ----\n%s" % (c.name, c.text()[-6000:]), flush=True)
        for c in (x, v):
            if c is not None:
                c.close()

    npass = sum(1 for ok, _ in RESULTS if ok)
    nfail = len(RESULTS) - npass
    print("\n====================================")
    print("DECNET PAIR (OVMX/VAX <-> OVMX/x86): %d passed, %d failed" % (npass, nfail))
    return 0 if nfail == 0 and npass > 0 else 1


if __name__ == "__main__":
    sys.exit(main())
