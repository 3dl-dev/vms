#!/usr/bin/env python3
"""checkrm.py <tools/cluster> <pcap>...: rd vms-e09's pre-registered rules R1-R3 (PREDICTION.md) on
every named cat-0x02 op-0x04 in each capture, and R4's shape on every 0xf7 answer."""
import sys
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A

for p in sys.argv[2:]:
    last_f9 = {}; open_ = {}
    n = r1 = r2 = r2_scope = ans04 = 0; f7 = f7ok = 0; reqs = {}
    for ts, f in A.read_pcap(p):
        if not A.is_cm(f) or f[55] != 0x02:
            continue
        b = f[A.OFF_CM_BODY:A.OFF_CM_BODY + 132]
        if len(b) < 132:
            continue
        s, d = f[6:12], f[0:6]
        key = bytes(b[44:48 + b[47]])
        if b[8] == 0x02 and b[9] in (0x01, 0x07):
            reqs[(s, d, bytes(b[4:8]))] = b
        if b[8] == 0x82 and b[9] == 0x01 and b[34] == 0xf9:
            last_f9[(d, s, key)] = b          # D answered M: key (M, D, N)
            open_[(d, s, key)] = True
        if b[8] == 0x82 and b[9] == 0x04:
            ans04 += 1
        if b[8] == 0x82 and b[34] == 0xf7:
            f7 += 1
            q = reqs.get((d, s, bytes(b[4:8])))
            if q is not None:
                same = all(b[o] == q[o] for o in range(4, 132)
                           if o != 8 and not (28 <= o < 39))
                if same and b[28:32] == bytes(4):
                    f7ok += 1
        if b[8] == 0x02 and b[9] == 0x04 and b[47] > 0:
            n += 1
            ok = b[4:8] == bytes(4)
            a = last_f9.get((s, d, key))
            if a is not None:
                ok = ok and b[12:16] == a[12:16] and b[128:132] == a[128:132]
                r2_scope += 1
                if open_.get((s, d, key)):
                    r2 += 1
                    open_[(s, d, key)] = False
            if ok:
                r1 += 1
    print('%s: removals=%d R1=%d (%.2f%%) R2=%d/%d R3 answers=%d | 0xf7=%d R4-shape=%d'
          % (p, n, r1, 100.0 * r1 / max(n, 1), r2, r2_scope, ans04, f7, f7ok))
