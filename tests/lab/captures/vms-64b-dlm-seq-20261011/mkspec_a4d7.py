#!/usr/bin/env python3
"""Write the rd vms-a4d7 DEFERRED-GRANT specimens out of the committed captures.

Two arrangements (heldout-proof-must-vary-topology): two different masters, two
different requesters, two different modes, two captures. Each specimen is the
cat-0x02 op-0x02 a real master sent AFTER it had answered that requester's
op-0x01 with 0xfb QUEUED on the same (master handle, requester handle) pair.
"""
import gzip, hashlib, os, struct, sys
SCA = b'\x60\x07'
FLEN = 204


def frames(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path, 'rb') as fp:
        data = fp.read()
    off, idx = 24, 0
    while off + 16 <= len(data):
        _t, _u, cl, _o = struct.unpack('<IIII', data[off:off + 16])
        off += 16
        fr = data[off:off + cl]
        off += cl
        idx += 1
        if len(fr) != FLEN or fr[12:14] != SCA:
            continue
        if fr[31] != 0x13 or fr[55] != 0x02:
            continue
        yield idx, fr


def get(path, want):
    for idx, fr in frames(path):
        if idx == want:
            return fr
    raise SystemExit("frame %d not found in %s" % (want, path))


def spec(out, name, cap, frameno, note, fr):
    lines = ["%OVMX-CLUSTER-SPECIMEN-1",
             "name:      %s" % name,
             "class:     scs-msg",
             "origin:    capture",
             "capture:   %s" % os.path.basename(cap),
             "frame:     %d" % frameno,
             'spec:      vms_cluster_codec_dlm.h "THE DEFERRED GRANT (op 0x02)" (rd vms-a4d7)',
             "wire-len:  204",
             "sha256:    %s" % hashlib.sha256(fr).hexdigest(),
             "%bytes"]
    for l in note.splitlines():
        lines.append("; " + l)
    for i in range(0, FLEN, 16):
        lines.append("@%-4d %s" % (i, " ".join("%02x" % b for b in fr[i:i + 16])))
    open(out, "w").write("\n".join(lines) + "\n")
    print("wrote", out)


root = sys.argv[1]
F = os.path.join(root, "tests/cluster/host/fixtures")
A = os.path.join(root, "tests/lab/captures/vms-64b-dlm-seq-20261011/order1-vax-first.pcap")
B = os.path.join(root, "tests/lab/captures/vms-d52c-ovmx-first-20261012/r3-ovmx-first.pcap")

spec(os.path.join(F, "dlm-grant-notify-a.spec"), "dlm-grant-notify-a", A, 3587,
"""REAL captured frame 3587 of tests/lab/captures/vms-64b-dlm-seq-20261011/order1-vax-first.pcap,
on a private real OpenVMS VAX V7.3 cluster. VAX1 (aa:00:04:00:01:04), the MASTER
of 'F11B..VAX1DATA', had answered 08:00:2b:22:85:c2's op-0x01 for PW with 0xfb
QUEUED seven frames earlier (frame 3580, body[20:24] = ec03001c, body[24:28] =
2d010022, body[30] = 04). THIS frame is what the master sent next about that lock:
cat-0x02 op-0x02, the SAME handle pair at body[20:28], body[4:8] all zero (no
transaction -- nobody answers one), body[28:30] = 14 00, body[30] = body[31] = 04
(the mode it had queued for), body[47] = 0 (no resource name: keyed by HANDLES),
body[112:128] = 00000000 ffffffff 00000000 ffffffff. The residue spans
(body[10:12], [16:20], [32:48], [48:112], [128:132]) are this sender's own stale
message buffer and OVMX writes zeros there (rd vms-42de) -- body[16:20] reads
28440080 here, the S0 kernel address 0x80004428 showing through.""", get(A, 3587))

spec(os.path.join(F, "dlm-grant-notify-b.spec"), "dlm-grant-notify-b", B, 7812,
"""REAL captured frame 7812 of tests/lab/captures/vms-d52c-ovmx-first-20261012/r3-ovmx-first.pcap
-- a SECOND arrangement: a DIFFERENT master (08:00:2b:7a:2a:ac), a different
requester (08:00:2b:03:7e:61), a different resource ('QMAN..EW_JOBCTL') and a
different mode (EX, not PW). That master had answered the requester's op-0x01 with
0xfb QUEUED one frame earlier (frame 7811); this is the deferred grant, with
body[30] = body[31] = 05 and the same structural cells as specimen A.""", get(B, 7812))
