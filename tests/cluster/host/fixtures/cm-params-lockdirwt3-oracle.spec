%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-lockdirwt3-oracle
class:     scs-msg
origin:    capture
capture:   fcb-L1-params.pcap
spec:      docs/cluster-protocol-spec.md 4(j) "LOCKDIRWT -- GROUNDED by controlled reconfiguration" (rd vms-fcb)
wire-len:  204
sha256:    59622fdd167f4ba27d50378385f48fe83c0b6ce8de9bf158f4500d45a8d6600e
%bytes
; REAL captured cat-0x01 op-0x01 cluster-parameters record (frame 1 of
; tests/lab/captures/vms-fcb-lockdirwt-20261004/fcb-L1-params.pcap) sent by a real OpenVMS VAX V7.3
; VAX1 (SCSSYSTEMID 1025, the founder), booted conversationally with SYSBOOT> SET LOCKDIRWT 3
; and read back with SYSBOOT> SHOW LOCKDIRWT on its own console (L1-vax1.console.log).
;
; THE FIELD: body[26:28] (abs 98) = 03 00 -- the sender's LOCKDIRWT 3.
;   body[18:20] (abs 90) = 01 00  member count (rd vms-e88)
;   body[22:24] (abs 94) = 01 00  VOTES (sec 4(j))
@0    08 00 2b 92 0b 09 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   0a 00 0d 00 01 00 12 00 0a 00 00 00 0d 00 00 00
@48   0a 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   09 00 db cb 0a 00 52 2f 02 00 00 00 00 00 00 00
@80   01 01 00 00 21 50 00 00 00 00 01 00 01 00 01 00
@96   01 00 03 00 00 99 05 8c c7 37 bc 00 00 99 05 8c
@112  c7 37 bc 00 02 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 99 05 8c c7 37 bc 00
@144  10 00 00 00 01 00 00 00 00 00 2b 00 82 00 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 04 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 08 cf 00 87 00 00 00 00
