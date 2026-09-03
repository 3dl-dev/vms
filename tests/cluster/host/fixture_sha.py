#!/usr/bin/env python3
"""fixture_sha.py - recompute the sha256: line of a cluster codec specimen.

The C loader (cluster_fixture.c) refuses a specimen whose declared digest does
not match the assembled wire-len bytes; this is the tool that computes the
digest when a specimen is written or a byte is corrected. It parses the format
exactly as the loader does, so a disagreement between the two shows up as a
red test rather than as a silently-accepted fixture.

    usage: fixture_sha.py [--write] FILE...
"""
import hashlib
import sys


def assemble(path):
    """Return (wire_len, bytes) for a specimen file, loader-compatible."""
    wire_len = None
    in_bytes = False
    cursor = None
    buf = None
    for raw in open(path, "r", encoding="utf-8"):
        for ch in "#;":
            if ch in raw:
                raw = raw[: raw.index(ch)]
        line = raw.strip()
        if not line:
            continue
        if line.startswith("%OVMX-CLUSTER-SPECIMEN"):
            continue
        if line == "%bytes":
            if wire_len is None:
                raise SystemExit(f"{path}: %bytes before wire-len:")
            buf = bytearray(wire_len)
            in_bytes = True
            continue
        if not in_bytes:
            key, _, val = line.partition(":")
            if key.strip() == "wire-len":
                wire_len = int(val.strip())
            continue
        toks = line.split()
        if toks[0].startswith("@"):
            cursor = int(toks[0][1:])
            toks = toks[1:]
        if cursor is None:
            raise SystemExit(f"{path}: byte line before any @offset")
        for tok in toks:
            if len(tok) != 2:
                raise SystemExit(f"{path}: bad hex byte {tok!r}")
            buf[cursor] = int(tok, 16)
            cursor += 1
    if buf is None:
        raise SystemExit(f"{path}: no %bytes section")
    return wire_len, bytes(buf)


def patch(path, digest):
    out = []
    for line in open(path, "r", encoding="utf-8"):
        if line.lstrip().startswith("sha256:"):
            line = "sha256:    %s\n" % digest
        out.append(line)
    open(path, "w", encoding="utf-8").writelines(out)


def main(argv):
    write = "--write" in argv
    files = [a for a in argv[1:] if not a.startswith("--")]
    for path in files:
        _, data = assemble(path)
        digest = hashlib.sha256(data).hexdigest()
        print("%s  %s" % (digest, path))
        if write:
            patch(path, digest)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
