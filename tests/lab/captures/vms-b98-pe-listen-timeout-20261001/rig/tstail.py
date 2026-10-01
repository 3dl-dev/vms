#!/usr/bin/env python3
# tstail.py <log> <out> : append every new console line of <log> to <out>, prefixed with host epoch (10 ms poll)
import sys, time, os
src, dst = sys.argv[1], sys.argv[2]
f = open(src, 'rb'); f.seek(0, 2)
o = open(dst, 'a', buffering=1)
buf = b''
while True:
    d = f.read()
    if d:
        buf += d
        while b'\n' in buf:
            line, buf = buf.split(b'\n', 1)
            o.write('%.3f %s\n' % (time.time(), line.decode('latin1').replace('\r','').replace('\x07','')))
    else:
        time.sleep(0.01)
