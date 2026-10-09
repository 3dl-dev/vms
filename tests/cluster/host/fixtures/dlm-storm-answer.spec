%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-storm-answer
class:     scs-msg
origin:    capture
spec:      docs/cluster-protocol-spec.md 4(f).1 (rd vms-b5b0)
capture:   vms-b5b0-storm-window.pcap
wire-len:  204
sha256:    e756b4ee0b2d88f8f3550b1a672345304d6e36229f7260d4ebead7948c0d271b
%bytes
; OVMXE's ANSWER to that request -- the frame that caused the storm. SCA
; frame 2082, 2.1 ms later. OVMX sent 65,340 of these. It is kept as a fixture
; because it is the NEGATIVE control: a test that cannot tell it from a real
; master's grant is not testing anything.
@0    aa 00 04 00 01 04 52 54 00 00 e5 01 60 07 bc 00
@16   aa 00 04 00 01 04 01 00 aa 00 04 00 06 04 4b 13
@32   be 00 a1 00 04 00 12 00 be 00 00 00 a1 00 00 00
@48   be 00 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   0c 00 14 1f 09 00 73 c2 6d 00 8a 00 08 00 e4 0e
@80   82 01 00 00 00 00 00 00 00 00 00 00 20 02 20 20
@96   57 03 00 00 00 00 05 00 00 00 00 00 00 00 00 00
@112  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@128  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@144  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@176  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@192  00 00 00 00 00 00 00 00 00 00 00 00
;
; body[8]     = 82           the response bit -- so VAX1 did get an answer
; body[20:24] = 20 02 20 20  the request's own PID-form placeholder, echoed
;                            back -- so the MASTER's handle slot carries no
;                            handle at all
; body[24:28] = 57 03 00 00  OVMX's OWN master handle (0x00000357), written
;                            into THE REQUESTER's slot. VAX1's own handle
;                            (0x0f0003ce, body[24:28] of the request) appears
;                            NOWHERE in this frame: the two slots were
;                            swapped, so VAX1 could not correlate the
;                            completion to its lock, and asked again.
; body[28]    = 00           NO grant record (38/38 real grants have 0x10)
; body[30]    = 05           the granted mode, in a byte a real grant clears
; body[32:36] = 00 00 00 00  so body[34], THE OUTCOME BYTE, reads 0x00 --
;                            not 0xfa (granted), not 0xf9, not 0xf8: a value
;                            no real answer carries
; body[36:52] = 00 ...       zeros where the master resource's value block
;                            belongs
