%OVMX-CLUSTER-SPECIMEN-1
name:      cm-params-lockdirwt0-oracle
class:     scs-msg
origin:    capture
capture:   fcb-L1-params.pcap
spec:      docs/cluster-protocol-spec.md 4(j) "LOCKDIRWT -- GROUNDED by controlled reconfiguration" (rd vms-fcb)
wire-len:  204
sha256:    6ceb4fc8b09455ddde2083dfc71c67c37886eaa3a4ed2f250b5ea15455e51a4a
%bytes
; REAL captured cat-0x01 op-0x01 cluster-parameters record (frame 2 of
; tests/lab/captures/vms-fcb-lockdirwt-20261004/fcb-L1-params.pcap) sent by a real OpenVMS VAX V7.3
; VAX2 (SCSSYSTEMID 1026, the joiner), booted conversationally with SYSBOOT> SET LOCKDIRWT 0
; and read back with SYSBOOT> SHOW LOCKDIRWT on its own console (L1-vax2.console.log).
;
; THE FIELD: body[26:28] (abs 98) = 00 00 -- the sender's LOCKDIRWT 0.
;   body[18:20] (abs 90) = 00 00  member count (rd vms-e88)
;   body[22:24] (abs 94) = 00 00  VOTES (sec 4(j))
@0    aa 00 04 00 01 04 08 00 2b 92 0b 09 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 02 04 4b 13
@32   0d 00 0d 00 01 00 12 00 0d 00 00 00 0d 00 00 00
@48   0d 00 00 00 01 00 00 02 92 00 04 00 0a 00 00 00
@64   0a 00 52 2f 09 00 db cb 02 00 00 00 00 00 00 00
@80   01 01 00 00 00 50 00 00 00 00 00 00 01 00 00 00
@96   01 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 80 4a 3f 0e 57 9f 00
@144  10 00 00 00 01 00 00 00 00 00 2a 00 0d 00 00 00
@160  56 37 2e 33 20 20 20 20 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
