#!/usr/bin/env python3
"""mk_sysgen.py [--force] <out.dat> <SCSNODE> <SCSSYSTEMID> [template.dat]
       mk_sysgen.py --alloc <prefix> [registry-dir]

x86_64 replacement for tools/mk_sysgen, which is an aarch64 ELF binary with no
surviving source and therefore does not run on the workshop dev host
(vms-2f3 bring-up 2026-08-02).

IDENTITY UNIQUENESS IS ENFORCED HERE (vms-1ae). SCSNODE and SCSSYSTEMID are
cluster-wide unique keys. If a joiner presents either one already held by a
system the peer knows, OpenVMS's configuration poller refuses it outright:

    %PEA0, Remote System Conflicts with Known System - REMOTE NODE <name>

and the join never happens -- the VC opens, the peer never sends its connect,
and CLUSTER_NODES never moves. That is indistinguishable at the console from
the vms-2f3 stall, so a colliding identity silently turns any experiment into
a null result attributed to the wrong cause. It has already happened: parallel
agents minted OVMXY1 and OVMXP1 both on SCSSYSTEMID 1601, and OVMXP2/OVMXY2
both on 1602, because this script would mint anything it was asked for.

So before writing a store it scans every other sysgen-*.dat in the registry --
the output directory, the template's directory (the template IS a store and
lives in the lab work directory), and anything in MK_SYSGEN_REGISTRY -- and
refuses a SCSNODE or SCSSYSTEMID already claimed by a different store. Scanning
only the output directory would have missed the 1601 collision, because a store
written to a scratch path sees an empty registry.
`--alloc` hands out a free pair instead of making the caller guess.
`--force` overrides, loudly -- use it only when a collision IS the experiment
(the vms-1ae C/E arms deliberately collide).

This registry is necessary but NOT sufficient: it only knows identities minted
on this host. The peer's own `SHOW CLUSTER` is the authoritative known-systems
set, which is why tests/lab/tools/conflictbracket.sh also pre-flights against
it before starting a daemon.

It does NOT regenerate the store from scratch: it copies a known-good template
and patches only SCSNODE and SCSSYSTEMID in place. The default template is the
store used by runs s8A/s8B, both of which JOINED, so every other field is
byte-identical to a store proven to work on this lab.

Layout is from src/libvms/include/sysgen_params.h (v2, SYSGEN_MAX_PARAMS=64):
  header  = magic u32 "GSYS" | version u32 | count u32          (12 bytes)
  param   = name[32] current u32 default u32 min u32 max u32
            flags u8 description[80] type u8 str_current[8] str_default[8]
            -> 146 bytes, padded to 148 by 4-byte alignment
  file    = 12 + 64*148 = 9484 bytes

VMS truncates SCSNODE to 6 characters, so OVMXR11 and OVMXR12 both go on the
wire as OVMXR1 and a grep for the full name silently finds nothing and looks
like a failed run. Names longer than 6 chars are rejected here instead.
"""
import shutil, struct, sys, os

HDR = 12
PSZ = 148
NAME, CUR, DEF, FLAGS, DESC, TYPE, STRCUR, STRDEF = 0, 32, 36, 48, 49, 129, 130, 138
# The lab dataset moved to ZFS tank/vax on 2026-08-01; the ~/vax prefix in
# every older doc and script is dead (CLAUDE.md Rule 8).
DEFAULT_TEMPLATE = "/data/training/vax/cluster/work/sysgen-s8.dat"

def die(m):
    print("mk_sysgen.py: FATAL -- " + m, file=sys.stderr)
    sys.exit(2)


def read_identity(path):
    """(SCSNODE, SCSSYSTEMID) of a store, or None if it is not a SYSG v2 store."""
    try:
        buf = open(path, "rb").read()
    except OSError:
        return None
    if len(buf) != HDR + 64 * PSZ:
        return None
    magic, version, count = struct.unpack_from("<III", buf, 0)
    if magic != 0x53595347 or version != 2:
        return None
    node = sysid = None
    for i in range(count):
        base = HDR + i * PSZ
        name = buf[base:base + 32].split(b"\0")[0].decode("ascii", "replace")
        if name == "SCSNODE":
            node = buf[base + STRCUR:base + STRCUR + 8].split(b"\0")[0].decode(
                "ascii", "replace")
        elif name == "SCSSYSTEMID":
            sysid = struct.unpack_from("<I", buf, base + CUR)[0]
    if node is None or sysid is None:
        return None
    return node, sysid


def registry(dirname, skip=None):
    """{path: (SCSNODE, SCSSYSTEMID)} for every other store in dirname."""
    out = {}
    skip = os.path.realpath(skip) if skip else None
    try:
        names = sorted(os.listdir(dirname))
    except OSError:
        return out
    for fn in names:
        if not (fn.startswith("sysgen-") and fn.endswith(".dat")):
            continue
        path = os.path.join(dirname, fn)
        if skip and os.path.realpath(path) == skip:
            continue
        ident = read_identity(path)
        if ident:
            out[path] = ident
    return out


def registry_dirs(out, tmpl):
    """Directories whose stores count as already-claimed identities.

    The output directory alone is not enough: a store written to /tmp (or to a
    per-run scratch dir) would see an empty registry and mint a duplicate of an
    identity sitting in the lab work directory -- which is exactly how the
    vms-0fe / vms-AA1 collision on SCSSYSTEMID 1601 got through. The template
    is itself a store and lives in the work directory by convention, so it
    names that directory for free. MK_SYSGEN_REGISTRY adds more.
    """
    dirs = [os.path.dirname(os.path.abspath(out)) or ".",
            os.path.dirname(os.path.abspath(tmpl)) or "."]
    dirs += [p for p in os.environ.get("MK_SYSGEN_REGISTRY", "").split(os.pathsep) if p]
    seen, out_dirs = set(), []
    for d in dirs:
        r = os.path.realpath(d)
        if r not in seen and os.path.isdir(r):
            seen.add(r)
            out_dirs.append(d)
    return out_dirs


def check_unique(out, node, sysid, force, tmpl):
    """Refuse an identity another store already claims. Returns nothing; exits."""
    reg = {}
    for d in registry_dirs(out, tmpl):
        reg.update(registry(d, skip=out))
    clashes = []
    for path, (onode, osysid) in sorted(reg.items()):
        # VMS truncates SCSNODE to 6 chars, so compare what reaches the wire.
        if onode[:6].upper() == node[:6].upper():
            clashes.append("SCSNODE %s is already %s" % (node[:6], path))
        if osysid == sysid:
            clashes.append("SCSSYSTEMID %d is already %s (SCSNODE %s)"
                           % (sysid, path, onode))
    if not clashes:
        return
    msg = ("identity collides with an already-minted store:\n    "
           + "\n    ".join(clashes)
           + "\n  A peer that already knows the other system will refuse this one with\n"
             "  '%PEA0, Remote System Conflicts with Known System' and the join will\n"
             "  never complete -- which looks exactly like the vms-2f3 stall and gets\n"
             "  attributed to it. Pick a free pair (mk_sysgen.py --alloc <prefix>).")
    if force:
        print("mk_sysgen.py: WARNING (--force) -- " + msg, file=sys.stderr)
        return
    die(msg)


def alloc(prefix, dirname):
    """Print a SCSNODE/SCSSYSTEMID pair no store in the registry claims."""
    if len(prefix) > 5 or not prefix.isascii() or not prefix.isalnum():
        die("--alloc prefix %r must be <=5 alphanumeric ASCII chars "
            "(SCSNODE is 6 on the wire)" % prefix)
    reg = {}
    for d in registry_dirs(os.path.join(dirname, "x.dat"), DEFAULT_TEMPLATE):
        reg.update(registry(d))
    names = {n[:6].upper() for n, _ in reg.values()}
    ids = {s for _, s in reg.values()}
    node = None
    for suffix in "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ":
        cand = (prefix + suffix).upper()
        if cand not in names:
            node = cand
            break
    if node is None:
        die("every %s? SCSNODE is taken in %s -- pick another prefix"
            % (prefix.upper(), dirname))
    sysid = max(ids) + 1 if ids else 1024
    while sysid in ids or sysid >= 65536:
        sysid += 1
    if sysid >= 65536:
        die("no free SCSSYSTEMID below 65536 in %s" % dirname)
    print("%s %d" % (node, sysid))


def main():
    argv = sys.argv[1:]
    if argv and argv[0] == "--alloc":
        if len(argv) not in (2, 3):
            die("usage: mk_sysgen.py --alloc <prefix> [registry-dir]")
        alloc(argv[1], argv[2] if len(argv) == 3 else
              os.path.dirname(os.path.abspath(DEFAULT_TEMPLATE)) or ".")
        return
    force = False
    if argv and argv[0] == "--force":
        force = True
        argv = argv[1:]
    if len(argv) not in (3, 4):
        die("usage: mk_sysgen.py [--force] <out.dat> <SCSNODE> <SCSSYSTEMID> "
            "[template.dat]\n       mk_sysgen.py --alloc <prefix> [registry-dir]")
    out, node, sysid = argv[0], argv[1], int(argv[2])
    tmpl = argv[3] if len(argv) == 4 else DEFAULT_TEMPLATE

    if len(node) > 6:
        die("SCSNODE %r is %d chars; VMS truncates to 6 and the run becomes "
            "indistinguishable from another identity." % (node, len(node)))
    if not node.isascii() or not node.isalnum():
        die("SCSNODE %r must be alphanumeric ASCII" % node)
    if not (0 < sysid < 65536):
        die("SCSSYSTEMID %d out of range" % sysid)
    if not os.path.isfile(tmpl):
        die("template %s not found" % tmpl)

    check_unique(out, node, sysid, force, tmpl)

    buf = bytearray(open(tmpl, "rb").read())
    if len(buf) != HDR + 64 * PSZ:
        die("template is %d bytes, expected %d" % (len(buf), HDR + 64 * PSZ))
    magic, version, count = struct.unpack_from("<III", buf, 0)
    if magic != 0x53595347 or version != 2:
        die("template magic/version %08x/%d is not SYSG v2" % (magic, version))

    seen = {}
    for i in range(count):
        base = HDR + i * PSZ
        name = buf[base:base + 32].split(b"\0")[0].decode()
        seen[name] = base
        if name == "SCSNODE":
            if buf[base + TYPE] != 1:
                die("SCSNODE in template is not string-typed")
            for off in (STRCUR, STRDEF):
                buf[base + off:base + off + 8] = node.encode().ljust(8, b"\0")
        elif name == "SCSSYSTEMID":
            if buf[base + TYPE] != 0:
                die("SCSSYSTEMID in template is not numeric-typed")
            struct.pack_into("<I", buf, base + CUR, sysid)
            struct.pack_into("<I", buf, base + DEF, sysid)

    for req in ("SCSNODE", "SCSSYSTEMID"):
        if req not in seen:
            die("template has no %s parameter" % req)

    open(out, "wb").write(buf)

    # The prior-admission record (vms-2f3 sec 4d.2) lives beside the store and IS
    # keyed to this identity. A store minted fresh must not inherit one, or the
    # "fresh identity" positive control is really a rejoin.
    side = out + ".cluster"
    if os.path.exists(side):
        os.remove(side)
        print("mk_sysgen.py: removed stale %s" % side)

    print("mk_sysgen.py: %s SCSNODE=%s SCSSYSTEMID=%d (template %s)"
          % (out, node, sysid, tmpl))

main()
