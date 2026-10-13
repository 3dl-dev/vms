# follow.py <tools/cluster> <pcaps/dirs...>: for every 0b/01->8b/01 pair, report the answer (28,36) and
# whether the requester sends that answerer a 0b/04 (or any cat-0x0b request) within 60 s afterwards.
import os, sys
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A
paths = []
for a in sys.argv[2:]:
    if os.path.isdir(a):
        for r, _, ns in os.walk(a):
            paths += [os.path.join(r, n) for n in ns if n.endswith('.pcap')]
    else:
        paths.append(a)
seen = set()
for p in sorted(paths):
    try:
        frames = [(ts, f) for ts, f in A.read_pcap(p) if A.is_cm(f)]
    except Exception:
        continue
    reqs = {}
    for i, (ts, f) in enumerate(frames):
        b = f[A.OFF_CM_BODY:A.OFF_CM_BODY + 132]
        if b[8] == 0x0b and b[9] == 1:
            reqs[(f[6:12], f[0:6], b[4:8])] = ts
        elif b[8] == 0x8b and b[9] == 1:
            k = (f[0:6], f[6:12], b[4:8])
            if k not in reqs:
                continue
            key = bytes(b)
            if key in seen:
                continue
            seen.add(key)
            ans = (int.from_bytes(b[28:32], 'little'), int.from_bytes(b[36:40], 'little'))
            later = set()
            for ts2, g in frames[i + 1:]:
                if ts2 - ts > 60:
                    break
                c = g[A.OFF_CM_BODY:A.OFF_CM_BODY + 132]
                if g[6:12] == k[0] and g[0:6] == k[1] and c[8] == 0x0b:
                    later.add('%02x/%02x' % (c[8], c[9]))
            print(os.path.basename(p), k[1].hex()[-4:], 'answers', k[0].hex()[-4:], ans, 'then requester sends:', sorted(later))
