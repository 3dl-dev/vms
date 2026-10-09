%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-storm-request
class:     scs-msg
origin:    capture
spec:      docs/cluster-protocol-spec.md 4(f).1 (rd vms-b5b0)
capture:   vms-b5b0-storm-window.pcap
wire-len:  204
sha256:    ce664d357b1c8ffc93b88e7b0ec38753aca57507680663b4c730508b40f779f1
%bytes
; ONE of the 65,356 identical op-01 ENQs VAX1 sent OVMXE in 63.7 s
; (1026/s, median gap 0.362 ms) during the rd vms-b5b0 evacuation lab run:
; $ENQW EX on EVAC$WORKLOAD, a resource an OVMX process had taken NL on and
; OVMX therefore mastered. SCA frame 2081 of vms-b5b0-storm-window.pcap.
; EVERY byte is a real OpenVMS VAX V7.3 node's own.
@0    52 54 00 00 e5 01 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 06 04 01 00 aa 00 04 00 01 04 4b 13
@32   a0 00 be 00 01 00 12 00 a0 00 00 00 be 00 00 00
@48   a0 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 73 c2 0c 00 14 1f 8a 00 6c 00 08 00 e4 0e
@80   02 01 4f 03 01 00 08 00 00 00 00 00 20 02 20 20
@96   ce 03 00 0f 00 00 05 00 20 00 01 00 00 00 00 00
@112  00 00 00 00 01 00 03 0d 45 56 41 43 24 57 4f 52
@128  4b 4c 4f 41 44 41 20 20 20 20 04 00 00 00 00 00
@144  c0 c6 2d 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 0f 01 12 00 01 00 00 81 a0 87 00 00 00 00
@176  80 24 9d 87 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 ff ff ff ff 10 7e 02 00
;
; body[9]     = 01           op ENQ
; body[24:28] = THE REQUESTER's own handle -- the value OVMX's grant had to
;               echo for VAX1 to correlate the completion, and did not
; body[30]    = 05           the mode asked for (EX)
; body[44:46] = 01 00        UIC group 1; body[46] = 03 supervisor mode
; body[47]    = 0d           name length 13; body[48:61] EVAC$WORKLOAD
; body[128:132]              VAX1's own directory hash for the resource
