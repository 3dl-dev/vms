import os, sys, glob
sys.path.insert(0, '/tmp/vms-4f0-wt/docs/clean-room/tools')
from pcap import frames, is6007
BODY = 72
tot_req = tot_rsp = 0
for path in sorted(sys.argv[1:]):
    try:
        fr = [(t,p) for (t,p) in frames(path) if is6007(p) and len(p) >= BODY+40]
    except Exception as e:
        print("%-70s ERR %s" % (os.path.basename(path), e)); continue
    if not fr: continue
    req = []; rsp = []
    for t,p in fr:
        if p[30] not in (0x4b,0x5b): continue
        cat = p[BODY+8]; op = p[BODY+9]
        if op != 0x12: continue
        if cat == 0x01: req.append((t, p[6:12], p[0:6]))
        elif cat == 0x81: rsp.append((t, p[6:12], p[0:6]))
    if not req and not rsp: continue
    # match: a request A->B answered by B->A within 2s
    answered = 0
    for (t,s,d) in req:
        if any(abs(t2-t) < 2.0 and s2 == d and d2 == s for (t2,s2,d2) in rsp):
            answered += 1
    tot_req += len(req); tot_rsp += answered
    print("%-62s req=%-4d answered=%-4d rsp=%d" % (os.path.basename(path), len(req), answered, len(rsp)))
print("TOTAL op-0x12 requests=%d answered=%d" % (tot_req, tot_rsp))
