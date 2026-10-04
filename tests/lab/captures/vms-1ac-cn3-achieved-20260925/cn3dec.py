import os, sys
sys.path.insert(0, '/tmp/vms-4f0-wt/docs/clean-room/tools')
from pcap import frames, is6007
BODY=72
NAMES={'080 02b01977e':'VAXC'}
def who(m):
    h=m.hex()
    return {'08002b01977e':'VAXC','525400004f0a':'OVMXA','525400004f0b':'OVMXB'}.get(h, h)
CAT={0x01:'CONFIG',0x02:'DLM',0x04:'ACK',0x06:'MEMB',0x81:'CONFIG.r',0x82:'DLM.r',0x84:'ACK.r',0x86:'MEMB.r'}
OP={0x14:'MODEL',0x01:'PARAMS',0x02:'op02-REQ',0x03:'op03-COMMIT',0x05:'op05-MEMBREC',
    0x06:'op06-MEMB',0x08:'op08-REM',0x09:'op09-ADD',0x0a:'op0a-GO',0x0b:'op0b-STEP',
    0x0c:'op0c-REL',0x0d:'op0d',0x0f:'op0f',0x12:'op12-RELAY',0x04:'op04-ABORT',0x00:'-'}
fr=[(t,p) for (t,p) in frames(sys.argv[1]) if is6007(p) and len(p)>=BODY+12]
t0=fr[0][0]
lo=float(sys.argv[2]) if len(sys.argv)>2 else 0
hi=float(sys.argv[3]) if len(sys.argv)>3 else 1e9
for t,p in fr:
    mt=p[30]
    if mt not in (0x4b,0x5b): continue
    cat=p[BODY+8]; op=p[BODY+9]
    if (cat&0x7f) not in (0x01,0x04,0x06): continue
    dt=t-t0
    if dt<lo or dt>hi: continue
    print("%9.3f %-6s>%-6s %02x %-9s %-13s b16=%02x b17=%02x b18=%02x"%(
        dt, who(p[6:12]), who(p[0:6]), mt, CAT.get(cat,'%02x'%cat),
        OP.get(op,'%02x'%op), p[BODY+16], p[BODY+17], p[BODY+18]))
