import os, sys, glob
sys.path.insert(0, '/tmp/vms-4f0-wt/docs/clean-room/tools')
from pcap import frames, is6007
BODY = 72
diff17 = same17 = 0
b18_ok = b20_ok = 0; n = 0
for path in sorted(sys.argv[1:]):
    try:
        fr = [(t,p) for (t,p) in frames(path) if is6007(p) and len(p) >= BODY+40]
    except Exception: continue
    reqs = []
    for t,p in fr:
        if p[30] not in (0x4b,0x5b): continue
        cat = p[BODY+8]; op = p[BODY+9]
        if op != 0x12: continue
        b = p[BODY:BODY+40]
        if cat == 0x01:
            reqs.append((t, p[6:12], p[0:6], b))
        elif cat == 0x81:
            for (t0,s0,d0,b0) in reqs:
                if s0 == p[0:6] and d0 == p[6:12] and 0 <= t-t0 < 2.0:
                    n += 1
                    if b0[17] != b[17]: diff17 += 1
                    else: same17 += 1
                    if b[18] == 0x01: b18_ok += 1
                    if b[20:24] == b0[12:16]: b20_ok += 1
                    # verbatim outside the mutations?
                    mism = [i for i in range(10,40)
                            if i not in (17,18,20,21,22,23) and b[i] != b0[i]]
                    if mism: print("  %s: extra mutation at %s" % (os.path.basename(path), mism))
                    reqs.remove((t0,s0,d0,b0)); break
print("matched pairs=%d  b18==0x01: %d  b20:24==req b12:16: %d  b17 same=%d diff=%d" % (n, b18_ok, b20_ok, same17, diff17))
