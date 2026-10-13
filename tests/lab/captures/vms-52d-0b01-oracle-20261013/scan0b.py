# scan0b.py <dirs...>: every VAX-sent SYSAP message with body[8]==0x0b or 0x8b, any length.
# prints count by (cat/op, dst kind, len, txn, [12:16], [16:20])
import os, struct, sys, collections
cnt = collections.Counter(); ex = {}; seen = set()
for d0 in sys.argv[1:]:
    for r, _, ns in os.walk(d0):
        for n in ns:
            if not n.endswith('.pcap'):
                continue
            p = os.path.join(r, n)
            d = open(p, 'rb').read()
            if d[:4] != b'\xd4\xc3\xb2\xa1':
                continue
            o = 24
            while o + 16 <= len(d):
                ts, tu, cl, _ = struct.unpack('<IIII', d[o:o + 16]); o += 16
                f = d[o:o + cl]; o += cl
                if len(f) < 100 or f[12:14] != b'\x60\x07' or f[31] != 0x13 or f[55] != 2:
                    continue
                if f[6:12].hex().startswith('525400'):
                    continue
                k = (ts, tu, f[56:120])
                if k in seen:
                    continue
                seen.add(k)
                b = f[72:]
                if b[8] not in (0x0b, 0x8b):
                    continue
                dst = 'OVMX' if f[0:6].hex().startswith('525400') else 'VAX'
                key = ('%02x/%02x' % (b[8], b[9]), dst, len(f), b[4:6].hex(), b[12:16].hex(), b[16:20].hex())
                cnt[key] += 1
                ex.setdefault(key, p)
for k, v in sorted(cnt.items()):
    print(v, k, ex[k].split('/captures/')[-1][:60])
