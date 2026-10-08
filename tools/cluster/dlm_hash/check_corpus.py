#!/usr/bin/env python3
"""check_corpus.py -- the committed DLM hash corpus is well formed and its
derivation/held-out split is still exactly the one frozen before any derivation
work started (rd vms-9c95).

This is the guard that keeps the held-out evidence meaningful: if someone
re-splits the corpus (or quietly moves a row from held-out to derivation after
the fact) the generalisation claim in rd vms-c6e stops meaning anything. Pure
text, no network, no lab.

  check_corpus.py [--fixtures DIR]
"""
import argparse
import collections
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import split as splitmod  # noqa: E402

COLUMNS = ("name_hex", "name_len", "mode", "group", "value",
           "sender", "sender_mac", "pcap", "frame")
NAME_MAX = 31
EXCLUDED_SENDER_PREFIX = "OVMX"


def read_tsv(path):
    with open(path) as fh:
        header = fh.readline().rstrip("\n").split("\t")
        rows = [ln.rstrip("\n").split("\t") for ln in fh if ln.strip()]
    return header, rows


def check_header(header, path, fail):
    if tuple(header) != COLUMNS:
        fail("%s: header is %s, expected %s" % (path, header, list(COLUMNS)))


def check_row(row, lineno, path, fail):
    if len(row) != len(COLUMNS):
        fail("%s:%d: %d columns, expected %d" % (path, lineno, len(row), len(COLUMNS)))
        return
    name_hex, name_len, mode, group, value, sender = row[:6]
    try:
        name = bytes.fromhex(name_hex)
    except ValueError:
        fail("%s:%d: name_hex is not hex" % (path, lineno))
        return
    if len(name) != int(name_len) or not 1 <= len(name) <= NAME_MAX:
        fail("%s:%d: name_len %s does not match a 1..31-byte name" % (path, lineno, name_len))
    if not value.startswith("0x") or len(value) != 10:
        fail("%s:%d: value %r is not 0x%%08x" % (path, lineno, value))
    for num in (mode, group):
        if not num.isdigit():
            fail("%s:%d: %r is not a decimal integer" % (path, lineno, num))
    if sender.upper().startswith(EXCLUDED_SENDER_PREFIX):
        fail("%s:%d: sender %s is an OVMX node -- circular, must never be in the corpus"
             % (path, lineno, sender))


def check_no_conflicts(rows, path, fail):
    values = collections.defaultdict(set)
    for row in rows:
        values[(row[0], row[2], row[3])].add(row[4])
    for key, seen in sorted(values.items()):
        if len(seen) > 1:
            fail("%s: %s mode=%s group=%s carries %s" % (path, key[0], key[1], key[2],
                                                         sorted(seen)))
    if len(values) != len(rows):
        fail("%s: %d rows but only %d unique keys -- corpus must be deduped"
             % (path, len(rows), len(values)))


def check_split(corpus_lines, derivation_lines, heldout_lines, prestudy, fail):
    want_d, want_h = splitmod.partition(corpus_lines, prestudy)
    if derivation_lines != want_d:
        fail("derivation split is not what split.py produces (%d committed, %d computed)"
             % (len(derivation_lines), len(want_d)))
    if heldout_lines != want_h:
        fail("held-out split is not what split.py produces (%d committed, %d computed)"
             % (len(heldout_lines), len(want_h)))
    if not heldout_lines:
        fail("held-out split is empty -- there is nothing left to prove on")


def main(argv=None):
    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.normpath(os.path.join(here, "..", "..", "..",
                                            "tests", "cluster", "fixtures"))
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--fixtures", default=default)
    args = ap.parse_args(argv)

    failures = []

    def fail(msg):
        failures.append(msg)

    paths = {name: os.path.join(args.fixtures, "dlm_hash_%s.tsv" % name)
             for name in ("corpus", "derivation", "heldout")}
    raw = {}
    for name, path in paths.items():
        header, rows = read_tsv(path)
        check_header(header, path, fail)
        for i, row in enumerate(rows, start=2):
            check_row(row, i, path, fail)
        raw[name] = rows
    check_no_conflicts(raw["corpus"], paths["corpus"], fail)

    lines = {}
    for name, path in paths.items():
        with open(path) as fh:
            fh.readline()
            lines[name] = [ln.rstrip("\n") for ln in fh if ln.strip()]
    prestudy = splitmod.read_prestudy(os.path.join(args.fixtures,
                                                   "dlm_hash_prestudy_names.tsv"))
    check_split(lines["corpus"], lines["derivation"], lines["heldout"], prestudy, fail)

    for msg in failures:
        sys.stderr.write("FAIL %s\n" % msg)
    print("corpus %d rows, derivation %d, held-out %d, prestudy %d: %s"
          % (len(raw["corpus"]), len(raw["derivation"]), len(raw["heldout"]),
             len(prestudy), "FAILED" if failures else "OK"))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
