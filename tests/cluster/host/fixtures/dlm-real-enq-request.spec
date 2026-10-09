%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-real-enq-request
class:     scs-msg
origin:    capture
spec:      docs/cluster-protocol-spec.md 4(f).1 (rd vms-b5b0)
capture:   vms-b5b0-storm-window.pcap
wire-len:  204
sha256:    0aa9a5477c1792bb4903953a34445d9cc3c6f54bb24b3cf511bea40f6e60c042
%bytes
; A REAL op-01 ENQ request, VAX1 -> VAX2, SCA frame 1 of the rd vms-b5b0
; storm capture (pcap sha256 23e4524f5f0fabb942d5aa67856c461d2e2fe3c67c00942ffb6d0f529cd87d4b).
; EVERY byte here is a real OpenVMS VAX V7.3 node's own, so every byte is cited:
; this is a capture specimen, not a composed one.
@0    aa 00 04 00 02 04 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   fc 4a 25 59 01 00 12 00 fc 4a 00 00 25 59 00 00
@48   fc 4a 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 ad 1c 09 00 ad 1c 42 51 19 43 04 00 8a 15
@80   02 01 00 00 01 00 07 00 00 00 00 00 13 02 20 20
@96   cd 03 00 09 39 02 04 00 13 00 01 00 06 00 00 01
@112  2c 02 00 04 00 00 00 0a 46 31 31 42 24 73 2a 00
@128  00 00 49 4f 4e 5f 44 41 54 41 42 41 53 45 00 00
@144  02 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 03 00 00 00 00 04 00 ff ff ff ff ff ff
@176  e0 ff 00 60 ee 78 de 00 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff d7 68 a6 0b
;
; body[20:24] = 13 02 20 20  the PID-form placeholder in THE MASTER's handle slot
; body[24:28] = cd 03 00 09  THE REQUESTER's own handle (VAX1's)
; body[30]    = 04           the mode asked for (PW)
; body[44:46] = 00 00        UIC group; body[46] = 00 access mode (kernel)
; body[47]    = 0a           name length; body[48..] F11B$s*
; body[128:132] = d7 68 a6 0b  the sender's directory hash for the resource
