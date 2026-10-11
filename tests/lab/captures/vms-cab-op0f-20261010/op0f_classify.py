#!/usr/bin/env python3
"""op0f_classify.py <pcap>... -- rd vms-cab: score real op-0x0f / 82-0x15 pairs
against the answer rule OVMX implements (vms_dlm_op0f_ack_build).

Pairs: cat 0x02 op 0x0f request and cat 0x82 op 0x15 response between the same
two MACs with equal body[4:8] and body[20:28]; deduplicated by content. OVMX
(52:54:00:*) frames are excluded: only real VMS<->VMS pairs are scored.

INTEREST: a responder has observed interest in the request's resource name if,
anywhere earlier in the same capture, it sent or was sent a cat-0x02 request
with a lock-acquiring op -- 0x01 ENQ, 0x06/0x07 CONVERT, 0x0d
registration -- naming that resource. (op 0x0e, sent alongside 0x0f, is not.)

RULE (what OVMX sends): the request echoed, cat 0x82, op 0x15, body[28:32]=0,
body[32:36]=01 00 fa 00; compared over body[4:132] (body[0:4] is the envelope).
OVMX sends it only when its lock engine holds no lock on the resource -- the
no-interest class here.
"""
import struct, sys, hashlib, collections

def pairs(path):
    d = open(path, 'rb').read(); off = 24; reqs = {}; hist = collections.defaultdict(set)
    while off + 16 <= len(d):
        ts, tus, cl, _ = struct.unpack("<IIII", d[off:off+16]); off += 16
        f = d[off:off+cl]; off += cl
        if f[12:14] != b"\x60\x07": continue
        s = f[14:]
        if len(s) < 58 + 132 or struct.unpack("<H", s[46:48])[0] != 10: continue
        b = bytes(s[58:58+132]); src = f[6:12].hex(); dst = f[0:6].hex()
        if src.startswith('525400') or dst.startswith('525400'): continue
        nm = bytes(b[48:48+min(b[47], 31)])
        if b[8] == 2 and b[9] in (0x01, 0x06, 0x07, 0x0d):
            hist[nm].add(src); hist[nm].add(dst)
        if b[8] == 2 and b[9] == 0x0f:
            reqs[(src, dst, b[4:8], b[20:28])] = b
        elif b[8] == 0x82 and b[9] == 0x15:
            q = reqs.pop((dst, src, b[4:8], b[20:28]), None)
            if q is not None:
                yield q, b, (src in hist[bytes(q[48:48+min(q[47], 31)])])

def rule(q):
    e = bytearray(q); e[8] = 0x82; e[9] = 0x15
    e[28:32] = b'\0\0\0\0'; e[32:36] = b'\x01\x00\xfa\x00'
    return bytes(e)

seen = set(); c = collections.Counter(); bad = []
for p in sys.argv[1:]:
    for q, b, interest in pairs(p):
        h = hashlib.sha1(q + b).digest()
        if h in seen: continue
        seen.add(h)
        ok = rule(q)[4:] == b[4:]
        c[(interest, ok)] += 1
        if not interest and not ok: bad.append((p, q.hex(), b.hex()))
n0 = c[(False, True)] + c[(False, False)]
print("pairs %d" % sum(c.values()))
print("no-interest responders (OVMX answers): %d of %d match the rule" % (c[(False, True)], n0))
print("interest responders (OVMX withholds): %d match, %d differ" % (c[(True, True)], c[(True, False)]))
for x in bad: print("MISS", x)
sys.exit(0 if n0 > 0 and not bad else 1)
