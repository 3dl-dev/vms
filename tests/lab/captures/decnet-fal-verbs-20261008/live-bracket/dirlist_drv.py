#!/usr/bin/env python3
"""Lab instrument (rd vms-b2f): a DRV for dapprobe_skipci.py `serve` that plays the
FIRST link of a VMS RENAME -- answers CONFIGURATION with $CFG and the client's
DIRECTORY LIST with NAME(volume) NAME(directory) NAME(file) + ACCESS COMPLETE,
blocked as a VMS FAL sends them, then holds the link until the client drops it.
$NAMES = "vol|dir|file" (default the VAX's own form, SYS$SYSDEVICE:|[SYSMGR]|X.TXT;1;
OVMX's form today is VDA0:|[SYS0.SYSCOMMON.SYSMGR]|X.TXT;1). Never ships."""
import os, sys
def out(s): sys.stdout.write(s + "\n"); sys.stdout.flush()
def name(t, s, last=False):
    b = bytes([t, len(s)]) + s.encode()
    return (bytes([0x0f, 0x00]) + b) if last else (bytes([0x0f, 0x02, len(b)]) + b)
vol, d, f = os.environ.get("NAMES", "SYS$SYSDEVICE:|[SYSMGR]|X.TXT;1").split("|")
out("R"); sys.stderr.write("client CONFIG " + sys.stdin.readline().strip() + "\n")
out("S " + os.environ["CFG"])
out("R"); sys.stderr.write("client ACCESS " + sys.stdin.readline().strip() + "\n")
out("S " + (name(8, vol) + name(4, d) + name(2, f) + bytes([0x07, 0x00, 0x02])).hex())
out("R"); sys.stderr.write("client then " + sys.stdin.readline().strip() + "\n")
out("X 0")
