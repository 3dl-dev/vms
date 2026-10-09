%OVMX-CLUSTER-SPECIMEN-1
name:      cm-step1-ack-oracle
class:     scs-msg
origin:    capture
capture:   XF-barrier.pcap
spec:      docs/cluster-protocol-spec.md 4(p) barrier table; rd vms-f297
wire-len:  204
sha256:    9b7c5ad27c92479dab0c4f916d59e5629c96fffa18f46566490f3e56b69c514f
%bytes
; REAL captured frame 2 of tests/lab/captures/vms-f297-open-cells-20261008/XF-barrier.pcap
; VAX1's 0x81/0x0b acknowledgement of that report: the request echoed, body[8] |= 0x80,
; body[16:19] = 10 02 01 -- role 0x10, the transition CLASS 2, the response marker 0x01.
@0    08 00 2b 92 0b 09 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 5b 13
@32   8c 00 39 01 01 00 12 00 8c 00 00 00 39 01 00 00
@48   8c 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0a 00 ef 75 0a 00 53 2f 0b 01 5e 00 03 00 12 65
@80   81 0b 03 00 03 00 00 00 10 02 01 00 00 00 00 00
@96   00 00 00 00 00 00 00 00 00 00 00 00 02 00 00 00
@112  00 00 00 00 00 00 00 00 6e 00 00 00 25 00 60 00
@128  c0 24 f9 7f 7e 00 00 00 c0 24 f9 7f 77 00 00 00
@144  c0 24 f9 7f 70 00 00 00 c0 24 f9 7f 00 00 00 00
@160  00 41 00 00 00 00 d7 01 01 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
