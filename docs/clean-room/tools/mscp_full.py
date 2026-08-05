from pcap import frames, is6007
path='/home/baron/vax/cluster/captures/af2-firsttimer-established-20260728.pcap'
JOINER=bytes.fromhex('08002b7856b9'); VAX1=bytes.fromhex('aa0004000104')
def le16(b,o): return b[o]|(b[o+1]<<8)
def le32(b,o): return b[o]|(b[o+1]<<8)|(b[o+2]<<16)|(b[o+3]<<24)
fr=[(t,p) for (t,p) in frames(path) if is6007(p)]
t0=fr[0][0]
CONS={0x8fd20008,0x3554000a}
print("=== FULL FRAMES, MSCP client conn (SCC+GUS cmd/resp) ===")
def split_body(b):
    # candidate MSCP-over-SCS layout
    print("      [0:2]cred/mt=%s [2:4]msgid=%s [4:8]=%s [8]op=%02x [9]fl=%02x [10:12]=%s rest=%s"%(
        b[0:2].hex(), b[2:4].hex(), b[4:8].hex(), b[8], b[9], b[10:12].hex(), b[12:].hex()))
for t,p in fr:
    dt=t-t0
    if dt<143.895 or dt>143.905: continue
    if len(p)<72 or p[30] not in (0x5b,0x4b): continue
    rc=le32(p,64); lc=le32(p,68)
    if not (rc in CONS or lc in CONS): continue
    d='J>V' if p[6:12]==JOINER else 'V>J'
    op=le16(p,60)
    body=p[72:]
    label=''
    if op==10 and len(body)>=9:
        mop=body[8]
        label={0x04:'SCC-cmd',0x84:'SCC-END',0x03:'GUS-cmd',0x83:'GUS-END'}.get(mop,'MSCP?%02x'%mop)
    elif op in (0,1,2,3): label='CONN-op%d'%op
    print("%9.5f %s op=%2d ss=%3d ra=%3d rc=%08x lc=%08x  %s"%(dt,d,op,le16(p,34),le16(p,32),rc,lc,label))
    print("      FULLFRAME: %s"%p.hex())
    if op==10 and len(body)>=9: split_body(body)
