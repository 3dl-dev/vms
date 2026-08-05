import struct, sys
from pcap import frames, is6007
path=sys.argv[1]
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
print("total 6007 frames:", len(fr))
t0=fr[0][0]; t1=fr[-1][0]
print("span sec:", round(t1-t0,2))
# MAC inventory
from collections import Counter
src=Counter(); dst=Counter()
for t,p in fr:
    dst[p[0:6].hex()]+=1; src[p[6:12].hex()]+=1
print("--- src MACs ---")
for m,c in src.most_common(): print(" ", m, c)
print("--- dst MACs ---")
for m,c in dst.most_common(): print(" ", m, c)
# msgtype histogram (abs30)
mt=Counter()
for t,p in fr:
    if len(p)>=31: mt[p[30]]+=1
print("--- msgtype@30 histogram ---")
for m,c in mt.most_common(): print("   0x%02x"%m, c)
