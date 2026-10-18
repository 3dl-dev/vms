#!/usr/bin/env python3
"""cmp_obj_dump.py - vms-5303e: compare an object DCL DUMP printed on OVMX with
the bytes of a reference object, ignoring the byte offsets the reference toolchain
itself varies between two assemblies of the same source (creation time stamps).

usage: cmp_obj_dump.py <console-log> <ref1.obj> <ref2.obj>
Exit 0 when the dumped object has the reference length and differs from ref1
only at offsets where ref1 and ref2 differ from each other. Prints a verdict."""
import os, re, sys

BEGIN = os.environ.get("CMP_BEGIN", "AS-PROOF: === INDEPENDENT READER: DCL DUMP HELLO.OBJ ===")
END = os.environ.get("CMP_END", "AS-PROOF: DUMP-STATUS=")

def dumped(log):
    out, on = bytearray(), False
    for line in open(log, "rb").read().decode("latin-1").splitlines():
        line = line.rstrip("\r")
        if BEGIN in line:
            on = True
            continue
        if END in line:
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
    # The creation time stamp is a character string in the EMH, and the two
    # assemblers do not spell it alike: bfd's own host-independent branch writes
    # "18-Oct-2026 08:02" from ctime(), the VMS branch (SYS$ASCTIM, which a VMS
    # host runs) "18-OCT-2006 08:08" in upper case, from the guest's own clock. Any
    # DD-MMM-YYYY HH:MM string in either object is a time stamp and is not compared.
    stamp = re.compile(rb"\d\d-[A-Za-z]{3}-\d{4} \d\d:\d\d")
    stamps = 0
    for blob in (a, got):
        for m in stamp.finditer(blob):
            volatile.update(range(m.start(), m.end()))
            stamps += 1
    print(f"  reference object {len(a)} bytes")
    print(f"  dumped object    {len(got)} bytes")
    if len(got) == 0:
        print("  no object bytes dumped")
        return 1
    # DUMP pads the last 16-byte row; trim to the reference length when only zero pad follows
    if len(got) > len(a) and not any(got[len(a):]):
        got = got[:len(a)]
    if len(got) != len(a):
        print("  LENGTH differs from the reference")
        return 1
    bad = [i for i in range(len(a)) if got[i] != a[i] and i not in volatile]
    if bad:
        print(f"  {len(bad)} non-volatile bytes differ; first at offsets {bad[:12]}")
        # every run of differing bytes with both sides, so a wall is read from the log
        runs, start = [], None
        for i in bad:
            if start is not None and i - prev <= 2:
                prev = i
                continue
            if start is not None:
                runs.append((start, prev))
            start = prev = i
        runs.append((start, prev))
        for lo, hi in runs[:24]:
            lo2, hi2 = max(0, lo - 4), min(len(a), hi + 5)
            print(f"    @{lo}..{hi}: got {got[lo2:hi2].hex()} {got[lo2:hi2]!r}")
            print(f"    {' ' * len(str(lo))}    ref {a[lo2:hi2].hex()} {a[lo2:hi2]!r}")
        return 1
    print(f"  identical outside {len(volatile)} time-stamp bytes ({stamps} date strings, plus what two reference runs vary)")
    return 0

sys.exit(main())
