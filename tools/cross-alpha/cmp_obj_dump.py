#!/usr/bin/env python3
"""cmp_obj_dump.py - vms-5303e: compare an object DCL DUMP printed on OVMX with
the bytes of a reference object, ignoring the byte offsets the reference toolchain
itself varies between two assemblies of the same source (creation time stamps).

usage: cmp_obj_dump.py <console-log> <ref1.obj> <ref2.obj>
Exit 0 when the dumped object has the reference length and differs from ref1
only at offsets where ref1 and ref2 differ from each other. Prints a verdict."""
import re, sys

def dumped(log):
    out, on = bytearray(), False
    for line in open(log, "rb").read().decode("latin-1").splitlines():
        line = line.rstrip("\r")
        if "AS-PROOF: === INDEPENDENT READER: DCL DUMP HELLO.OBJ ===" in line:
            on = True
            continue
        if "AS-PROOF: DUMP-STATUS=" in line:
            break
        if on and re.match(r"^[0-9A-F]{8} ", line):
            out += bytes.fromhex(line[9:44].replace(" ", ""))
    return bytes(out)

def main():
    log, r1, r2 = sys.argv[1:4]
    got = dumped(log)
    a, b = open(r1, "rb").read(), open(r2, "rb").read()
    if len(a) != len(b):
        print(f"  reference objects differ in LENGTH ({len(a)} vs {len(b)}): cannot mask")
        return 1
    volatile = {i for i in range(len(a)) if a[i] != b[i]}
    print(f"  reference object {len(a)} bytes, {len(volatile)} volatile (time-stamp) offsets")
    print(f"  dumped object    {len(got)} bytes")
    if len(got) == 0:
        print("  no object bytes dumped")
        return 1
    # DUMP pads the last 16-byte row; trim to the reference length when only zero pad follows
    if len(got) > len(a) and not any(got[len(a):]):
        got = got[:len(a)]
    if len(got) != len(a):
        print("  LENGTH differs from the cross-assembled object")
        return 1
    bad = [i for i in range(len(a)) if got[i] != a[i] and i not in volatile]
    if bad:
        print(f"  {len(bad)} non-volatile bytes differ; first at offsets {bad[:12]}")
        return 1
    print("  identical outside the time-stamp offsets")
    return 0

sys.exit(main())
