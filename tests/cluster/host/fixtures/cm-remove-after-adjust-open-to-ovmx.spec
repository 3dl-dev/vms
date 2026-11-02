%OVMX-CLUSTER-SPECIMEN-1
name:      cm-remove-after-adjust-open-to-ovmx
class:     scs-msg
origin:    capture
capture:   c350-removenode-cm.pcap
spec:      docs/cluster-protocol-spec.md 4(r) (rd vms-c350)
wire-len:  204
sha256:    22fbef5e6c678cd8e7a85e3085e25fa00502bfdf78dbd7a77d1a0aab1aa5be25
%bytes
; REAL captured frame 302 of the same capture: VAX2's class-0x03 removal of VAX1, cat 0x01 op 0x08, to OVMXE,
; after the adjustment. body[22:24] = 1: the quorum the adjustment left, kept by the next coordinator
; although max{EXPECTED_VOTES} is still 3 (OVMXE's).
@0    52 54 00 00 e5 01 08 00 2b a4 02 df 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 02 04 4b 13
@32   80 00 29 01 01 00 12 00 80 00 00 00 29 01 00 00
@48   80 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   07 00 bf 85 0d 00 52 2f 17 01 6f 00 09 00 1c 1f
@80   01 08 00 00 2c 00 0e 03 40 03 00 00 04 00 01 00
@96   03 00 00 00 02 01 00 00 a0 44 e5 fd 04 4e bc 00
@112  a0 e1 ef 20 08 4e bc 00 00 01 04 00 00 00 00 0c
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 03
@160  00 00 00 01 00 30 2e 37 03 00 00 00 00 00 00 00
@176  00 00 00 60 ee 78 de ff ff ff 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
