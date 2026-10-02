%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-b36or-1
class:     scs-msg
origin:    capture
capture:   af4-op08-b36or.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    cbf928f4d0bbd3457db95eaeae86286b527eedae414e754816ee974990fef256
%bytes
; REAL captured frame 1 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-b36or.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1025 (coordinator) -> 1026.
; 3 real VAX V7.3 members (VAX1 csid 1, VAX2 2, VAX3 3); VAX3 (slot 3) removed; the transition keeps slots 1,2
@0    08 00 2b 9c d9 04 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   fb 32 aa 23 01 00 12 00 fb 32 00 00 aa 23 00 00
@48   fb 32 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   08 00 52 2f 08 00 52 2f ce 22 20 32 04 00 52 20
@80   01 08 00 00 06 00 00 00 40 03 00 00 04 00 01 00
@96   03 07 01 00 02 01 00 00 20 ca 91 dc 8c 30 bc 00
@112  e0 d8 a8 40 8e 30 bc 00 00 01 04 00 00 00 00 06
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 03
@160  00 00 00 01 00 00 01 00 03 00 00 00 00 00 00 00
@176  00 41 00 60 ee 78 de ff ff ff 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff ff 95 3e 8f
