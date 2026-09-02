%OVMX-CLUSTER-SPECIMEN-1
name:      scs-msg190-vaxcluster-cat01-op14
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(d), 4(j)
capture:   formation-ci1-joinwindow.pcap
frame:     45
wire-len:  204
sha256:    9c1ebd53af302086c89210298fc4300683d3ba8ca9eb3907d1ecf177d0b2267f
%bytes
;
; The 190-byte SCS message class -- the single most solidly grounded
; structural finding in the spec (17557/17557 frames) and the ONLY length
; class whose Con.ID location was independently confirmed against SDA. This is
; the class the connection manager and the whole DLM ride.
;
@0    08 00 2b 4a b7 15          ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9          ; eth src = VAX2 HW MAC
@12   60 07
@14   bc 00                      ; 0x00bc = 188 -> 190-byte payload (sec 4d)
@16   aa 00 04 00 01 04          ; pl2  dst logical (VAX1)
@22   01 00                      ; pl8
@24   aa 00 04 00 02 04          ; pl10 src logical (VAX2)
@30   4b                         ; pl16 msgtype
@31   13                         ; pl17 format
@64   09 00 c5 62                ; abs 64 REMOTE Con.ID 0x62C50009 -- VAX1's own
                                 ; Con.ID, in a frame sent BY VAX2: this field is
                                 ; the DESTINATION's id as the sender addresses
                                 ; it (GROUNDED sec 4d)
@68   08 00 58 33                ; abs 68 LOCAL Con.ID 0x33580008 = VAX2's own
@72   01 00                      ; body0 SYSAP send-msg#, starts at 1 (GROUNDED
                                 ;       monotonic 2902/2902)
@74   00 00                      ; body2 SYSAP ack-msg#
;
; body4..7 (abs 76..79) is the (transaction, checksum) correlation token. Its
; VALUES are per-dialogue and the checksum's derivation is not recoverable from
; passive capture (sec 4j), so nothing is cited here -- an invented token is
; exactly the fabrication INV-6 forbids.
;
@80   01                         ; body8  message category 0x01 = membership/config
                                 ;        (bit 0x80 would mark a response)
@81   14                         ; body9  opcode 0x14 = node CPU/model advertisement
@88   15                         ; body16 length prefix 0x15 = 21
@89   56 41 58 73 65 72 76 65 72 20 33 39 30 30 20 53 65 72 69 65 73
                                 ; body17 "VAXserver 3900 Series" (GROUNDED sec 4j)
