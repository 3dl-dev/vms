# unans.py <tools/cluster> <pcap> [dst-mac-tail]: requests (txn != 0, no 0x80 bit) never answered by their recipient
import sys
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A
want = sys.argv[3] if len(sys.argv) > 3 else None
frames = [(ts, f) for ts, f in A.read_pcap(sys.argv[2]) if A.is_cm(f)]
t0 = frames[0][0] if frames else 0
reqs = {}
for ts, f in frames:
    b = f[A.OFF_CM_BODY:A.OFF_CM_BODY + 132]
    txn = b[4:6]
    if b[8] & 0x80:
        reqs.pop((f[0:6], f[6:12], bytes(b[4:8])), None)
    elif txn != b'\x00\x00' and b[8] not in (0x04,):
        reqs[(f[6:12], f[0:6], bytes(b[4:8]))] = (ts, b)
for (s, d, k), (ts, b) in sorted(reqs.items(), key=lambda x: x[1][0]):
    if want and not d.hex().endswith(want):
        continue
    print('%8.3f %s -> %s %02x/%02x %s' % (ts - t0, s.hex()[-4:], d.hex()[-4:], b[8], b[9], b[10:48].hex()))
