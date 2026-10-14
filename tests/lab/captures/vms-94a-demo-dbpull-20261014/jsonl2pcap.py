#!/usr/bin/env python3
"""jsonl2pcap.py <frames.jsonl> <out.pcap> -- the demo L2 switch's frame log
({"t": ms, "port": name, "hex": bytes}) as a libpcap Ethernet capture, so the
lane's pcap tools (dbpush.py, cm_dialogue_audit.py) read it unchanged."""
import json, struct, sys

src, dst = sys.argv[1], sys.argv[2]
out = open(dst, 'wb')
out.write(struct.pack('<IHHiIII', 0xa1b2c3d4, 2, 4, 0, 0, 65535, 1))
n = 0
for line in open(src):
    line = line.strip()
    if not line:
        continue
    try:
        r = json.loads(line)
        b = bytes.fromhex(r['hex'])
    except Exception:
        continue
    ms = int(r.get('t', 0))
    out.write(struct.pack('<IIII', ms // 1000, (ms % 1000) * 1000, len(b), len(b)))
    out.write(b)
    n += 1
out.close()
print('%d frames -> %s' % (n, dst))
