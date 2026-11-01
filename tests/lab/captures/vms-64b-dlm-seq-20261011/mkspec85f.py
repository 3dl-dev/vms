#!/usr/bin/env python3
"""Write the rd vms-85f queued-answer specimens out of the committed captures."""
import gzip, hashlib, os, struct, sys
SCA=b'\x60\x07'; FLEN=204
def frames(path):
    op = gzip.open if path.endswith('.gz') else open
    with op(path,'rb') as fp: data=fp.read()
    off,idx=24,0
    while off+16<=len(data):
        _t,_u,cl,_o=struct.unpack('<IIII',data[off:off+16]); off+=16
        fr=data[off:off+cl]; off+=cl; idx+=1
        if len(fr)!=FLEN or fr[12:14]!=SCA: continue
        if fr[31]!=0x13 or fr[55]!=0x02: continue
        yield idx,fr
def get(path, want):
    for idx,fr in frames(path):
        if idx==want: return fr
    raise SystemExit("frame %d not found in %s"%(want,path))
def spec(out, name, cap, frameno, note, fr):
    lines = ["%OVMX-CLUSTER-SPECIMEN-1",
             "name:      %s"%name,
             "class:     scs-msg",
             "origin:    capture",
             "capture:   %s"%os.path.basename(cap),
             "frame:     %d"%frameno,
             'spec:      vms_cluster_codec_dlm.h "THE SAME RECORD WHEN THE MASTER QUEUED THE REQUEST INSTEAD" (rd vms-85f)',
             "wire-len:  204",
             "sha256:    %s"%hashlib.sha256(fr).hexdigest(),
             "%bytes"]
    for l in note.splitlines():
        lines.append("; "+l)
    for i in range(0,204,16):
        lines.append("@%-4d %s"%(i," ".join("%02x"%b for b in fr[i:i+16])))
    open(out,"w").write("\n".join(lines)+"\n")
    print("wrote",out)
root=sys.argv[1]
F=os.path.join(root,"tests/cluster/host/fixtures")
A=os.path.join(root,"tests/lab/captures/vms-64b-dlm-seq-20261011/order1-vax-first.pcap")
B=os.path.join(root,"tests/lab/captures/vms-d52c-ovmx-first-20261012/r3-ovmx-first.pcap")
spec(os.path.join(F,"dlm-queued-enq-req-a.spec"),"dlm-queued-enq-req-a",A,1039,
"""REAL captured frame 1039 of tests/lab/captures/vms-64b-dlm-seq-20261011/order1-vax-first.pcap.
A real OpenVMS VAX V7.3 node (08:00:2b:22:85:c2) asks VAX1 (aa:00:04:00:01:04, the
tree's MASTER) for EX on the ROOT resource 'UCX$INETACP_...': cat-0x02 op-0x01,
body[30] = 0x05, body[24:28] = 0e0002b2 (its own handle), body[36:44] all zero --
no parent, so this is a ROOT request. Its answer is dlm-queued-enq-ans-a.""",get(A,1039))
spec(os.path.join(F,"dlm-queued-enq-ans-a.spec"),"dlm-queued-enq-ans-a",A,1040,
"""REAL captured frame 1040 of the same capture: VAX1's answer, sent 0 frames later.
cat-0x82 op-0x01 with body[34] = 0xfb -- THE MASTER QUEUED IT. The request echoed,
with body[0:4] (the CM envelope, swapped), body[8] (the category), body[20:24]
(0x0f000272, the handle VAX1 minted), body[28:30] (0x14 0x00, the QUEUED record's
flags), body[32:36] (00 00 fb 00, the record) and body[52:54] (zeroed) rewritten,
and NOTHING ELSE -- the requester's own handle, the mode it asked for, the name and
body[36:44] all ride back unchanged. This is the frame OVMX now builds.""",get(A,1040))
spec(os.path.join(F,"dlm-queued-enq-req-b.spec"),"dlm-queued-enq-req-b",B,8245,
"""REAL captured frame 8245 of tests/lab/captures/vms-d52c-ovmx-first-20261012/r3-ovmx-first.pcap
-- a SECOND arrangement (heldout-proof-must-vary-topology): a different master
(08:00:2b:7a:2a:ac), a different requester, a different resource ('LNM$CWLOGICALS')
and a different mode (0x04, PW). Still a ROOT request: body[36:44] all zero.""",get(B,8245))
spec(os.path.join(F,"dlm-queued-enq-ans-b.spec"),"dlm-queued-enq-ans-b",B,8246,
"""REAL captured frame 8246: that second master's queued answer. Same fifteen
offsets rewritten, and it shows the two cells whose determinant is NOT grounded
taking their OTHER real values -- body[28:30] = 0x54 0x02 (not 0x14 0x00) and
body[52] = 0x02 (not 0). OVMX writes the commonest form (0x14 0x00 / 0x00); the
difference is a deviation row in frame-grounding.tsv, not a guess.""",get(B,8246))
