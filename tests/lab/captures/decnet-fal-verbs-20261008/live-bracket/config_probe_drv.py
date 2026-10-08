#!/usr/bin/env python3
"""Lab instrument (rd vms-277a): a DRV for ../../decnet-fal-inbound-20261004/dapprobe.py
`serve` that answers the client's CONFIGURATION with the hex in $CFG and logs the
next DAP message the VMS client sends -- to find which CONFIGURATION makes a VMS
RENAME send ACCESS(RENAME) instead of refusing RMS-F-SUPPORT. Never ships.
  CFG=<hex of a whole CONFIGURATION message> DRV=./config_probe_drv.py dapprobe.py ... serve
"""
import os, sys
def out(s): sys.stdout.write(s + "\n"); sys.stdout.flush()
out("R")                                   # the client's CONFIGURATION
sys.stderr.write("client CONFIG " + sys.stdin.readline().strip() + "\n")
out("S " + os.environ["CFG"])
out("R")                                   # what the client sends next
sys.stderr.write("client NEXT " + sys.stdin.readline().strip() + "\n")
out("X 0")
