#!/usr/bin/env python3
# dbreq_conid_census.py <pcap>... -- every cat-0x0b op-0x05 clusterwide
# logical-name DATABASE REQUEST in the corpus, with the three spans that
# decide whether its transfer descriptor is a field or residue (rd vms-92a).
#
#   body[12:16]  the requester's CSID, high word then low word
#   body[16:20]  VMS_OFB_CM_XFER_NAME  -- zero in every real request
#   body[20:24]  VMS_OFB_CM_XFER_CONID -- NON-ZERO in every real-VMS request
#   body[24:32]  a further per-request value, non-zero in the same requests
#   body[32:36]  0x000091e8 in every request
#
# A sender whose [20:24] is zero is OVMX: the frame-grounding table called
# body[16:32] "leftover buffer contents ... sent as zero", and a real VAX asked
# to block-transfer onto connection 0 bugchecks INVEXCEPTN in its port driver.
import struct, sys
rows = []
for path in sys.argv[1:]:
    try: d = open(path, 'rb').read()
    except Exception: continue
    o = 24
    while o + 16 <= len(d):
        ts, tu, cl, ol = struct.unpack('<IIII', d[o:o+16]); o += 16
        f = d[o:o+cl]; o += cl
        if len(f) != 204 or f[12:14] != b'\x60\x07' or f[31] != 0x13: continue
        b = f[72:]
        if len(b) < 132 or b[8] != 0x0b or b[9] != 0x05: continue
        rows.append((path.split('/')[-1], f[6:12].hex(), bytes(b)))
nz = sum(1 for _, _, b in rows if b[20:24] != b'\0\0\0\0')
print('cat-0x0b op-0x05 requests: %d   with a NON-ZERO transfer Con.ID: %d   with ZERO: %d'
      % (len(rows), nz, len(rows) - nz))
for name, src, b in rows:
    print('%-28s src=%s  csid=%s  NAME=%s  CONID=%s  [24:32]=%s  LEN=%s'
          % (name, src[-4:], b[12:16].hex(), b[16:20].hex(), b[20:24].hex(),
             b[24:32].hex(), b[32:36].hex()))
