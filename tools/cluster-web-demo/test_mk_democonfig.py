#!/usr/bin/env python3
"""Tests for mk_democonfig.py (rd vms-8b38).

Run:  python3 -m pytest tools/cluster-web-demo/test_mk_democonfig.py
  or:  python3 -m unittest tools.cluster-web-demo.test_mk_democonfig  (via path)

The cross-validation tests are deliberately NON-CIRCULAR:
  * the store layout is checked with tests/lab/tools/mk_sysgen.py's OWN reader
    (read_identity) and against a store mk_sysgen.py itself produces by patching
    the shipped seed -- an independently written codepath, not this writer;
  * CLUSTER_AUTHORIZE.DAT bytes are checked against tools/cluster/
    mk_cluster_authorize (which calls the runtime's cluster_authorize_write()).
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

_THIS_DIR = os.path.dirname(os.path.abspath(__file__))
_REPO_ROOT = os.path.abspath(os.path.join(_THIS_DIR, "..", ".."))
sys.path.insert(0, _THIS_DIR)

import mk_democonfig as M  # noqa: E402

_SEED = os.path.join(_REPO_ROOT, "distro", "rootfs", "vms", "SYS0",
                     "SYSCOMMON", "SYSEXE", "OVMXVMSSYS.PAR")
_MK_SYSGEN = os.path.join(_REPO_ROOT, "tests", "lab", "tools", "mk_sysgen.py")


def _load_mk_sysgen_read_identity():
    """Load mk_sysgen.py's read_identity() WITHOUT running its module-level
    main() (which would parse argv and sys.exit)."""
    src = open(_MK_SYSGEN).read()
    lines = [ln for ln in src.splitlines() if ln.strip() != "main()"]
    ns = {}
    exec(compile("\n".join(lines), _MK_SYSGEN, "exec"), ns)  # noqa: S102
    return ns["read_identity"]


def _find_record_base(buf, want_name):
    """Locate the byte offset of the param record named want_name (name-scan,
    the same way the C reader's strncasecmp loop finds it)."""
    _, _, count = struct.unpack_from("<III", buf, 0)
    for i in range(count):
        base = M.HDR + i * M.PSZ
        name = buf[base:base + 32].split(b"\0")[0].decode("ascii", "replace")
        if name == want_name:
            return base
    return None


class TestSysgenStore(unittest.TestCase):
    def test_header_and_size(self):
        buf = M.build_sysgen_store("OVMXA", 1987, 1, 2)
        self.assertEqual(len(buf), 9484)
        magic, version, count = struct.unpack_from("<III", buf, 0)
        self.assertEqual(magic, 0x53595347)
        self.assertEqual(version, 2)
        self.assertEqual(count, len(M.PARAM_TABLE))
        self.assertLessEqual(count, 64)

    def test_round_trip_identity(self):
        buf = M.build_sysgen_store("OVMXA", 1987, votes=1, expected_votes=2,
                                   alloclass=7, vaxcluster=2)
        p = M.parse_sysgen_store(buf)["params"]
        self.assertEqual(p["SCSNODE"]["str_current"], "OVMXA")
        self.assertEqual(p["SCSNODE"]["type"], M.SYSGEN_TYPE_STRING)
        self.assertEqual(p["SCSSYSTEMID"]["current"], 1987)
        self.assertEqual(p["SCSSYSTEMID"]["type"], M.SYSGEN_TYPE_NUMERIC)
        self.assertEqual(p["VAXCLUSTER"]["current"], 2)
        self.assertEqual(p["VOTES"]["current"], 1)
        self.assertEqual(p["EXPECTED_VOTES"]["current"], 2)
        self.assertEqual(p["ALLOCLASS"]["current"], 7)

    def test_completeness_of_loader_params(self):
        """Every parameter load_cluster_sysgen_params() reads is PRESENT with
        the right type -- the completeness the shipped seed lacks (E61)."""
        buf = M.build_sysgen_store("OVMXA", 1987, 1, 2)
        p = M.parse_sysgen_store(buf)["params"]
        for name in M.REQUIRED_LOADER_PARAMS:
            self.assertIn(name, p, "loader param %s absent from store" % name)
        self.assertEqual(p["SCSNODE"]["type"], M.SYSGEN_TYPE_STRING)
        for name in M.REQUIRED_LOADER_PARAMS:
            if name != "SCSNODE":
                self.assertEqual(p[name]["type"], M.SYSGEN_TYPE_NUMERIC, name)
        # The six the seed omits, present with their table defaults:
        self.assertEqual(p["MSCP_LOAD"]["current"], 1)
        self.assertEqual(p["MSCP_SERVE_ALL"]["current"], 0)
        self.assertEqual(p["NISCS_MAX_PKTSZ"]["current"], 1498)
        self.assertEqual(p["TIMVCFAIL"]["current"], 1600)
        self.assertEqual(p["LOCKDIRWT"]["current"], 0)
        self.assertEqual(p["QDSKVOTES"]["current"], 0)

    def test_seed_actually_omits_them(self):
        """Guard the premise: the shipped seed IS incomplete (E61), so the
        completeness this tool adds is real, not a straw man."""
        seed = open(_SEED, "rb").read()
        for name in ("LOCKDIRWT", "QDSKVOTES", "TIMVCFAIL", "NISCS_MAX_PKTSZ",
                     "MSCP_LOAD", "MSCP_SERVE_ALL"):
            self.assertIsNone(_find_record_base(seed, name),
                              "seed unexpectedly already has %s" % name)

    def test_rejects_bad_scsnode(self):
        with self.assertRaises(ValueError):
            M.build_sysgen_store("TOOLONG", 1987, 1, 2)  # >6 chars
        with self.assertRaises(ValueError):
            M.build_sysgen_store("OV MX", 1987, 1, 2)    # non-alnum
        with self.assertRaises(ValueError):
            M.build_sysgen_store("OVMXA", 0, 1, 2)       # sysid out of range


class TestLayoutCrossValidation(unittest.TestCase):
    """Non-circular: the OVMX C reader parses this writer's store identically."""

    def test_mk_sysgen_reader_parses_our_store(self):
        read_identity = _load_mk_sysgen_read_identity()
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "ours.dat")
            with open(path, "wb") as fp:
                fp.write(M.build_sysgen_store("OVMXT", 1987, 1, 2))
            ident = read_identity(path)
        self.assertIsNotNone(ident, "mk_sysgen.read_identity rejected our store")
        node, sysid = ident
        self.assertEqual(node, "OVMXT")
        self.assertEqual(sysid, 1987)

    def test_identity_records_byte_match_mk_sysgen(self):
        """Generate the same identity two ways -- our from-scratch writer and
        mk_sysgen.py patching the shipped seed -- and assert the SCSNODE and
        SCSSYSTEMID records carry the identity bytes at identical intra-record
        offsets (same layout the proven-good store uses)."""
        node, sysid = "OVMXT", 1987
        with tempfile.TemporaryDirectory() as d:
            # mk_sysgen wants a template; use a copy of the seed named so its
            # registry scan (sysgen-*.dat) ignores it, and an empty output dir.
            tmpl = os.path.join(d, "template.dat")
            shutil.copyfile(_SEED, tmpl)
            outdir = os.path.join(d, "out")
            os.makedirs(outdir)
            ref = os.path.join(outdir, "ref.dat")
            env = dict(os.environ, MK_SYSGEN_REGISTRY="")
            subprocess.run([sys.executable, _MK_SYSGEN, ref, node, str(sysid),
                            tmpl], check=True, capture_output=True, env=env)
            ref_buf = open(ref, "rb").read()

        our_buf = M.build_sysgen_store(node, sysid, 1, 2)
        self.assertEqual(len(ref_buf), len(our_buf), 9484)

        for pname, sub_lo, sub_len in (
                ("SCSNODE", M.O_STRCUR, M.SYSGEN_STRVAL_LEN),   # str_current
                ("SCSNODE", M.O_STRDEF, M.SYSGEN_STRVAL_LEN),   # str_default
                ("SCSSYSTEMID", M.O_CUR, 4)):                    # current u32
            rb = _find_record_base(ref_buf, pname)
            ob = _find_record_base(our_buf, pname)
            self.assertIsNotNone(rb, "mk_sysgen store lacks %s" % pname)
            self.assertIsNotNone(ob, "our store lacks %s" % pname)
            self.assertEqual(
                ref_buf[rb + sub_lo:rb + sub_lo + sub_len],
                our_buf[ob + sub_lo:ob + sub_lo + sub_len],
                "%s identity bytes at intra-record offset %d differ from "
                "mk_sysgen's layout" % (pname, sub_lo))
        # And the type tag sits where mk_sysgen expects it (str vs numeric).
        self.assertEqual(ref_buf[_find_record_base(ref_buf, "SCSNODE") + M.O_TYPE],
                         our_buf[_find_record_base(our_buf, "SCSNODE") + M.O_TYPE])


class TestClusterAuthorize(unittest.TestCase):
    def test_header_and_group(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "CLUSTER_AUTHORIZE.DAT")
            M.write_cluster_authorize(path, 257, "", writer="py")
            buf = open(path, "rb").read()
        self.assertEqual(len(buf), 44)
        magic, version, group = struct.unpack_from("<IIH", buf, 0)
        self.assertEqual(magic, 0x43415554)
        self.assertEqual(version, 1)
        self.assertEqual(group, 257)

    def test_byte_equality_python_vs_c_writer(self):
        exe = M.build_c_cluster_authorize()
        if not exe:
            self.skipTest("no C compiler available to build mk_cluster_authorize")
        for group, pw in ((257, ""), (2026, "s3cr3t"), (1, "x" * 40)):
            with tempfile.TemporaryDirectory() as d:
                cpath = os.path.join(d, "c.dat")
                M.write_cluster_authorize_via_c(cpath, group, pw, exe=exe)
                c_bytes = open(cpath, "rb").read()
            py_bytes = M.pack_cluster_authorize_py(group, pw)
            self.assertEqual(py_bytes, c_bytes,
                             "python CAUT replica differs from the C writer "
                             "for group=%d pw=%r" % (group, pw))

    def test_c_writer_is_the_default_when_available(self):
        with tempfile.TemporaryDirectory() as d:
            path = os.path.join(d, "CLUSTER_AUTHORIZE.DAT")
            used = M.write_cluster_authorize(path, 257, "", writer="auto")
        # On this repo cc/gcc is present, so auto must pick the C writer.
        if shutil.which("cc") or shutil.which("gcc"):
            self.assertEqual(used, "c")


class TestRosterUniqueness(unittest.TestCase):
    def test_rejects_duplicate_scsnode(self):
        nodes = [{"name": "OVMXA", "id": 1987},
                 {"name": "OVMXA", "id": 1988}]
        with self.assertRaises(ValueError):
            M.validate_roster(nodes)

    def test_rejects_duplicate_scssystemid(self):
        nodes = [{"name": "OVMXA", "id": 1987},
                 {"name": "OVMXB", "id": 1987}]
        with self.assertRaises(ValueError):
            M.validate_roster(nodes)

    def test_rejects_scsnode_case_fold_collision(self):
        # SCSNODE is compared case-folded (the wire name is upper-cased), so
        # OVMXA and ovmxa are the same identity.
        nodes = [{"name": "OVMXA", "id": 1987},
                 {"name": "ovmxa", "id": 1988}]
        with self.assertRaises(ValueError):
            M.validate_roster(nodes)

    def test_accepts_distinct_roster_and_emits_both_artifacts(self):
        nodes = [{"name": "OVMXA", "id": 1987, "votes": 1, "expected_votes": 2,
                  "group": 257},
                 {"name": "OVMXB", "id": 1988, "votes": 1, "expected_votes": 2,
                  "group": 257}]
        with tempfile.TemporaryDirectory() as d:
            written = M.emit_roster(nodes, d, caut_writer="auto")
            self.assertEqual(len(written), 2)
            for w in written:
                self.assertEqual(os.path.getsize(w["par"]), 9484)
                self.assertEqual(os.path.getsize(w["caut"]), 44)
                p = M.parse_sysgen_store(open(w["par"], "rb").read())["params"]
                self.assertEqual(p["SCSNODE"]["str_current"], w["name"])
                self.assertEqual(p["SCSSYSTEMID"]["current"], w["id"])

    def test_builtin_demo_node_a(self):
        n = M.DEMO_NODE_A
        self.assertEqual(n["name"], "OVMXA")
        self.assertLessEqual(len(n["name"]), 6)
        self.assertEqual(n["votes"], 1)
        # rd vms-6d3d: a real three-node VMScluster carries EXPECTED_VOTES = the
        # total VOTES of its members on every node it authors. This was 2, left
        # over from the proven 2-node milestone.
        self.assertEqual(n["expected_votes"], 3)
        self.assertEqual(n["group"], 257)
        with tempfile.TemporaryDirectory() as d:
            written = M.emit_roster([n], d, caut_writer="auto")
            store = open(written[0]["par"], "rb").read()
        p = M.parse_sysgen_store(store)["params"]
        self.assertEqual(p["SCSNODE"]["str_current"], "OVMXA")
        self.assertEqual(p["SCSSYSTEMID"]["current"], 1987)
        self.assertEqual(p["VAXCLUSTER"]["current"], 2)

    def test_builtin_demo_node_b(self):
        n = M.DEMO_NODE_B
        self.assertEqual(n["name"], "OVMXB")
        self.assertEqual(n["id"], 1988)
        self.assertLessEqual(len(n["name"]), 6)
        self.assertEqual(n["group"], 257)
        self.assertEqual(n["vaxcluster"], 2)

    def test_builtin_demo_node_c(self):
        n = M.DEMO_NODE_C
        self.assertEqual(n["name"], "VAXC")
        self.assertEqual(n["id"], 1989)
        self.assertLessEqual(len(n["name"]), 6)
        self.assertEqual(n["group"], 257)

    def test_demo_roster_is_the_single_source_of_all_three_identities(self):
        # DEMO_ROSTER must be exactly {A, B, C} -- the page/generator's one
        # place to get "all three nodes" (single-ledger: no second hardcoded
        # list of names/ids anywhere else).
        names = {n["name"] for n in M.DEMO_ROSTER}
        ids = {n["id"] for n in M.DEMO_ROSTER}
        self.assertEqual(names, {"OVMXA", "OVMXB", "VAXC"})
        self.assertEqual(ids, {1987, 1988, 1989})

    def test_demo_roster_validates_and_emits_three_distinct_nodes(self):
        # The full target roster must pass the uniqueness validator (no
        # SCSNODE/SCSSYSTEMID collision) and emit three independent artifact
        # sets -- this is the config-injection input for the 3-node demo.
        with tempfile.TemporaryDirectory() as d:
            written = M.emit_roster(M.DEMO_ROSTER, d, caut_writer="auto")
            self.assertEqual({w["name"] for w in written}, {"OVMXA", "OVMXB", "VAXC"})
            for w in written:
                self.assertEqual(os.path.getsize(w["par"]), 9484)
                self.assertEqual(os.path.getsize(w["caut"]), 44)


if __name__ == "__main__":
    unittest.main()
