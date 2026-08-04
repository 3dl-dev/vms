#!/usr/bin/env python3
"""
scs_seqgap_measure.py -- vms-abc: how often does the p. 2-31 SEQUENTIALITY
guarantee actually fail on the reference wire?

WHY THIS EXISTS. vms-abc makes OVMX BREAK a virtual circuit when it detects a
gap in the peer's SCS send-sequence numbers. That is a fail-loud change: if the
detector fires on a wire that is in fact healthy, OVMX would tear down working
circuits. Before shipping the detector, run it -- unchanged in rule -- over
every capture this project holds and count how many times it would have fired.

THE DETECTOR, stated exactly (same rule as scs_vc_note_recv_checked() in
src/vmsscs/scs_vc.c):

    a received SEQUENCED MESSAGE (SCS envelope format constant 0x13 at payload
    [17], send_seq at [20:22] != 0, opcode NOT 0x41) whose send_seq is
    "ahead" of recv_seq by more than 1 is a GAP of (send_seq - recv_seq - 1)
    missing messages. send_seq <= recv_seq is a retransmit/duplicate and is NOT
    a gap.  Comparison is modular over 16 bits.

VC SCOPE. One counter per ORDERED (src,dst) MAC pair, reset to 0 when a 0x41
START frame is seen in either direction of that pair -- spec sec 4i.A: "the
post-START SCS VC resets to send_seq = 1 on both sides", GROUNDED.  0x41 frames
themselves are excluded from the sequence check, exactly as scsd.c's credit
block excludes them (they carry the separate config-round counters of sec 4g
phase 2, and an established member's round-0 START carries a continuation value
in the thousands -- sec 4i.A).

Usage:  scs_seqgap_measure.py <pcap> [<pcap> ...]
        scs_seqgap_measure.py --all      (every .pcap in the lab capture dir)
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from dissect_sca import read_pcap  # noqa: E402

SCA_ETHERTYPE = 0x6007
FORMAT_CONST = 0x13
OP_START = 0x41

LAB_CAPTURES = "/data/training/vax/cluster/captures"


def seq_ahead(a, b):
    """1 if 16-bit sequence `a` is strictly ahead of `b` (modular)."""
    return ((a - b) & 0xFFFF) != 0 and ((a - b) & 0xFFFF) < 0x8000


def scan(path):
    frames = read_pcap(path)
    recv = {}          # (src,dst) -> recv_seq high-water
    gaps = []          # (frame_idx, src, dst, recv_seq, send_seq, missing)
    anchors = []       # first sequenced message seen on each VC (never a gap)
    seqd = 0           # sequenced messages examined
    dups = 0           # send_seq <= recv_seq (retransmit / duplicate)
    inorder = 0        # send_seq == recv_seq + 1
    starts = 0
    for idx, (_s, _u, _o, fr) in enumerate(frames):
        if len(fr) < 36:
            continue
        if ((fr[12] << 8) | fr[13]) != SCA_ETHERTYPE:
            continue
        dst = fr[0:6].hex()
        src = fr[6:12].hex()
        pl = fr[14:]
        if len(pl) < 22:
            continue
        opcode = pl[16]
        if opcode == OP_START:
            starts += 1
            recv[(src, dst)] = 0
            recv[(dst, src)] = 0
            continue
        if pl[17] != FORMAT_CONST:
            continue
        send_seq = pl[20] | (pl[21] << 8)
        if send_seq == 0:
            continue  # a 0x48 credit-return: pure ack, no new sequence
        seqd += 1
        key = (src, dst)
        if key not in recv:
            # ANCHOR. A capture (and OVMX itself, when it attaches to a circuit
            # that is already carrying traffic) sees its first sequenced message
            # at whatever the peer's counter happens to be. That first message
            # ESTABLISHES the baseline; it can never be a gap, because nothing
            # is known about what preceded it.
            recv[key] = send_seq
            anchors.append((idx, src, dst, send_seq))
            continue
        cur = recv[key]
        if not seq_ahead(send_seq, cur):
            dups += 1
            continue
        missing = ((send_seq - cur) & 0xFFFF) - 1
        if missing == 0:
            inorder += 1
        else:
            gaps.append((idx, src, dst, cur, send_seq, missing))
        recv[key] = send_seq
    return {"path": path, "frames": len(frames), "starts": starts,
            "sequenced": seqd, "inorder": inorder, "dups": dups, "gaps": gaps,
            "anchors": anchors}


def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__)
        return 2
    if args == ["--all"]:
        args = sorted(os.path.join(LAB_CAPTURES, f)
                      for f in os.listdir(LAB_CAPTURES) if f.endswith(".pcap"))
    total_seq = total_gap = total_dup = 0
    for p in args:
        try:
            r = scan(p)
        except Exception as exc:  # noqa: BLE001 -- a bad pcap must not hide the rest
            print(f"{os.path.basename(p):<62} SKIP ({exc})")
            continue
        total_seq += r["sequenced"]
        total_gap += len(r["gaps"])
        total_dup += r["dups"]
        print(f"{os.path.basename(p):<62} frames={r['frames']:>6} "
              f"seqmsg={r['sequenced']:>6} inorder={r['inorder']:>6} "
              f"dup/retx={r['dups']:>5} anchors={len(r['anchors']):>3} starts={r['starts']:>4} "
              f"GAPS={len(r['gaps'])}")
        for (idx, src, dst, cur, ss, miss) in r["gaps"][:8]:
            print(f"    gap @frame {idx}: {src[-4:]}->{dst[-4:]} "
                  f"recv_seq={cur} send_seq={ss} missing={miss}")
    print(f"\nTOTAL sequenced messages={total_seq} dup/retx={total_dup} GAPS={total_gap}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
