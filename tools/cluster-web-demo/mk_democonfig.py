#!/usr/bin/env python3
"""mk_democonfig.py - per-node VMScluster config artifact generator (rd vms-8b38)

Authors the two persistent per-node cluster-configuration artifacts the OVMX
browser cluster demo needs, WITHOUT a running emulator and WITHOUT touching the
image-injection step (that is a separate item, vms-f0f):

  1. OVMXVMSSYS.PAR       - the SYSGEN parameter store STARTUP.EXE reads at boot
                            (src/ovmx_init/ovmx_init.c load_cluster_sysgen_params).
  2. CLUSTER_AUTHORIZE.DAT - the tiny cluster group/password record
                            (src/libvms/include/cluster_authorize.h).

WHY THIS TOOL EXISTS INSTEAD OF mk_sysgen.py
--------------------------------------------
tests/lab/tools/mk_sysgen.py PATCHES a known-good lab TEMPLATE (SCSNODE +
SCSSYSTEMID in place) and copies every other byte verbatim. That template is a
lab store, not the shipped seed, and -- like the shipped seed
distro/.../SYSEXE/OVMXVMSSYS.PAR (count=31) -- it OMITS several parameters that
load_cluster_sysgen_params() reads: LOCKDIRWT, QDSKVOTES, TIMVCFAIL,
NISCS_MAX_PKTSZ, MSCP_LOAD, MSCP_SERVE_ALL. sysgen_read_param() on an absent
record returns -1 and the caller's memset-zero stands, so those parameters read
0 at boot (the "E61" bug). This tool AUTHORS A COMPLETE store from scratch --
every parameter load_cluster_sysgen_params() reads is PRESENT with the correct
type and the table default -- so no reader falls back to a silent zero.

BYTE LAYOUT (authoritative: src/libvms/include/sysgen_params.h, v2)
-------------------------------------------------------------------
  header = magic u32 (SYSGEN_MAGIC 0x53595347 "SYSG") | version u32 (2)
           | count u32                                              (12 bytes)
  param  = name[32] current u32 default u32 min u32 max u32 flags u8
           description[80] type u8 str_current[8] str_default[8]
           -> 146 bytes, padded to 148 by 4-byte struct alignment
  file   = 12 + 64*148 = 9484 bytes (SYSGEN_MAX_PARAMS = 64, fixed size)

The PARAM_TABLE below is a faithful transcription of tools/vms_sysgen.c's
default_params[] (the table SYSGEN WRITE mints into the store) -- the two are
the same defaults on purpose (INV-LEDGER); test_mk_democonfig.py cross-checks
the identity fields against mk_sysgen.py's independent reader so the C runtime
reader parses this store identically.

CLUSTER_AUTHORIZE.DAT (src/libvms/include/cluster_authorize.h)
-------------------------------------------------------------
  magic u32 (0x43415554 "CAUT") | version u32 (1) | group u16 | password[32]
  -> 42 bytes, padded to 44 by 4-byte struct alignment.
By default this tool BUILDS + INVOKES tools/cluster/mk_cluster_authorize (which
calls the runtime's own cluster_authorize_write()), so the bytes are identical
to the runtime writer. If no C compiler is available it falls back to a Python
replica that test_mk_democonfig.py proves byte-for-byte equal to the C writer.

IDENTITY UNIQUENESS (mk_sysgen.py's rule, vms-1ae): SCSNODE (compared on the 6
chars that reach the wire, case-folded) and SCSSYSTEMID are cluster-wide unique
keys; a collision makes a real VAX refuse the joiner with '%PEA0, Remote System
Conflicts with Known System'. The roster driver rejects a duplicate of either.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

# --- sysgen_params.h constants (do not guess; mirrored from the header) ---
SYSGEN_MAGIC = 0x53595347            # "SYSG"
SYSGEN_VERSION = 2
SYSGEN_MAX_PARAMS = 64
SYSGEN_F_DYNAMIC = 0x01
SYSGEN_TYPE_NUMERIC = 0
SYSGEN_TYPE_STRING = 1
SYSGEN_STRVAL_LEN = 8
SYSGEN_DEFAULT_CLUSTER_CREDITS = 10

HDR = 12
PSZ = 148
FILE_SIZE = HDR + SYSGEN_MAX_PARAMS * PSZ   # 9484
# intra-record offsets (match sysgen_params.h / mk_sysgen.py exactly)
O_NAME, O_CUR, O_DEF, O_MIN, O_MAX = 0, 32, 36, 40, 44
O_FLAGS, O_DESC, O_TYPE, O_STRCUR, O_STRDEF = 48, 49, 129, 130, 138

# --- cluster_authorize.h constants ---
CLUSTER_AUTH_MAGIC = 0x43415554      # "CAUT"
CLUSTER_AUTH_VERSION = 1
CLUSTER_AUTH_PWD_LEN = 32
CAUT_FILE_SIZE = 44

_DYN = SYSGEN_F_DYNAMIC
_N = SYSGEN_TYPE_NUMERIC
_S = SYSGEN_TYPE_STRING

# Faithful transcription of tools/vms_sysgen.c default_params[].
# (name, current, default, min, max, flags, description, type, str_current, str_default)
PARAM_TABLE = [
    ("MAXPROCESSCNT", 64, 64, 4, 1024, 0,
     "Maximum number of concurrent processes", _N, "", ""),
    ("CHANNELCNT", 16, 16, 4, 256, _DYN,
     "Number of I/O channels per process", _N, "", ""),
    ("DEFPRI", 4, 4, 0, 31, _DYN, "Default process priority", _N, "", ""),
    ("MAXPRI", 31, 31, 0, 31, 0, "Maximum process priority", _N, "", ""),
    ("MAXBUF", 8192, 8192, 512, 65536, _DYN,
     "Maximum buffered I/O byte count", _N, "", ""),
    ("PQL_DWSDEFAULT", 256, 256, 64, 65536, _DYN,
     "Default working set size", _N, "", ""),
    ("PQL_DWSQUOTA", 512, 512, 64, 65536, _DYN,
     "Working set quota", _N, "", ""),
    ("PQL_DWSEXTENT", 2048, 2048, 64, 262144, _DYN,
     "Working set extent", _N, "", ""),
    ("PQL_DENQLM", 200, 200, 4, 32767, _DYN,
     "Default enqueue limit", _N, "", ""),
    ("PQL_DFILLM", 100, 100, 4, 8192, _DYN,
     "Default open file limit", _N, "", ""),
    ("PQL_DTQELM", 20, 20, 1, 1024, _DYN,
     "Default timer queue entry limit", _N, "", ""),
    ("PQL_DBIOLM", 40, 40, 4, 4096, _DYN,
     "Default buffered I/O limit", _N, "", ""),
    ("PQL_DDIOLM", 40, 40, 4, 4096, _DYN,
     "Default direct I/O limit", _N, "", ""),
    ("PQL_DBYTLM", 65536, 65536, 1024, 16777216, _DYN,
     "Default buffered I/O byte limit", _N, "", ""),
    ("PQL_DPGFLQUOTA", 50000, 50000, 1024, 4194304, _DYN,
     "Default page file quota", _N, "", ""),
    ("VIRTUALPAGECNT", 1048576, 1048576, 1024, 67108864, 0,
     "Virtual page count", _N, "", ""),
    ("GBLPAGES", 8192, 8192, 256, 4194304, _DYN, "Global pages", _N, "", ""),
    ("GBLSECTIONS", 256, 256, 16, 4096, _DYN, "Global sections", _N, "", ""),
    ("LNMPHASHTBL", 128, 128, 16, 8192, _DYN,
     "Logical name hash table size", _N, "", ""),
    ("ACP_MAPCACHE", 32, 32, 4, 256, _DYN, "ACP map cache size", _N, "", ""),
    ("BALSETCNT", 16, 16, 4, 256, 0,
     "Maximum number of processes in balance set", _N, "", ""),
    ("IRPCOUNT", 256, 256, 32, 4096, 0,
     "Number of I/O request packets", _N, "", ""),
    ("SRPCOUNT", 256, 256, 32, 4096, 0,
     "Number of small request packets", _N, "", ""),
    ("LRPCOUNT", 32, 32, 4, 512, 0,
     "Number of large request packets", _N, "", ""),
    # --- vms-ci.8 cluster node-identity parameters ---
    ("SCSSYSTEMID", 0, 0, 0, 65535, _DYN,
     "Cluster system ID (OVMX default 0)", _N, "", ""),
    ("ALLOCLASS", 0, 0, 0, 255, _DYN,
     "Allocation class for shared cluster devices", _N, "", ""),
    ("VOTES", 1, 1, 0, 32767, _DYN,
     "Cluster quorum votes contributed by this node", _N, "", ""),
    ("EXPECTED_VOTES", 1, 1, 1, 32767, _DYN,
     "Expected total cluster quorum votes", _N, "", ""),
    ("VAXCLUSTER", 0, 0, 0, 2, _DYN,
     "Cluster participation (0=disabled,1=enabled,2=auto)", _N, "", ""),
    ("RECNXINTERVAL", 20, 20, 1, 32767, _DYN,
     "Cluster reconnection interval, in seconds", _N, "", ""),
    ("SCSNODE", 0, 0, 0, 0, _DYN,
     "Cluster node name (SCS system name, max 6 chars)", _S, "OVMX", "OVMX"),
    # --- FC-P0.10: remaining VMS_IOCTL_SYSGEN_LOAD parameters ---
    ("LOCKDIRWT", 0, 0, 0, 255, _DYN,
     "Lock directory weight (0 = never a directory node, D-DLM-1)", _N, "", ""),
    ("QDSKVOTES", 0, 0, 0, 32767, _DYN,
     "Quorum disk votes (0 = no quorum disk configured)", _N, "", ""),
    ("TIMVCFAIL", 1600, 1600, 1, 65535, _DYN,
     "Virtual circuit failure detection time (OVMX default; see vms_pe_fsm.h)",
     _N, "", ""),
    ("CLUSTER_CREDITS", SYSGEN_DEFAULT_CLUSTER_CREDITS,
     SYSGEN_DEFAULT_CLUSTER_CREDITS, 1, 65535, _DYN,
     "Per-circuit send credit (OVMX default; matches the lab capture)",
     _N, "", ""),
    ("NISCS_MAX_PKTSZ", 1498, 1498, 512, 65535, _DYN,
     "Maximum NISCA packet size, bytes (spec sec 4(k))", _N, "", ""),
    ("MSCP_LOAD", 1, 1, 0, 1, _DYN,
     "Load the MSCP server (published OpenVMS default: enabled)", _N, "", ""),
    ("MSCP_SERVE_ALL", 0, 0, 0, 1, _DYN,
     "Serve every disk via MSCP (published OpenVMS default: disabled)",
     _N, "", ""),
    ("DISK_QUORUM", 0, 0, 0, 0, _DYN,
     "Quorum disk device name (\"\" = none configured)", _S, "", ""),
]

# Parameters load_cluster_sysgen_params() reads -- every one MUST be present in
# an authored store (the completeness the shipped seed lacks, "E61").
REQUIRED_LOADER_PARAMS = [
    "SCSNODE", "SCSSYSTEMID", "ALLOCLASS", "VOTES", "EXPECTED_VOTES",
    "VAXCLUSTER", "LOCKDIRWT", "QDSKVOTES", "RECNXINTERVAL", "TIMVCFAIL",
    "NISCS_MAX_PKTSZ", "MSCP_LOAD", "MSCP_SERVE_ALL",
]

# Built-in default demo roster entries -- the SINGLE SOURCE of the three demo
# node identities (rd vms-735/vms-4a5/vms-f0f). The page, the generator and
# every injector consume these constants; do not hardcode SCSNODE/SCSSYSTEMID
# anywhere else (single-ledger).
#
# SCSSYSTEMID values avoid ids used in tests/lab captures (1025/1026 = the VAX
# lab nodes, 1986 = OVMXJ1). All three share GROUP 257 -- the group Node C's
# real OpenVMS volume was configured with (tools/lab-vax/
# build_nodeC_vms73_cluster.sh CLUSTER_GROUP=257 -- V7.3 per rd vms-d24, which
# superseded the original V5.5 volume build_nodeC_vms55_cluster.sh still
# documents for anyone who needs it), which puts the demo segment
# on SCA HELLO multicast ab:00:04:01:01:02: the address is
# AB-00-04-01-<LE16(group + 0x100)>, not LE16(group) (rd vms-147 -- an earlier
# comment here read the group off ab:00:04:01:01:01, which is group 1's
# address, and OVMX's matching derivation bug is what kept Node A from ever
# hearing Node C).
#
# Node A (OVMX/x86_64, qemu-wasm) -- LIVE today (vms-b16/vms-f0f proven e2e).
# expected_votes=2 was authored for the proven 2-node milestone (A+C) and is
# INTENTIONALLY not yet 3; reconciling all three EXPECTED_VOTES to the full
# 3-node target is genesis-wiring work for whoever lands vms-1c3, not a change
# to make silently against an already-proven config.
DEMO_NODE_A = {
    "name": "OVMXA", "id": 1987, "votes": 1, "expected_votes": 2,
    "alloclass": 0, "vaxcluster": 2, "group": 257, "password": "",
}

# Node B (OVMX/VAX, pcjs KA655/NetBSD-VAX substrate) -- STAGED. Blocked on the
# Node-B SCS-on-VAX-in-browser join proof (rd vms-613, a separate lane); the
# demo page only materialises this node's iframe when given ?nodeB=. Config
# injection is the SAME arch-agnostic mechanism as Node A (mk_democonfig +
# inject-cluster-config.sh/inject-ods2-config.sh) -- no VAX-specific path.
# expected_votes=3 is the full 3-node target (B is the last node to join in
# the design's genesis order, so it always sees the final roster size).
DEMO_NODE_B = {
    "name": "OVMXB", "id": 1988, "votes": 1, "expected_votes": 3,
    "alloclass": 0, "vaxcluster": 2, "group": 257, "password": "",
}

# Node C (real OpenVMS VAX, pcjs KA655; V7.3 per rd vms-d24 as of rd vms-2570,
# superseding the original V5.5-2H4 choice) -- STAGED, and NOT authored by
# this tool's byte-writers at this edition: it is a PINNED, pre-configured,
# operator-maintained cluster volume (the real cluster password is an
# operator fact never committed to this repo, per docs/design/cluster-web-
# demo.md §5/§11). This entry exists so the identity (SCSNODE=VAXC,
# SCSSYSTEMID=1989, group 257) is declared in the ONE SSOT rather than
# hardcoded into the page/generator -- the roster driver can still validate
# it for uniqueness against A/B even though the real VMS volume's own SYSGEN
# is set by hand, once, outside this pipeline. votes=1, expected_votes=1
# reflects Node C's role as cluster GENESIS (it forms the 1-node cluster
# first; CN grows as A then B join) -- not the final 3-node target.
DEMO_NODE_C = {
    "name": "VAXC", "id": 1989, "votes": 1, "expected_votes": 1,
    "alloclass": 0, "vaxcluster": 2, "group": 257, "password": "",
}

# The full 3-node target roster, in genesis join order (C forms, A joins, B
# joins) -- the single place the demo page/generator should import from for
# "all three nodes" rather than assembling their own list.
DEMO_ROSTER = [DEMO_NODE_C, DEMO_NODE_A, DEMO_NODE_B]


# ---------------------------------------------------------------------------
# OVMXVMSSYS.PAR authoring
# ---------------------------------------------------------------------------

def _enc_fixed(s, width, keep_terminator=True):
    """ASCII-encode s into a fixed-width zero-padded field. If keep_terminator,
    truncate to width-1 so at least one trailing NUL remains (C string field)."""
    enc = s.encode("ascii")
    limit = width - 1 if keep_terminator else width
    if len(enc) > limit:
        enc = enc[:limit]
    return enc.ljust(width, b"\0")


def pack_param(name, cur, dfl, mn, mx, flags, desc, typ, strcur, strdef):
    b = bytearray(PSZ)
    b[O_NAME:O_NAME + 32] = _enc_fixed(name, 32)
    struct.pack_into("<IIII", b, O_CUR,
                     cur & 0xFFFFFFFF, dfl & 0xFFFFFFFF,
                     mn & 0xFFFFFFFF, mx & 0xFFFFFFFF)
    b[O_FLAGS] = flags & 0xFF
    b[O_DESC:O_DESC + 80] = _enc_fixed(desc, 80)
    b[O_TYPE] = typ & 0xFF
    b[O_STRCUR:O_STRCUR + SYSGEN_STRVAL_LEN] = _enc_fixed(strcur, SYSGEN_STRVAL_LEN)
    b[O_STRDEF:O_STRDEF + SYSGEN_STRVAL_LEN] = _enc_fixed(strdef, SYSGEN_STRVAL_LEN)
    # bytes 146,147 stay 0 (struct tail padding)
    return bytes(b)


def build_sysgen_store(scsnode, scssystemid, votes, expected_votes,
                       alloclass=0, vaxcluster=2):
    """Author a COMPLETE OVMXVMSSYS.PAR (9484 bytes) from the param table,
    with the identity/quorum fields parameterized. Returns bytes."""
    if not scsnode or not scsnode.isascii() or not scsnode.isalnum():
        raise ValueError("SCSNODE %r must be non-empty alphanumeric ASCII" % scsnode)
    if len(scsnode) > 6:
        raise ValueError(
            "SCSNODE %r is %d chars; VMS truncates SCSNODE to 6 on the wire, so "
            "a longer name is silently a different identity" % (scsnode, len(scsnode)))
    if not (0 < scssystemid < 65536):
        raise ValueError("SCSSYSTEMID %d out of range (1..65535)" % scssystemid)

    # Parameterized overrides applied to both current AND default (an authored
    # config where current==default, mirroring mk_sysgen.py which patches both).
    overrides = {
        "SCSSYSTEMID": ("num", scssystemid),
        "ALLOCLASS": ("num", alloclass),
        "VOTES": ("num", votes),
        "EXPECTED_VOTES": ("num", expected_votes),
        "VAXCLUSTER": ("num", vaxcluster),
        "SCSNODE": ("str", scsnode),
    }

    records = []
    seen = set()
    for (name, cur, dfl, mn, mx, flags, desc, typ, strcur, strdef) in PARAM_TABLE:
        seen.add(name)
        if name in overrides:
            kind, val = overrides[name]
            if kind == "num":
                cur = dfl = val
            else:
                strcur = strdef = val
        records.append(pack_param(name, cur, dfl, mn, mx, flags, desc, typ,
                                   strcur, strdef))

    for req in REQUIRED_LOADER_PARAMS:
        if req not in seen:
            raise AssertionError("param table is missing loader param %s" % req)

    count = len(records)
    if count > SYSGEN_MAX_PARAMS:
        raise ValueError("param table has %d entries, exceeds SYSGEN_MAX_PARAMS=%d"
                         % (count, SYSGEN_MAX_PARAMS))
    body = b"".join(records)
    body += b"\0" * ((SYSGEN_MAX_PARAMS - count) * PSZ)   # zero-fill spare slots
    buf = struct.pack("<III", SYSGEN_MAGIC, SYSGEN_VERSION, count) + body
    assert len(buf) == FILE_SIZE, "authored store is %d bytes, expected %d" % (
        len(buf), FILE_SIZE)
    return buf


def parse_sysgen_store(buf):
    """Parse an authored store back into {name: dict}. Independent of the writer
    (uses only the documented offsets), for round-trip assertions."""
    if len(buf) != FILE_SIZE:
        raise ValueError("store is %d bytes, expected %d" % (len(buf), FILE_SIZE))
    magic, version, count = struct.unpack_from("<III", buf, 0)
    out = {}
    for i in range(count):
        base = HDR + i * PSZ
        name = buf[base + O_NAME:base + O_NAME + 32].split(b"\0")[0].decode("ascii")
        cur, dfl, mn, mx = struct.unpack_from("<IIII", buf, base + O_CUR)
        typ = buf[base + O_TYPE]
        strcur = buf[base + O_STRCUR:base + O_STRCUR + SYSGEN_STRVAL_LEN].split(
            b"\0")[0].decode("ascii", "replace")
        out[name] = {"current": cur, "default": dfl, "min": mn, "max": mx,
                     "type": typ, "str_current": strcur, "flags": buf[base + O_FLAGS]}
    return {"magic": magic, "version": version, "count": count, "params": out}


# ---------------------------------------------------------------------------
# CLUSTER_AUTHORIZE.DAT authoring
# ---------------------------------------------------------------------------

def pack_cluster_authorize_py(group, password):
    """Python replica of cluster_authorize_write() bytes (44-byte record).
    Cross-validated byte-for-byte against the C writer in the tests."""
    if not (0 <= group <= 0xFFFF):
        raise ValueError("group %d does not fit uint16_t" % group)
    b = bytearray(CAUT_FILE_SIZE)
    struct.pack_into("<IIH", b, 0, CLUSTER_AUTH_MAGIC, CLUSTER_AUTH_VERSION,
                     group & 0xFFFF)
    # password[32] at offset 10, strncpy(..., CLUSTER_AUTH_PWD_LEN-1=31):
    pw = (password or "").encode("ascii")[:CLUSTER_AUTH_PWD_LEN - 1]
    b[10:10 + len(pw)] = pw
    # bytes 42,43 stay 0 (struct tail padding)
    return bytes(b)


_REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
_MK_CAUT_SRC = os.path.join(_REPO_ROOT, "tools", "cluster", "mk_cluster_authorize.c")
_CAUT_INCLUDE = os.path.join(_REPO_ROOT, "src", "libvms", "include")


def build_c_cluster_authorize(cache_dir=None):
    """Compile tools/cluster/mk_cluster_authorize to an executable and return
    its path, or None if no compiler / source is available."""
    cc = shutil.which("cc") or shutil.which("gcc")
    if not cc or not os.path.isfile(_MK_CAUT_SRC):
        return None
    cache_dir = cache_dir or tempfile.gettempdir()
    exe = os.path.join(cache_dir, "mk_cluster_authorize")
    try:
        subprocess.run([cc, "-O2", "-I", _CAUT_INCLUDE, "-o", exe, _MK_CAUT_SRC],
                       check=True, capture_output=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return exe


def write_cluster_authorize_via_c(out_path, group, password, exe=None):
    """Invoke the C writer so the bytes come from cluster_authorize_write()
    itself. Returns True on success, False if the C writer is unavailable."""
    exe = exe or build_c_cluster_authorize()
    if not exe:
        return False
    args = [exe, out_path, str(group)]
    if password:
        args.append(password)
    subprocess.run(args, check=True, capture_output=True)
    return True


def write_cluster_authorize(out_path, group, password, writer="auto"):
    """Write CLUSTER_AUTHORIZE.DAT. writer: 'c' (require C), 'py' (require
    replica), or 'auto' (prefer C, fall back to the validated replica)."""
    if writer in ("c", "auto"):
        if write_cluster_authorize_via_c(out_path, group, password):
            return "c"
        if writer == "c":
            raise RuntimeError("C cluster_authorize writer unavailable "
                               "(need cc/gcc + %s)" % _MK_CAUT_SRC)
    with open(out_path, "wb") as fp:
        fp.write(pack_cluster_authorize_py(group, password))
    return "py"


# ---------------------------------------------------------------------------
# Roster driver
# ---------------------------------------------------------------------------

def _node_defaults(node):
    n = dict(node)
    n.setdefault("votes", 1)
    n.setdefault("expected_votes", 1)
    n.setdefault("alloclass", 0)
    n.setdefault("vaxcluster", 2)
    n.setdefault("group", 257)
    n.setdefault("password", "")
    return n


def validate_roster(nodes):
    """Enforce SCSNODE<=6 chars and cluster-wide uniqueness of SCSNODE (6-char,
    case-folded) and SCSSYSTEMID. Raises ValueError on any collision."""
    by_name = {}
    by_id = {}
    for node in nodes:
        node = _node_defaults(node)
        name = node["name"]
        if len(name) > 6:
            raise ValueError("SCSNODE %r is %d chars; VMS truncates to 6 on the "
                             "wire" % (name, len(name)))
        if not name.isascii() or not name.isalnum():
            raise ValueError("SCSNODE %r must be alphanumeric ASCII" % name)
        key = name[:6].upper()
        sid = int(node["id"])
        if not (0 < sid < 65536):
            raise ValueError("SCSSYSTEMID %d out of range for %s" % (sid, name))
        if key in by_name:
            raise ValueError(
                "duplicate SCSNODE %s (collides with %s): a real VAX refuses the "
                "second with '%%PEA0, Remote System Conflicts with Known System'"
                % (key, by_name[key]))
        if sid in by_id:
            raise ValueError(
                "duplicate SCSSYSTEMID %d (%s collides with %s): a real VAX "
                "refuses the second with '%%PEA0, Remote System Conflicts with "
                "Known System'" % (sid, name, by_id[sid]))
        by_name[key] = name
        by_id[sid] = name


def emit_roster(nodes, out_dir, caut_writer="auto"):
    """For each node emit <NAME>/OVMXVMSSYS.PAR and <NAME>/CLUSTER_AUTHORIZE.DAT
    under out_dir. Returns a list of per-node dicts describing what was written."""
    validate_roster(nodes)
    os.makedirs(out_dir, exist_ok=True)
    written = []
    for node in nodes:
        node = _node_defaults(node)
        name = node["name"]
        node_dir = os.path.join(out_dir, name.upper())
        os.makedirs(node_dir, exist_ok=True)

        par_path = os.path.join(node_dir, "OVMXVMSSYS.PAR")
        store = build_sysgen_store(name, int(node["id"]), int(node["votes"]),
                                   int(node["expected_votes"]),
                                   int(node["alloclass"]), int(node["vaxcluster"]))
        with open(par_path, "wb") as fp:
            fp.write(store)

        caut_path = os.path.join(node_dir, "CLUSTER_AUTHORIZE.DAT")
        used = write_cluster_authorize(caut_path, int(node["group"]),
                                       node["password"], writer=caut_writer)
        written.append({"name": name, "id": int(node["id"]), "dir": node_dir,
                        "par": par_path, "caut": caut_path, "caut_writer": used})
    return written


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------

def _cmd_sysgen(args):
    store = build_sysgen_store(args.scsnode, args.scssystemid, args.votes,
                               args.expected_votes, args.alloclass, args.vaxcluster)
    with open(args.out, "wb") as fp:
        fp.write(store)
    print("mk_democonfig: wrote %s (%d bytes) SCSNODE=%s SCSSYSTEMID=%d "
          "VOTES=%d EXPECTED_VOTES=%d VAXCLUSTER=%d"
          % (args.out, len(store), args.scsnode, args.scssystemid, args.votes,
             args.expected_votes, args.vaxcluster))


def _cmd_authorize(args):
    used = write_cluster_authorize(args.out, args.group, args.password,
                                   writer=args.writer)
    print("mk_democonfig: wrote %s group=%d (writer=%s)"
          % (args.out, args.group, used))


def _cmd_roster(args):
    if args.roster:
        with open(args.roster) as fp:
            nodes = json.load(fp)
    elif args.demo_roster:
        nodes = DEMO_ROSTER
        print("mk_democonfig: --demo-roster given; using the built-in 3-node "
              "demo roster (OVMXA/OVMXB/VAXC)")
    else:
        nodes = [DEMO_NODE_A]
        print("mk_democonfig: no --roster/--demo-roster given; using the "
              "built-in demo Node A only")
    written = emit_roster(nodes, args.out_dir, caut_writer=args.writer)
    for w in written:
        print("  %-8s id=%-6d %s (caut writer=%s)"
              % (w["name"], w["id"], w["dir"], w["caut_writer"]))
    print("mk_democonfig: wrote %d node(s) into %s" % (len(written), args.out_dir))


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("sysgen", help="author one OVMXVMSSYS.PAR")
    p.add_argument("out")
    p.add_argument("scsnode")
    p.add_argument("scssystemid", type=int)
    p.add_argument("--votes", type=int, default=1)
    p.add_argument("--expected-votes", type=int, default=1)
    p.add_argument("--alloclass", type=int, default=0)
    p.add_argument("--vaxcluster", type=int, default=2)
    p.set_defaults(func=_cmd_sysgen)

    p = sub.add_parser("authorize", help="author one CLUSTER_AUTHORIZE.DAT")
    p.add_argument("out")
    p.add_argument("group", type=int)
    p.add_argument("password", nargs="?", default="")
    p.add_argument("--writer", choices=["auto", "c", "py"], default="auto")
    p.set_defaults(func=_cmd_authorize)

    p = sub.add_parser("roster", help="emit both artifacts per node into a dir")
    p.add_argument("out_dir")
    p.add_argument("--roster", help="JSON list of {name,id,votes,expected_votes,"
                                     "alloclass,vaxcluster,group,password}; "
                                     "default = built-in demo Node A")
    p.add_argument("--demo-roster", action="store_true",
                    help="use the built-in 3-node demo roster (DEMO_ROSTER: "
                         "OVMXA/OVMXB/VAXC) instead of Node A alone; ignored "
                         "if --roster is also given")
    p.add_argument("--writer", choices=["auto", "c", "py"], default="auto")
    p.set_defaults(func=_cmd_roster)

    args = ap.parse_args(argv)
    args.func(args)
    return 0


if __name__ == "__main__":
    sys.exit(main())
