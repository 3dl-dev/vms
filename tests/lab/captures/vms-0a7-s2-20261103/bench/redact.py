#!/usr/bin/env python3
"""Replace the bench passwords (env MASK=pw1,pw2,...) by XX in the files named:
as text, and as the space-separated hex a decoder prints ("4e 4f ..." -> "58 58")."""
import os, sys
M = [m.encode() for m in os.environ['MASK'].split(',') if m]
for f in sys.argv[1:]:
    b = open(f, 'rb').read(); n = b
    for m in M:
        n = n.replace(m, b'XX')
        for h in (m.hex(' ').encode(), m.hex(' ').upper().encode()):
            n = n.replace(h, b'58 58')
    if n != b:
        open(f, 'wb').write(n); print('redacted', f)
