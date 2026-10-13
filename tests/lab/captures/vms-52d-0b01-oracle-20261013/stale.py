# stale.py <tools/cluster> <pcap> <cat> <op>: for each request of cat/op sent A->B, compare its body with the
# last CM message B->A before it; print offsets that DIFFER (i.e. what the sender wrote fresh)
import sys
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A
cat = int(sys.argv[3], 16); op = int(sys.argv[4], 16)
last = {}
frames = list(A.read_pcap(sys.argv[2])); t0 = frames[0][0]
for ts, f in frames:
    if not A.is_cm(f):
        continue
    b = f[A.OFF_CM_BODY:A.OFF_CM_BODY + 132]
    src, dst = f[6:12], f[0:6]
    if b[8] == cat and b[9] == op and (dst, src) in last:
        pts, pb = last[(dst, src)]
        diff = [i for i in range(10, 132) if b[i] != pb[i]]
        # compress into ranges
        rng = [];
        for i in diff:
            if rng and rng[-1][1] == i - 1:
                rng[-1][1] = i
            else:
                rng.append([i, i])
        print("%8.3f %s->%s prev=%02x/%02x@%.3f fresh=%s" % (ts - t0, src.hex()[-4:], dst.hex()[-4:], pb[8], pb[9], pts - t0,
              " ".join("%d:%d" % (a, c + 1) for a, c in rng)))
    last[(src, dst)] = (ts, b)
