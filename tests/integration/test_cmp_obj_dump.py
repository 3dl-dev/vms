#!/usr/bin/env python3
"""cmp_obj_dump.py masks the EMH creation time stamp in BOTH spellings and on
every day of the month: bfd's host branch (ctime, " 1-Nov-2026 08:44") and the
VMS branch ($ASCTIM, " 1-NOV-2006 08:51"). Before this, a one-digit day was not
recognised and every Alpha self-host leg went red on the 1st-9th. The control:
a differing byte OUTSIDE the stamp must still fail the comparison."""
import os, sys, tempfile

repo = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..", "..")
sys.path.insert(0, os.path.join(repo, "tools", "cross-alpha"))
import cmp_obj_dump as c  # noqa: E402

fails = 0
def check(cond, msg):
    global fails
    print(("  ok: " if cond else "  FAIL: ") + msg)
    fails += 0 if cond else 1

def obj(stamp, tail=b"\x01\x02\x03\x04"):
    return b"\x08\x00\x00\x00HDR" + stamp + b"\x00\x00" + tail + b"\x00" * 9

def dump_log(blob):
    rows = []
    for off in range(0, len(blob), 16):
        chunk = blob[off:off + 16].ljust(16, b"\x00")
        words = " ".join(chunk[i:i + 4].hex().upper() for i in range(0, 16, 4))
        rows.append(f"{off:08X} {words}  ................")
    return "\n".join([c.BEGIN] + rows + [c.END + "%X00000001"]) + "\n"

def run(ref, ref2, got):
    d = tempfile.mkdtemp()
    p1, p2, lg = (os.path.join(d, n) for n in ("r1.obj", "r2.obj", "con.log"))
    open(p1, "wb").write(ref); open(p2, "wb").write(ref2); open(lg, "w").write(dump_log(got))
    sys.argv = ["cmp_obj_dump.py", lg, p1, p2]
    return c.main()

for day in (b" 1", b"09", b"18", b"31"):
    m = c.STAMP.fullmatch(day + b"-Nov-2026 08:44")
    check(m is not None, f"host spelling with day {day!r} is a stamp")
    check(c.STAMP.fullmatch(day + b"-NOV-2006 08:51") is not None, f"VMS spelling with day {day!r} is a stamp")

ref = obj(b" 1-Nov-2026 08:44")
check(run(ref, ref, obj(b" 1-NOV-2006 08:51")) == 0,
      "one-digit day: objects differing only in the stamp compare equal")
ref18 = obj(b"18-Oct-2026 08:02")
check(run(ref18, ref18, obj(b"18-OCT-2006 08:08")) == 0, "two-digit day: stamp-only difference compares equal")
check(run(ref, ref, obj(b" 1-NOV-2006 08:51", tail=b"\x01\x02\xff\x04")) == 1,
      "control: a byte that differs outside the stamp still fails")

print("PASS" if fails == 0 else f"FAILED ({fails})")
sys.exit(1 if fails else 0)
