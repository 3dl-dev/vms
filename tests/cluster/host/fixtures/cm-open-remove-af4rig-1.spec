%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-remove-af4rig-1
class:     scs-msg
origin:    capture
capture:   af4-op08-af4rig.pcap
spec:      docs/cluster-protocol-spec.md 4(p).R (rd vms-af4)
wire-len:  204
sha256:    3f4bca1ddbd02661dc5ae2b6b5c77f444f4c072c08af2da4b726bd771174d145
%bytes
; REAL captured frame 1 of tests/lab/captures/vms-af4-op08-remove-20261001/af4-op08-af4rig.pcap
; cat-0x01 op-0x08 class-0x03 REMOVE open, SCSSYSTEMID 1989 (coordinator) -> 1987.
; 3 members (VAXC csid 1, OVMXA 2, OVMXB 3); OVMXB (slot 3) SIGKILLed and removed; the transition keeps slots 1,2
@0    52 54 00 00 df 0a 08 00 2b ba cf 26 60 07 bc 00
@16   aa 00 04 00 c3 07 01 01 aa 00 04 00 c5 07 4b 13
@32   f6 01 c5 02 01 00 12 00 f6 01 00 00 c5 02 00 00
@48   f6 01 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   06 00 e2 d2 09 00 e9 db 92 02 c3 01 07 00 38 cb
@80   01 08 00 00 05 00 00 00 40 03 00 00 04 00 02 00
@96   03 00 00 00 02 02 00 00 c0 ae 26 5c 53 35 bc 00
@112  60 b0 35 c9 53 35 bc 00 00 c5 07 00 00 00 00 06
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 03
@160  00 00 00 03 00 00 00 00 03 00 01 00 02 00 02 00
@176  03 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 9e b0 8a 9d
