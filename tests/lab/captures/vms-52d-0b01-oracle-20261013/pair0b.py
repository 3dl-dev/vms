# pair0b.py <tools/cluster> <op hex> <pcaps/dirs...>: pair each VAX 0b/<op> request with its 8b/<op>
# answer (same body[4:8], reversed MACs) and print the byte offsets that differ (outside [0:4], [8]).
import os, sys, collections
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A
op = int(sys.argv[2], 16)
paths = []
for a in sys.argv[3:]:
    if os.path.isdir(a):
        for r, _, ns in os.walk(a):
            paths += [os.path.join(r, n) for n in ns if n.endswith('.pcap')]
    else:
        paths.append(a)
seen = set(); diffs = collections.Counter(); n = 0
for p in sorted(paths):
    try:
        frames = list(A.read_pcap(p))
    except Exception:
        continue
    reqs = {}
    for ts, f in frames:
        if not A.is_cm(f):
            continue
        b = f[A.OFF_CM_BODY:A.OFF_CM_BODY + 132]
        if b[9] != op:
            continue
        if b[8] == 0x0b:
            reqs[(f[6:12], f[0:6], b[4:8])] = (ts, b)
        elif b[8] == 0x8b:
            k = (f[0:6], f[6:12], b[4:8])
            if k not in reqs:
                continue
            q = reqs.pop(k)[1]
            key = (q, b)
            if key in seen:
                continue
            seen.add(key); n += 1
            d = tuple((i, q[i], b[i]) for i in range(4, 132) if i != 8 and q[i] != b[i])
            diffs[d] += 1
            if len(sys.argv) and os.environ.get('V'):
                print(p.split('/')[-1], k[0].hex()[-4:], '->', k[1].hex()[-4:], q[12:40].hex(), d)
print(n, 'pairs')
for d, c in diffs.most_common():
    print(c, d)
