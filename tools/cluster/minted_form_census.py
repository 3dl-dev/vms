#!/usr/bin/env python3
# minted_form_census.py <dir|pcap-dir>... -- rd vms-ci.6 minted-form audit.
#
# Census of every value a node MINTS and puts on the wire, real VAX vs OVMX
# (sender MAC 52:54:00:*): SCS Con.IDs, CM txn/token, DLM lock ids, MSCP P.CRF,
# port buffer names (MSCP descriptors and block headers), block tags. For each
# field: count, how many have a zero high word / low word, the ranges of each
# half, the top-byte range, and examples. A field whose OVMX form falls outside
# the REAL rows is a crash-vector candidate (a 0x00000101 buffer name halted a
# real VAX2 with a VAXPORT bugcheck, rd vms-239) and blocks lab fires until a
# test pins OVMX's form inside the real envelope.
import struct,sys,os,collections
ps=[os.path.join(r,n) for a in sys.argv[1:] for r,_,ns in os.walk(a) for n in ns if n.endswith(".pcap")]
F=collections.defaultdict(list)
def add(field, real, v): F[(field, 'REAL' if real else 'OVMX')].append(v)
seen=set()
for p in ps:
    try: d=open(p,'rb').read()
    except Exception: continue
    if d[:4]!=b"\xd4\xc3\xb2\xa1": continue
    o=24
    while o+16<=len(d):
        ts,tu,cl,_=struct.unpack("<IIII",d[o:o+16]); o+=16; f=d[o:o+cl]; o+=cl
        if len(f)<60 or f[12:14]!=b"\x60\x07" or f[31]!=0x13: continue
        key=(ts,tu,f[:64]);
        if key in seen: continue
        seen.add(key)
        real=not f[6:12].hex().startswith('525400')
        mt=f[30]; op55=f[55]
        u16=lambda a: struct.unpack('<H',f[a:a+2])[0]
        u32=lambda a: struct.unpack('<I',f[a:a+4])[0]
        if op55==2 and len(f)>=72 and u16(58)==4:
            ct=u16(60)
            if ct in (0,2) and len(f)>=72: add('scs_conid_local(ctl op0/2)',real,u32(68))
            if ct==10 and len(f)>=84:
                body=f[72:]
                cat=body[8]; op=body[9]
                if cat in (1,2,3,4,5,6) and len(body)>=8:
                    add('cm_txn body[4:6] cat%02x'%cat,real,struct.unpack('<H',body[4:6])[0])
                    add('cm_token body[6:8] cat%02x'%cat,real,struct.unpack('<H',body[6:8])[0])
                if cat==2 and len(body)>=28:
                    if op in (1,7): add('dlm_req_lkid op01/07 body[24:28]',real,struct.unpack('<I',body[24:28])[0])
                    if op==0x12: add('dlm_holder_lkid op12 body[24:28]',real,struct.unpack('<I',body[24:28])[0])
                    if op==0x82 and body[10]==1: add('dlm_master_lkid 82/01 body[20:24]',real,struct.unpack('<I',body[20:24])[0])
                # MSCP command CRF (94-content command with opcode < 0x80)
                sca=u16(14)+2
                if sca==94 and len(f)>=100 and f[80] in (0x21,0x22):
                    add('mscp_buffer_name (READ/WRITE desc)',real,u32(92))
                if sca==94 and len(f)>=81 and f[80]<0x80 and f[80] in (0x01,0x02,0x03,0x04,0x08,0x09,0x0a,0x0b,0x21,0x22,0x10,0x11,0x12,0x13):
                    add('mscp_cmd_crf',real,u32(72))
        if op55 in (5,8,0x14) and len(f)>=84:
            add('blk_initiator_conid +0',real,u32(56))
            add('blk_tag +4',real,u16(60)); add('blk_tag +6',real,u16(62))
            add('blk_header src_name',real,u32(68))
for k in sorted(F):
    v=F[k]; n=len(v)
    hi0=sum(1 for x in v if x>0xffff and False)
    if max(v)<=0xffff:
        print("%-38s %s n=%6d min=%#06x max=%#06x distinct=%d zero=%d" % (k[0],k[1],n,min(v),max(v),len(set(v)),v.count(0)))
    else:
        h0=sum(1 for x in v if (x>>16)==0); l0=sum(1 for x in v if (x&0xffff)==0)
        los=[x&0xffff for x in v]; his=[x>>16 for x in v]
        print("%-38s %s n=%6d hi16==0:%d lo16==0:%d lo16[%#x..%#x] hi16[%#x..%#x] top8[%#x..%#x] eg=%s" % (k[0],k[1],n,h0,l0,min(los),max(los),min(his),max(his),min(x>>24 for x in v),max(x>>24 for x in v),['%08x'%x for x in list(dict.fromkeys(v))[:4]]))
