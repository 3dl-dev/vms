# xfer04.py <tools/cluster> <pcap>: for each 0b/04 request, show the block transfer that follows on the same
# pair (REQUEST DATA b55=0x08, DATA b55=0x0d / SEND DATA...) and the 8b/04 answer.
import sys
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A
frames = list(A.read_pcap(sys.argv[2])); t0 = frames[0][0]
open_req = {}
for ts, f in frames:
    if not A.is_sca(f):
        continue
    s, d = f[6:12], f[0:6]
    if A.is_cm(f):
        b = f[72:204]
        if b[8] == 0x0b and b[9] == 0x04:
            open_req[(s, d)] = (ts, b)
            print('\n%9.3f REQ %s->%s %s' % (ts - t0, s.hex()[-4:], d.hex()[-4:], b[10:48].hex()))
        elif b[8] == 0x8b and b[9] == 0x04 and (d, s) in open_req:
            print('%9.3f ANS %s->%s %s' % (ts - t0, s.hex()[-4:], d.hex()[-4:], b[10:72].hex()))
            del open_req[(d, s)]
        continue
    if (s, d) in open_req or (d, s) in open_req:
        print('%9.3f  sca b55=%02x len=%d %s->%s %s' % (ts - t0, f[55], len(f), s.hex()[-4:], d.hex()[-4:], f[56:].hex()[:200]))
