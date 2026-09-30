"""trim.py <in.pcap> <out.pcap> <joiner-sysid> -- keep only what the e88 finding
reads: every 0x6007 frame to or from the JOINER that is channel/VC control, SCA
connection control, or a VMS$VAXcluster cat-0x01/0x81/0x04 dialogue body, plus
every cat-0x01 op-0x02 / op-0x12 between any two nodes. Drops MSCP and DLM bulk.
Offsets: abs 30 msgtype (spec 4d), abs 60 SCA ctl type (4h(1a)), body abs 72."""
import sys, struct
def sysid(p): return p[28] | (p[29] << 8)
inp, outp, J = sys.argv[1], sys.argv[2], int(sys.argv[3])
d = open(inp, 'rb').read()
hdr, off, out, mac2 = d[:24], 24, [], {}
recs = []
while off + 16 <= len(d):
    ts, tu, incl, orig = struct.unpack('<IIII', d[off:off+16]); p = d[off+16:off+16+incl]
    recs.append((d[off:off+16], p)); off += 16 + incl
for h, p in recs:
    if len(p) >= 32 and p[12:14] == b'\x60\x07' and sysid(p):
        mac2.setdefault(bytes(p[6:12]), sysid(p))
keep = 0
with open(outp, 'wb') as f:
    f.write(hdr)
    for h, p in recs:
        if len(p) < 32 or p[12:14] != b'\x60\x07':
            continue
        s = sysid(p); dd = mac2.get(bytes(p[0:6]))
        ty = p[30]; k = False
        body = len(p) >= 92 and ty in (0x4b, 0x5b, 0x7b)
        if body and struct.unpack('<H', p[60:62])[0] == 10 and p[80] == 1 and p[81] in (2, 0x12):
            k = True
        elif J in (s, dd):
            if not body:
                k = True
            elif struct.unpack('<H', p[60:62])[0] != 10:
                k = True
            elif p[80] in (0x01, 0x81, 0x04) and len(p) == 204:
                k = True
        if k:
            f.write(h); f.write(p); keep += 1
print(outp, keep)
