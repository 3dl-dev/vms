#!/usr/bin/env python3
# envelope.py <frames.jsonl> <port> -- for every (cat,op) the named demo port
# emitted, compare each body byte against the REAL-VAX value set for the same
# (cat,op) over the capture corpus, and report the bytes OVMX writes that no
# real VAX has been seen writing there.
#
# A real VAX's SCA source MAC is the DECnet form aa:00:04:00:xx:xx; OVMX under
# QEMU/pcjs-bridged lanes uses 52:54:xx (QEMU) -- the split is stated, not guessed.
import json, struct, sys, collections, glob, os
LOG, PORT = sys.argv[1], sys.argv[2]
CORPUS = sys.argv[3:]

ovmx = collections.defaultdict(lambda: collections.defaultdict(set))
for line in open(LOG):
    line = line.strip()
    if not line: continue
    r = json.loads(line)
    if r['port'] != PORT: continue
    b = bytes.fromhex(r['hex'])
    if len(b) != 204: continue
    body = b[72:]
    k = (body[8], body[9])
    for i in range(132): ovmx[k][i].add(body[i])

real = collections.defaultdict(lambda: collections.defaultdict(set))
nreal = collections.Counter()
for path in CORPUS:
    try: d = open(path, 'rb').read()
    except Exception: continue
    o = 24
    while o + 16 <= len(d):
        ts, tu, cl, ol = struct.unpack('<IIII', d[o:o+16]); o += 16
        f = d[o:o+cl]; o += cl
        if len(f) != 204 or f[12:14] != b'\x60\x07' or f[31] != 0x13: continue
        if f[6:10] != b'\xaa\x00\x04\x00': continue          # real-VMS source only
        body = f[72:]
        k = (body[8], body[9])
        nreal[k] += 1
        for i in range(132): real[k][i].add(body[i])

print('%-8s %-8s %-6s %s' % ('cat/op', 'ovmx-n', 'real-n', 'bytes OVMX writes that no real VAX has written there'))
for k in sorted(ovmx):
    out = []
    for i in range(132):
        if not real[k]: continue
        extra = ovmx[k][i] - real[k][i]
        if extra:
            out.append('[%d]=%s' % (i, ','.join('%02x' % v for v in sorted(extra))))
    print('%02x/%02x  %-8s %-6s %s' % (k[0], k[1], len(ovmx[k][0]), nreal[k],
                                       ' '.join(out[:14]) if real[k] else '(no real frames of this type in the corpus)'))
