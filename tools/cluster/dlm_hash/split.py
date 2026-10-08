#!/usr/bin/env python3
"""split.py -- freeze the derivation / held-out split of the DLM hash corpus.

Run ONCE, BEFORE any derivation work (rd vms-9c95 -> vms-66fe -> vms-c6e): the
held-out rows are the only evidence that the derived function generalises
rather than memorises, so they must be chosen by a rule fixed in advance and
then not looked at.

THE RULE (seed "vms-9c95"):

    digest = sha256(b"vms-9c95:" + name_hex + b":" + mode + b":" + group)
    held_out  <=>  digest[0:4] (big-endian u32) % 5 == 0   AND
                   the key is not in prestudy_names.tsv

~20% land in held-out.  prestudy_names.tsv lists the 25 keys that were PRINTED
on screen during the pre-corpus reconnaissance of L1.pcap (the pass that
established where the value rides); they are forced into the derivation split
so that every held-out row is genuinely one nobody had seen.

  split.py CORPUS.tsv --derivation D.tsv --held-out H.tsv [--prestudy P.tsv]
"""
import argparse
import hashlib
import os
import sys

SEED = b"vms-9c95:"
HELD_OUT_MODULUS = 5


def row_key(fields):
    """(name_hex, mode, group) -- the identity the split rule hashes."""
    return (fields[0], fields[2], fields[3])


def selects_held_out(key):
    blob = SEED + b":".join(part.encode("ascii") for part in key)
    digest = hashlib.sha256(blob).digest()
    return int.from_bytes(digest[0:4], "big") % HELD_OUT_MODULUS == 0


def read_prestudy(path):
    if not path or not os.path.exists(path):
        return set()
    keys = set()
    with open(path) as fh:
        for line in fh:
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            keys.add((parts[0], parts[1], parts[2]))
    return keys


def partition(lines, prestudy):
    derivation, held_out = [], []
    for line in lines:
        key = row_key(line.split("\t"))
        if selects_held_out(key) and key not in prestudy:
            held_out.append(line)
        else:
            derivation.append(line)
    return derivation, held_out


def write(path, header, lines):
    with open(path, "w") as fh:
        fh.write(header + "\n")
        for line in lines:
            fh.write(line + "\n")


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("corpus")
    ap.add_argument("--derivation", required=True)
    ap.add_argument("--held-out", required=True)
    ap.add_argument("--prestudy")
    args = ap.parse_args(argv)

    with open(args.corpus) as fh:
        header = fh.readline().rstrip("\n")
        lines = [ln.rstrip("\n") for ln in fh if ln.strip()]

    prestudy = read_prestudy(args.prestudy)
    derivation, held_out = partition(lines, prestudy)
    write(args.derivation, header, derivation)
    write(args.held_out, header, held_out)
    sys.stderr.write("corpus %d  derivation %d  held-out %d (%.1f%%)  prestudy forced %d\n"
                     % (len(lines), len(derivation), len(held_out),
                        100.0 * len(held_out) / max(1, len(lines)), len(prestudy)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
