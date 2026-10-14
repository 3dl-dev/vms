#!/usr/bin/env python3
"""lastframes.py <frames.jsonl> <port> [n] -- the last n frames before <port>
stopped transmitting (a bugchecked node's departure), in order, with the full
132-byte SYSAP body of every frame addressed TO it. Reading a peer's bugcheck
means reading what it was handed last; a tally cannot do that."""
import json, sys, collections

path = sys.argv[1]
port = sys.argv[2] if len(sys.argv) > 2 else 'VAXC'
n = int(sys.argv[3]) if len(sys.argv) > 3 else 30

rows = []
for line in open(path):
    line = line.strip()
    if not line:
        continue
    try:
        r = json.loads(line)
        b = bytes.fromhex(r['hex'])
    except Exception:
        continue
    rows.append((r['t'], r['port'], b))
if not rows:
    sys.exit('no frames')
t0 = rows[0][0]

# the victim's own MAC, learned from the frames it sent
mac = None
for _, p, b in rows:
    if p == port:
        mac = b[6:12]
        break
if mac is None:
    sys.exit('port %s never transmitted' % port)
last_tx = max(i for i, (_, p, _) in enumerate(rows) if p == port)
print('%s last transmitted at %.3fs (frame %d of %d); %d frames follow it'
      % (port, (rows[last_tx][0] - t0) / 1000.0, last_tx, len(rows),
         len(rows) - last_tx - 1))

def catop(b):
    if len(b) < 204:
        return '--/--'
    return '%02x/%02x' % (b[72 + 8], b[72 + 9])

print('\n--- the last %d frames up to and including it ---' % n)
for t, p, b in rows[max(0, last_tx - n + 1):last_tx + 1]:
    to_victim = b[0:6] == mac
    print('%9.3f %-6s %s %s %s%s' % ((t - t0) / 1000.0, p,
          b[6:12].hex()[-4:], '->' if not to_victim else '=>',
          b[0:6].hex()[-4:], '  ' + catop(b)))

print('\n--- every frame addressed TO %s in that window, body in full ---' % port)
for t, p, b in rows[max(0, last_tx - n + 1):last_tx + 2]:
    if b[0:6] != mac or len(b) < 204:
        continue
    body = b[72:72 + 132]
    print('%9.3f from %-6s %s' % ((t - t0) / 1000.0, p, catop(b)))
    for o in range(0, 132, 16):
        print('    [%3d] %s' % (o, body[o:o + 16].hex()))
