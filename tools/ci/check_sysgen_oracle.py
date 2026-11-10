#!/usr/bin/env python3
"""check_sysgen_oracle.py - tools/vms_sysgen.c's PQL page-file rows match the
real-VMS capture (rd vms-c746 / vms-89dd8).

docs/oracle/alpha84-sysboot-pql.console.txt is a SYSBOOT> SHOW capture from a
live OpenVMS Alpha V8.4 node. Its PQL_DPGFLQUOTA / PQL_MPGFLQUOTA rows are the
source; the default_params table in tools/vms_sysgen.c (through the
SYSGEN_DEFAULT_PQL_* / SYSGEN_PQL_PGFLQUOTA_MIN macros in
src/libvms/include/sysgen_params.h) is checked against them: current/default and
minimum must agree, and "max -1" must be the table's SYSGEN_NO_MAX.

usage: check_sysgen_oracle.py [repo-root]   (exit 0 clean, 1 mismatch)
       check_sysgen_oracle.py --selftest
"""
import os
import re
import sys

PARAMS = ("PQL_DPGFLQUOTA", "PQL_MPGFLQUOTA")
ORACLE = "docs/oracle/alpha84-sysboot-pql.console.txt"
TABLE = "tools/vms_sysgen.c"
HDR = "src/libvms/include/sysgen_params.h"


def oracle_rows(text):
    """name -> (current, default, min, max) from the SHOW table rows."""
    rows = {}
    for line in text.splitlines():
        m = re.match(r"^(PQL_[DM]PGFLQUOTA)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s+(-?\d+)\s", line)
        if m:
            rows[m.group(1)] = tuple(int(g) for g in m.groups()[1:])
    return rows


def macros(text):
    return {m.group(1): m.group(2).rstrip("uU")
            for m in re.finditer(r"^#define\s+(\w+)\s+(0x[0-9A-Fa-f]+|\d+)[uU]?\b", text, re.M)}


def resolve(tok, mac):
    tok = tok.strip()
    tok = mac.get(tok, tok)
    return int(tok.rstrip("uU"), 0)


def table_rows(text, mac):
    rows = {}
    for name in PARAMS:
        m = re.search(r'\{"%s",\s*([^,]+),\s*([^,]+),\s*([^,]+),\s*([^,]+),' % name, text)
        if m:
            cur, dflt, mn, mx = (resolve(g, mac) for g in m.groups())
            rows[name] = (cur, dflt, mn, mx)
    return rows


def check(oracle_text, table_text, hdr_text):
    errs = []
    o = oracle_rows(oracle_text)
    t = table_rows(table_text, macros(hdr_text))
    for name in PARAMS:
        if name not in o:
            errs.append("%s: no SHOW row in %s" % (name, ORACLE))
            continue
        if name not in t:
            errs.append("%s: no row in %s" % (name, TABLE))
            continue
        oc, od, om, ox = o[name]
        tc, td, tm, tx = t[name]
        if (tc, td, tm) != (oc, od, om):
            errs.append("%s: table current/default/min %s differ from oracle %s"
                        % (name, (tc, td, tm), (oc, od, om)))
        if ox == -1 and tx != 0xFFFFFFFF:
            errs.append("%s: oracle max is -1 (no maximum) but the table max is %d" % (name, tx))
    return errs


def selftest():
    oracle = ("PQL_DPGFLQUOTA   131072  131072  4096  -1 Pagelets D\n"
              "PQL_MPGFLQUOTA     4096    4096  4096  -1 Pagelets D\n")
    hdr = "#define K 0xFFFFFFFFu\n"
    good = ('{"PQL_DPGFLQUOTA", 131072, 131072, 4096, K, 0,\n'
            '{"PQL_MPGFLQUOTA", 4096, 4096, 4096, K, 0,\n')
    bad = good.replace("131072, 131072", "50000, 50000")
    assert not check(oracle, good, hdr), "good table rejected"
    assert check(oracle, bad, hdr), "drifted table accepted"
    assert check(oracle, good.replace("K, 0,\n{", "65535, 0,\n{", 1), hdr), "finite max accepted"
    print("check_sysgen_oracle selftest: ok")


def main(argv):
    if argv[1:2] == ["--selftest"]:
        selftest()
        return 0
    root = argv[1] if len(argv) > 1 else "."
    rd = lambda p: open(os.path.join(root, p), encoding="utf-8").read()
    errs = check(rd(ORACLE), rd(TABLE), rd(HDR))
    for e in errs:
        print("FAIL:", e)
    if not errs:
        print("check_sysgen_oracle: PQL page-file rows match", ORACLE)
    return 1 if errs else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
