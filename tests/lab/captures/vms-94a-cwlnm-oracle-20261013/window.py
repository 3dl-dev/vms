# window.py <tools/cluster> <pcap> <t_from> <t_to> [mac-tail...]: every SCA frame in a time window (relative to the
# first frame), with its SCA message type byte (abs 55?) and length; CM bodies show cat/op.
import sys
sys.path.insert(0, sys.argv[1])
import cm_dialogue_audit as A
frames = list(A.read_pcap(sys.argv[2])); t0 = frames[0][0]
lo, hi = float(sys.argv[3]), float(sys.argv[4]); macs = sys.argv[5:]
for ts, f in frames:
    t = ts - t0
    if t < lo or t > hi or not A.is_sca(f):
        continue
    s, d = f[6:12].hex()[-4:], f[0:6].hex()[-4:]
    if macs and not (s in macs and d in macs):
        continue
    extra = ''
    if A.is_cm(f):
        b = f[72:]
        extra = 'CM %02x/%02x txn=%s' % (b[8], b[9], b[4:8].hex())
    print('%9.4f %s->%s len=%4d b30=%02x b31=%02x b55=%02x %s %s' % (t, s, d, len(f), f[30], f[31], f[55] if len(f) > 55 else 0, extra, f[56:80].hex()))
