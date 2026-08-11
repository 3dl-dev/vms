#!/usr/bin/env python3
# vms-0425: decode the CM (membership/config) dialogue on the VMS$VAXcluster
# connection, per member, so a first-join capture can be diffed against a
# return capture. Offsets from scs_member_parse (scs_member.c): category=body[8]
# =frame[80], opcode=frame[81], conids frame[64:72], SYSAP name frame[76:92].
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from pcap import frames, is6007

VAX1 = bytes.fromhex('aa0004000104')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
def name_at(p,o): return p[o:o+16].split(b'\x00')[0].decode('latin1','replace')

CAT={0x01:'CONFIG',0x02:'DLM',0x04:'ACK',0x06:'MEMBSHIP',
     0x81:'CONFIG.r',0x82:'DLM.r',0x84:'ACK.r',0x86:'MEMBSHIP.r'}
OP ={0x14:'MODEL',0x01:'PARAMS',0x02:'op02-CONFIG',0x03:'op03-COMMIT',
     0x05:'op05-LOCKRB',0x06:'op06-MEMB',0x09:'op09-XITION',0x0a:'op0a-GO',
     0x12:'op12-RELAY',0x0d:'op0d-DEPART',0x04:'op04-ABORT',0x08:'op08',0x00:'-'}

path=sys.argv[1]
fr=[(t,p) for (t,p) in frames(path) if is6007(p) and len(p)>=82]
if not fr: sys.exit('no 6007 frames')
t0=fr[0][0]
# detect MACs
macs={}
for t,p in fr:
    macs[p[6:12]]=macs.get(p[6:12],0)+1
others=[m for m in macs if m!=VAX1]
# OVMX = the non-DEC-ish, but just label: VAX1, and the rest by hex
def who(mac):
    if mac==VAX1: return 'VAX1'
    if mac[:3]==bytes.fromhex('08002b'): return 'VAX2'
    return 'OVMX'

# map (conid-pair) -> sysap from connect frames (msgtype 0x5b/0x4b carrying a name)
SYSAPS=[b'VMS$VAXcluster',b'SCS$DIRECTORY',b'MSCP$DISK',b'MSCP$TAPE',b'SCS$DIR_LOOKUP',b'VMS$DISK_CL_DRVR']
conid_sysap={}
for t,p in fr:
    nm=p[76:92].split(b'\x00')[0]
    if nm in SYSAPS:
        rc=le32(p,64); lc=le32(p,68)
        conid_sysap[(rc,lc)]=nm.decode()
        conid_sysap[(lc,rc)]=nm.decode()

print(f"# {path}")
print(f"# MACs: " + ", ".join(f"{who(m)}={m.hex()}({macs[m]})" for m in macs))
print("  t       src>dst  mt  cat        op            rconid   lconid   sysap")
for t,p in fr:
    mt=p[30]
    if mt not in (0x4b,0x5b): continue
    cat=p[80]; op=p[81]
    # only CM-category frames (config/membership/ack and their responses)
    if (cat & 0x7f) not in (0x01,0x04,0x06): continue
    src=who(p[6:12]); dst=who(p[0:6])
    rc=le32(p,64); lc=le32(p,68)
    sysap=conid_sysap.get((rc,lc),'?')
    # focus on the cluster membership connection
    print("%8.3f  %-4s>%-4s %02x  %-9s %-13s %08x %08x %s"%(
        t-t0, src, dst, mt, CAT.get(cat,'%02x'%cat), OP.get(op,'%02x'%op), rc, lc, sysap))
