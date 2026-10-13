#!/usr/bin/env python3
"""dbpush.py <pcap> [outdir] -- rd vms-94a: reassemble every clusterwide logical-name DATABASE PUSH.
A member answers a booting node's cat-0x0b op-0x05 with a cat-0x0b op-0x06 naming a buffer
(body[16:20] name, [20:24] the recipient's Con.ID, [32:36] length); the recipient REQUEST DATAs it
and the bytes arrive in one or more DATA (0x0d) frames. Each DATA frame's 28-byte block header
(abs 56) is: dest Con.ID, two tag words, bytes remaining, source name, source offset, destination
name, destination offset; the data follows at abs 84. Prints each push and its sub-records."""
import os, struct, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../../../tools/cluster'))
import cm_dialogue_audit as A  # noqa: E402


def pushes(path):
    frames = list(A.read_pcap(path)); t0 = frames[0][0]
    want = {}            # (holder mac, puller mac) -> [t, src_name, len, bytearray, got]
    for ts, f in frames:
        if not A.is_sca(f) or len(f) < 84:
            continue
        s, d = f[6:12], f[0:6]
        if f[55] == 0x02 and A.is_cm(f) and f[80] == 0x0b and f[81] == 0x06:
            name, ln = struct.unpack_from('<I', f, 72 + 16)[0], struct.unpack_from('<I', f, 72 + 32)[0]
            want[(s, d)] = [ts - t0, name, ln, bytearray(ln), 0]
        elif f[55] in (0x0c, 0x0d) and (s, d) in want:
            w = want[(s, d)]
            src_name, src_off = struct.unpack_from('<II', f, 56 + 12)
            if src_name != w[1]:
                continue
            data = f[84:]
            n = min(len(data), w[2] - src_off)
            w[3][src_off:src_off + n] = data[:n]
            w[4] += n
            if w[4] >= w[2]:
                yield w[0], s.hex()[-4:], d.hex()[-4:], bytes(w[3])
                del want[(s, d)]


def split(db):
    """The container: a 20-byte header, then records each led by its own u16 length."""
    hdr = db[:20]
    recs, o = [], 20
    while o + 4 <= len(db):
        ln = struct.unpack_from('<H', db, o)[0]
        if ln == 0 or o + ln > len(db):
            break
        recs.append(db[o:o + ln]); o += ln
    return hdr, recs, len(db) - o


if __name__ == '__main__':
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import lnmrec
    for t, s, d, db in pushes(sys.argv[1]):
        hdr, recs, rest = split(db)
        print('%9.3f %s->%s %d bytes hdr=%s records=%d trailing=%d' % (t, s, d, len(db), hdr.hex(), len(recs), rest))
        for r in recs:
            try:
                print('     ', lnmrec.decode(r))
            except Exception as e:
                print('      UNDECODED %d bytes (%s) %s' % (len(r), e, r[:16].hex()))
        if len(sys.argv) > 2:
            open(os.path.join(sys.argv[2], 'db-%s-%s-%d.bin' % (s, d, int(t))), 'wb').write(db)
