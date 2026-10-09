%OVMX-CLUSTER-SPECIMEN-1
name:      cm-depart-commit-to-ovmx
class:     scs-msg
origin:    capture
capture:   m4-crashwindow.pcap
spec:      docs/cluster-protocol-spec.md 4(r) (rd vms-e8b)
wire-len:  204
sha256:    704bc286cdfcea539d3aa130401446b203b451dcbc62ec52ed0cef79de9e8e33
%bytes
; REAL captured frame 90 (0-based) of the same capture: the SAME departure commit, sent by
; VAX1 to the OVMX node (OVMXE, 1030) on OVMXE's own connection (Con.ID c6ee000d ->
; 8e0f0009). txn 9, token 46657, epoch 51249195. This is the request OVMX answered TO VAX2,
; which bugchecked VAX2 CNXMGRERR 378 us later (rd vms-e8b).
@0    52 54 00 00 e5 01 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 01 04 4b 13
@32   d0 01 45 02 01 00 12 00 d0 01 00 00 45 02 00 00
@48   d0 01 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 0f 8e 0d 00 ee c6 b9 01 44 01 09 00 41 b6
@80   01 03 00 00 2b 00 0e 03 20 04 00 00 80 73 5a 59
@96   f2 3a bc 00 00 00 00 00 00 00 f9 00 00 00 00 00
@112  00 00 00 00 00 00 00 16 46 31 31 42 24 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 59 00 00 00 00 00
@144  10 00 00 00 03 00 00 00 00 00 00 00 56 01 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 01 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff 51 b4 e6 84
