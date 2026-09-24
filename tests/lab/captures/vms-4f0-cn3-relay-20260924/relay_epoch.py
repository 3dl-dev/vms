import os, sys
sys.path.insert(0, '/tmp/vms-4f0-wt/docs/clean-room/tools')
from pcap import frames, is6007
BODY=72
n=0; own=0; echoed=0; both=0; neither=0; diffcase=0
for path in sorted(sys.argv[1:]):
    try: fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=BODY+40]
    except Exception: continue
    reqs=[]
    for t,p in fr:
        if p[30] not in (0x4b,0x5b): continue
        cat=p[BODY+8]; op=p[BODY+9]
        if op!=0x12: continue
        b=p[BODY:BODY+40]
        if cat==0x01: reqs.append((t,p[6:12],p[0:6],b))
        elif cat==0x81:
            for (t0,s0,d0,b0) in reqs:
                if s0==p[0:6] and d0==p[6:12] and 0<=t-t0<2.0:
                    n+=1
                    req_e=b0[12:16]; rsp_e=b[12:16]; rsp_20=b[20:24]
                    if req_e!=rsp_e:
                        diffcase+=1
                        print("  %-40s req_epoch=%s rsp_b12=%s rsp_b20=%s" % (
                            os.path.basename(path), req_e.hex(), rsp_e.hex(), rsp_20.hex()))
                    if rsp_20==rsp_e: own+=1
                    if rsp_20==req_e: echoed+=1
                    if rsp_20==rsp_e and rsp_20==req_e: both+=1
                    if rsp_20!=rsp_e and rsp_20!=req_e: neither+=1
                    reqs.remove((t0,s0,d0,b0)); break
print("pairs=%d  b20:24==RESPONDER's own b12:16: %d  ==REQUEST's b12:16: %d  (both-agree: %d, neither: %d)"%(n,own,echoed,both,neither))
print("pairs where request epoch != responder epoch: %d" % diffcase)
