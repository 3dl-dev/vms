%OVMX-CLUSTER-SPECIMEN-1
name:      cm-eb3-f6-phase1-open
class:     scs-msg
origin:    capture
capture:   eb3-F6-20260930.pcap
spec:      docs/cluster-protocol-spec.md 4(j)/4(p) + rd vms-eb3 oracle F5/F6
wire-len:  204
sha256:    3939f9a193d5edef7b55e6fc54648db4fe365fd09ad0076d8865f983019faf20
%bytes
; REAL captured frame 471 of tests/lab/captures/vms-eb3-joiner-freeze-20260930/eb3-F6-20260930.pcap
; The coordinator's Phase-1 transition OPEN (cat-0x01 op-0x09, class ADD) to the joiner.
; The joiner answered it with cat-0x81 op-0x09 smsg 94 ack 264 and was then SIGSTOPped for 14 s (fz.sh F6).
@0    08 00 2b d7 8e 3f 08 00 2b c4 ad 97 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 03 04 4b 13
@32   78 00 25 01 01 00 12 00 78 00 00 00 25 01 00 00
@48   78 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0a 00 1b 64 0d 00 52 2f 08 01 5a 00 07 00 79 1e
@80   01 09 58 63 04 00 00 00 40 02 00 00 04 00 02 00
@96   03 00 01 00 01 01 00 00 00 d0 ef c6 4b 34 bc 00
@112  e0 c1 b2 ff 4b 34 bc 00 00 03 04 00 00 00 00 0e
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 02
@160  00 00 00 00 02 00 00 00 02 00 01 00 02 00 02 00
@176  03 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 08 cf 00 87 00 00 00 00
