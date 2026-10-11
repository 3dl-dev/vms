#!/usr/bin/env python3
"""undump_vms.py DUMP.txt OUT -- rebuild a file, byte for byte, from the output
of OpenVMS DUMP (the default hex-longword format) captured off a lab console
(e.g. tools/oracle/semantic/capture.py --dcl ... 'DUMP SP_EF.EXE').

Each data line is eight longwords, printed most-significant first and right to
left, then the same 32 bytes as ASCII (in byte order), then the byte
offset in the block. The ASCII column is checked against the hex for every
printable byte, so a console glitch in either fails the rebuild. Every block
from 1 to the "End of file block" DUMP reports must be present and complete.
Observed output of the real system only (clean-room Rule 8).
"""
import re
import struct
import sys

LINE = re.compile(r"^ ((?:[0-9A-F]{8} ){8})(.{32}) ([0-9A-F]{6})\s*$")


def undump(text):
    blocks, cur, eof = {}, None, None
    for ln in text.splitlines():
        m = re.search(r"End of file block (\d+)", ln)
        if m:
            eof = int(m.group(1))
        m = re.match(r"Virtual block number (\d+)", ln)
        if m:
            cur = int(m.group(1))
            blocks.setdefault(cur, {})
            continue
        m = LINE.match(ln)
        if m and cur is not None:
            data = b"".join(struct.pack("<I", int(x, 16)) for x in reversed(m.group(1).split()))
            asc = m.group(2)
            for i, b in enumerate(data):
                c = asc[i]
                if 0x20 <= b < 0x7F and c != chr(b):
                    raise ValueError("block %d offset %#x: ASCII %r disagrees with hex %#x"
                                     % (cur, int(m.group(3), 16) + i, c, b))
            blocks[cur][int(m.group(3), 16)] = data
    if eof is None:
        raise ValueError("no 'End of file block' line: not DUMP output")
    out = bytearray()
    for n in range(1, eof + 1):
        rows = blocks.get(n)
        if not rows or sorted(rows) != list(range(0, 512, 32)):
            raise ValueError("block %d missing or incomplete" % n)
        out += b"".join(rows[o] for o in range(0, 512, 32))
    return bytes(out)


def selftest():
    data = bytes(range(256)) * 2
    lines = ["File ID (1,1,0)   End of file block 1 / Allocated 1", "",
             "Virtual block number 1 (00000001), 512 (0200) bytes", ""]
    for o in range(0, 512, 32):
        chunk = data[o:o + 32]
        lws = struct.unpack("<8I", chunk)
        asc = "".join(chr(b) if 0x20 <= b < 0x7F else "." for b in chunk)
        lines.append(" " + "".join("%08X " % w for w in reversed(lws)) + asc + " %06X" % o)
    good = "\n".join(lines)
    ok = undump(good) == data
    bad = good.replace(" 000020", " 000021", 1)
    try:
        undump(bad)
        ok = False
    except ValueError:
        pass
    print("undump_vms selftest:", "PASS" if ok else "FAIL")
    return ok


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        sys.exit(0 if selftest() else 1)
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    # capture.py writes the console text as UTF-8
    b = undump(open(sys.argv[1], encoding="utf-8", errors="replace").read())
    open(sys.argv[2], "wb").write(b)
    print("%s: %d bytes" % (sys.argv[2], len(b)))
