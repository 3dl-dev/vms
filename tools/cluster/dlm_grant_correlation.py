#!/usr/bin/env python3
"""dlm_grant_correlation.py -- HOW a real VMS master's grant is correlated to
the request it answers, and HOW LATE it may arrive (rd vms-f87).

WHY THIS TOOL EXISTS. rd vms-f87 asks OVMX to ORIGINATE a grant to a remote
waiter when the lock it was queued behind goes away -- the thing #1568/#1578
count as `deferred_grants_no_wire_op` and do not send, which left a real VAX
process in RWSCS indefinitely in the 2026-10-09 lab run. Originating it means
emitting a cat-0x82 at a system that did not JUST ask, and the arm's refusal to
do that rested on a claim about the protocol: that a grant is the answer to the
request it immediately follows (one dialogue, one transaction). That claim is
MEASURABLE against the captures already in hand, and this tool measures it,
rather than leaving the design resting on an assumption either way.

WHAT IT MEASURES, per capture:

  1. every cat-0x82 op-0x01 grant (outcome byte body[34] == 0xfa);
  2. the LAST cat-0x02 op-0x01/op-0x07 request from the grantee that carries the
     SAME requester handle at body[24:28] -- the correlation a real requester
     has available, since body[24:28] is its own lock id and the master echoes
     it back (spec 4(f).1, corrected by rd vms-b5b0);
  3. the GAP between them, how many SCA frames sat in between, and whether any
     op-0x03 release passed in that window;
  4. grants for which no such request exists at all -- a grant nobody asked
     for, which is what this executive is being asked to send.

Clean-room (Rule 8): it reads our own captures of a real OpenVMS VAX 7.3
reference cluster and public documentation only. It decodes nothing new -- the
offsets are the ones docs/cluster-protocol-spec.md 4(f).1 already grounds.

Usage:
    dlm_grant_correlation.py <pcap> [<pcap> ...]   measure
    dlm_grant_correlation.py --selftest            prove the measurement itself
"""
import os
import struct
import sys

# docs/cluster-protocol-spec.md 4(f).1, body-relative (body[0] = abs 72).
BODY = 72
OFF_CAT = 8
OFF_OP = 9
OFF_MASTER_LKID = 20      # the MASTER's handle (rd vms-b5b0)
OFF_REQ_LKID = 24         # the REQUESTER's own handle -- the correlation
OFF_MODE = 30
OFF_OUTCOME = 34          # 0xfa granted, 0xf9 you-master-it, 0xf8 redirect
OFF_NAMELEN = 47
OFF_NAME = 48
CAT_REQUEST = 0x02
RESPONSE_BIT = 0x80
OP_ENQ = 0x01
OP_DEQ = 0x03
OP_CONVERT = 0x07
OUTCOME_GRANTED = 0xFA
ETHERTYPE_SCA = 0x6007
FRAME_MIN = 204


def sca_frames(path):
    """Yield (index, timestamp, frame) for each SCA frame in a pcap."""
    try:
        f = open(path, "rb")
    except OSError:
        return
    with f:
        gh = f.read(24)
        if len(gh) < 24:
            return
        if gh[:4] == b"\xd4\xc3\xb2\xa1":
            end = "<"
        elif gh[:4] == b"\xa1\xb2\xc3\xd4":
            end = ">"
        else:
            return
        idx = 0
        while True:
            hdr = f.read(16)
            if len(hdr) < 16:
                return
            ts, tus, caplen, _ = struct.unpack(end + "IIII", hdr)
            data = f.read(caplen)
            if len(data) < caplen:
                return
            if len(data) < FRAME_MIN:
                continue
            if struct.unpack(">H", data[12:14])[0] != ETHERTYPE_SCA:
                continue
            idx += 1
            yield idx, ts + tus / 1e6, data


def _body(frame):
    return frame[BODY:]


def _name(b):
    n = b[OFF_NAMELEN]
    return bytes(b[OFF_NAME:OFF_NAME + n]) if 0 < n < 32 else b""


def measure(path):
    """Return (rows, orphans) for one capture.

    rows:    one per correlated grant -- (gap_s, frames_between, release_between,
             grant_index, request_index, mode, name)
    orphans: grants with no matching request from that peer at all.
    """
    frames = list(sca_frames(path))
    rows = []
    orphans = []
    seen = {}            # (src, dst, req_handle) -> (index, t, position)
    for pos, (idx, t, frame) in enumerate(frames):
        b = _body(frame)
        src, dst = frame[6:12], frame[0:6]
        cat, op = b[OFF_CAT], b[OFF_OP]
        if cat == CAT_REQUEST and op in (OP_ENQ, OP_CONVERT):
            key = (src, dst, bytes(b[OFF_REQ_LKID:OFF_REQ_LKID + 4]))
            seen[key] = (idx, t, pos, b[OFF_MODE], _name(b))
            continue
        if cat != (CAT_REQUEST | RESPONSE_BIT) or op != OP_ENQ:
            continue
        if b[OFF_OUTCOME] != OUTCOME_GRANTED:
            continue
        key = (dst, src, bytes(b[OFF_REQ_LKID:OFF_REQ_LKID + 4]))
        if key not in seen:
            orphans.append(idx)
            continue
        ridx, rt, rpos, rmode, rname = seen[key]
        release = None
        for m in range(rpos + 1, pos):
            qb = _body(frames[m][2])
            if qb[OFF_CAT] == CAT_REQUEST and qb[OFF_OP] == OP_DEQ:
                release = frames[m][0]
        rows.append((t - rt, pos - rpos - 1, release, idx, ridx, rmode, rname))
    return rows, orphans


def report(paths):
    total = 0
    total_orphans = 0
    worst = []
    for p in paths:
        rows, orphans = measure(p)
        if not rows and not orphans:
            continue
        total += len(rows)
        total_orphans += len(orphans)
        # Sort on the gap alone: `release_between` is None for most rows and
        # tuple ordering would compare None with an int.
        rows.sort(key=lambda r: r[0], reverse=True)
        if rows:
            worst.append((rows[0][0], os.path.basename(p), rows[0]))
        print("%-44s grants=%-6d orphans=%-4d max gap=%.3fs" % (
            os.path.basename(p), len(rows), len(orphans),
            rows[0][0] if rows else 0.0))
    worst.sort(key=lambda w: w[0], reverse=True)
    print()
    print("=== %d correlated grants, %d grants nobody asked for ===" % (
        total, total_orphans))
    print("the latest answers seen:")
    for gap, name, r in worst[:10]:
        print("  %-40s gap=%7.3fs frames_between=%-6d release_between=%s "
              "mode=%d name=%r" % (name, gap, r[1], r[2], r[5], r[6][:14]))
    print()
    print("READ IT LIKE THIS. `orphans` is the number of grants a real master")
    print("sent that answer no outstanding request from that peer: it is the")
    print("shape rd vms-f87 needs grounded. A large `max gap` with frames in")
    print("between says a grant need NOT be the next frame on the connection,")
    print("which is a weaker but real form of the same fact.")
    return 0


def selftest():
    """Build a pcap of three frames -- request, unrelated traffic, grant -- and
    prove the measurement finds the correlation ACROSS the gap, and that it
    reports an uncorrelated grant as an orphan. The measurement is what the
    design argument rests on, so it is tested rather than trusted."""
    import tempfile

    A = bytes.fromhex("aa0004000104")
    B = bytes.fromhex("aa0004000204")

    def frame(src, dst, cat, op, req_lkid, outcome=0, mode=0):
        f = bytearray(FRAME_MIN)
        f[0:6] = dst
        f[6:12] = src
        f[12:14] = struct.pack(">H", ETHERTYPE_SCA)
        b = BODY
        f[b + OFF_CAT] = cat
        f[b + OFF_OP] = op
        f[b + OFF_REQ_LKID:b + OFF_REQ_LKID + 4] = struct.pack("<I", req_lkid)
        f[b + OFF_OUTCOME] = outcome
        f[b + OFF_MODE] = mode
        return bytes(f)

    def pcap(path, rows):
        with open(path, "wb") as f:
            f.write(struct.pack("<IHHiIII", 0xA1B2C3D4, 2, 4, 0, 0, 65535, 1))
            for t, fr in rows:
                f.write(struct.pack("<IIII", int(t), int((t % 1) * 1e6),
                                    len(fr), len(fr)))
                f.write(fr)

    ok = True
    with tempfile.TemporaryDirectory() as d:
        p = os.path.join(d, "t.pcap")
        pcap(p, [
            (100.0, frame(A, B, CAT_REQUEST, OP_ENQ, 0x1111, mode=5)),
            (100.5, frame(A, B, CAT_REQUEST, OP_ENQ, 0x2222, mode=1)),
            (100.6, frame(B, A, CAT_REQUEST | RESPONSE_BIT, OP_ENQ, 0x2222,
                          outcome=OUTCOME_GRANTED)),
            (103.0, frame(A, B, CAT_REQUEST, OP_DEQ, 0x9999)),
            (105.0, frame(B, A, CAT_REQUEST | RESPONSE_BIT, OP_ENQ, 0x1111,
                          outcome=OUTCOME_GRANTED)),
            (106.0, frame(B, A, CAT_REQUEST | RESPONSE_BIT, OP_ENQ, 0x7777,
                          outcome=OUTCOME_GRANTED)),
        ])
        rows, orphans = measure(p)
        gaps = sorted(r[0] for r in rows)
        if len(rows) != 2:
            print("FAIL: expected 2 correlated grants, got", len(rows))
            ok = False
        elif abs(gaps[1] - 5.0) > 0.01 or abs(gaps[0] - 0.1) > 0.01:
            print("FAIL: gaps are", gaps)
            ok = False
        else:
            print("ok: a grant 5 s and 3 frames after its request is still "
                  "correlated to it, by the requester's own handle")
        late = [r for r in rows if r[0] > 1.0]
        if not late or late[0][2] is None:
            print("FAIL: the op-0x03 release between them was not reported")
            ok = False
        else:
            print("ok: and the release that passed in between is named")
        if orphans != [6]:
            print("FAIL: expected the grant nobody asked for at frame 6, got",
                  orphans)
            ok = False
        else:
            print("ok: a grant answering no outstanding request is reported as "
                  "an orphan, which is the shape rd vms-f87 needs")
    print("SELFTEST", "PASSED" if ok else "FAILED")
    return 0 if ok else 1


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    if argv[1] == "--selftest":
        return selftest()
    return report(argv[1:])


if __name__ == "__main__":
    sys.exit(main(sys.argv))
