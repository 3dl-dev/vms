# rec04.py <tools/cluster> <pcap>: print every DATA (b55=0x0d) payload that follows a 0b/04 on the same pair, as hex+ascii
import sys
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A
frames = list(A.read_pcap(sys.argv[2])); t0 = frames[0][0]
open_req = set(); seen = set()
for ts, f in frames:
    if not A.is_sca(f):
        continue
    s, d = f[6:12], f[0:6]
    if A.is_cm(f) and f[55] == 0x02:
        b = f[72:204]
        if b[8] == 0x0b and b[9] == 0x04:
            open_req.add((s, d))
        elif b[8] == 0x8b and b[9] == 0x04:
            open_req.discard((d, s))
        continue
    if f[55] == 0x0d and (s, d) in open_req:
        p = f[84:]
        if p in seen:
            continue
        seen.add(p)
        print('%9.3f %s->%s %d bytes' % (ts - t0, s.hex()[-4:], d.hex()[-4:], len(p)))
        for o in range(0, len(p), 32):
            c = p[o:o + 32]
            print('  %3d %-64s %s' % (o, c.hex(), ''.join(chr(x) if 32 <= x < 127 else '.' for x in c)))
