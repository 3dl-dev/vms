#!/usr/bin/env python3
"""Prefix each line of stdin with elapsed seconds since start (host wall clock)."""
import sys, time
t0 = time.time()
buf = b''
out = sys.stdout
while True:
    ch = sys.stdin.buffer.read(1)
    if not ch:
        break
    if ch in (b'\n', b'\r'):
        if buf.strip():
            out.write('[%8.3f] %s\n' % (time.time() - t0, buf.decode('utf-8', 'replace').rstrip()))
            out.flush()
        buf = b''
    else:
        buf += ch
if buf.strip():
    out.write('[%8.3f] %s\n' % (time.time() - t0, buf.decode('utf-8', 'replace')))
