#!/usr/bin/env python3
# scan08.py <pcap...> : every cat-0x01 op-0x08 (class-0x03 REMOVE open) CM body, deduped by
# (file, sender, send_msg); prints src LAVC sysid -> dst sysid, len, and body hex.
import sys, struct, gzip
def frames(path):
    op = gzip.open if path.endswith('.gz') else open
    f = op(path, 'rb'); gh = f.read(24)
    if len(gh) < 24: return
    le = gh[:4] in (b'\xd4\xc3\xb2\xa1', b'\x4d\x3c\xb2\xa1')
    ns = gh[:4] in (b'\x4d\x3c\xb2\xa1', b'\xa1\xb2\x3c\x4d')
    E = '<' if le else '>'
    while True:
        h = f.read(16)
        if len(h) < 16: break
        s, us, cl, ol = struct.unpack(E+'IIII', h); d = f.read(cl)
        yield s + us/(1e9 if ns else 1e6), d
def sid(b): return b[4] | (b[5] << 8)
B = 72
for path in sys.argv[1:]:
    seen = set()
    try:
        for t, p in frames(path):
            if len(p) < B+60 or p[12:14] != b'\x60\x07': continue
            if p[30] not in (0x4b, 0x5b, 0x7b): continue
            if struct.unpack('<H', p[60:62])[0] != 10: continue
            if p[B+8] != 0x01 or p[B+9] != 0x08: continue
            key = (sid(p[24:30]), p[B] | p[B+1] << 8)
            if key in seen: continue
            seen.add(key)
            n = struct.unpack('<H', p[14:16])[0] + 2 + 14   # SCA content + eth
            body = p[B:min(len(p), n)]
            print("%s t=%.3f %d->%d bodylen=%d %s" % (path.split('/')[-2] + '/' + path.split('/')[-1], t, sid(p[24:30]), sid(p[16:22]), len(body), body.hex()))
    except Exception as e:
        print("ERR", path, e, file=sys.stderr)
