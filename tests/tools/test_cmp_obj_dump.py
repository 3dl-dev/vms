#!/usr/bin/env python3
"""test_cmp_obj_dump.py - self-test for tools/cross-alpha/cmp_obj_dump.py, the
DUMP-vs-reference comparator every Alpha self-host gate (as, driver, link,
xgcclink) reads its verdict from.

The EMH creation time stamp is the one part of an object the two toolchains
legitimately write differently (ctime "Nov" vs $ASCTIM "NOV", and the clock
itself). On days 1-9 both writers pad the day with a blank (" 1-NOV-2026"); a
mask that only knew two-digit days reddened every self-host gate on 1 Nov 2026.
These cases pin: a stamp is masked whatever the day, and any other byte that
differs still fails.

Run: python3 -m pytest tests/tools/test_cmp_obj_dump.py -v
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
TOOL = REPO_ROOT / "tools" / "cross-alpha" / "cmp_obj_dump.py"
BEGIN = "T: === INDEPENDENT READER: DCL DUMP X.OBJ ==="
END = "T: DUMP-STATUS="


def dump_lines(b: bytes) -> list[str]:
    out = []
    for o in range(0, len(b), 16):
        row = b[o:o + 16]
        hx = " ".join(row[i:i + 4].hex().upper().ljust(8) for i in range(0, 16, 4))
        out.append("%08X %s  %s" % (o % 512, hx, "." * len(row)))
    return out


def obj(stamp: bytes, tail: bytes = b"\x11\x22\x33\x44") -> bytes:
    return b"\x00\x01\x02\x03HEAD" + stamp + b"\x00\x00" + tail + bytes(10)


def run(tmp_path: Path, got: bytes, ref1: bytes, ref2: bytes) -> int:
    log = tmp_path / "console.log"
    log.write_text("\r\n".join([BEGIN, *dump_lines(got), END + "%X00000001", ""]))
    (tmp_path / "r1.obj").write_bytes(ref1)
    (tmp_path / "r2.obj").write_bytes(ref2)
    env = {"CMP_BEGIN": BEGIN, "CMP_END": END, "PATH": "/usr/bin:/bin"}
    return subprocess.run(
        [sys.executable, "-I", str(TOOL), str(log), str(tmp_path / "r1.obj"), str(tmp_path / "r2.obj")],
        env=env, capture_output=True, text=True,
    ).returncode


CROSS_1 = b" 1-Nov-2026 08:44"
NATIVE_1 = b" 1-NOV-2006 08:56"
CROSS_18 = b"18-Oct-2026 08:02"
NATIVE_18 = b"18-OCT-2006 08:08"


@pytest.mark.parametrize("cross,native", [(CROSS_1, NATIVE_1), (CROSS_18, NATIVE_18)])
def test_stamp_masked_any_day(tmp_path, cross, native):
    ref = obj(cross)
    assert run(tmp_path, obj(native), ref, ref) == 0


@pytest.mark.parametrize("cross,native", [(CROSS_1, NATIVE_1), (CROSS_18, NATIVE_18)])
def test_other_byte_still_fails(tmp_path, cross, native):
    ref = obj(cross)
    assert run(tmp_path, obj(native, tail=b"\x11\x22\x34\x44"), ref, ref) == 1


def test_identical_passes_and_empty_fails(tmp_path):
    ref = obj(CROSS_1)
    assert run(tmp_path, ref, ref, ref) == 0
    assert run(tmp_path, b"", ref, ref) == 1


def test_not_a_date_is_compared(tmp_path):
    # a date-shaped string with no day digit at all is not a stamp
    ref = obj(b"  -Nov-2026 08:44")
    assert run(tmp_path, obj(b"  -NOV-2026 08:44"), ref, ref) == 1
