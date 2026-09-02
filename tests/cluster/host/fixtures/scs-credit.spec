%OVMX-CLUSTER-SPECIMEN-1
name:      scs-credit-return-short
class:     scs-credit
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(h)(3), 4(h)(4)
capture:   formation-ci1-joinwindow.pcap
frame:     26
wire-len:  60
sha256:    e92f221ef54d46a8261748a2f20097e4caafec116d06f6044e5eaaa2da631b2d
%bytes
;
; The 41-byte credit-return short (sec 4h(3)). 14 + 41 = 55 < 60, so the wire
; pads it to the Ethernet minimum: this specimen is the RUNT-PADDED arm of the
; sec 2 length identity (predicted <= actual AND actual == 60), which accounts
; for all 928 of the 24570-frame census's residuals.
;
; The acknowledged-sequence VALUE below is chosen, not published; what sec
; 4h(3) grounds is the FIELD and its equalities -- pl18 == pl26 (622/622) and a
; third repeat at pl34 (616/622) -- plus pl20 == 0 (622/622). The unit test
; asserts those invariants, not the number.
;
@0    08 00 2b 78 56 b9          ; eth dst = VAX2 HW MAC
@6    08 00 2b 4a b7 15          ; eth src = VAX1 HW MAC
@12   60 07
@14   27 00                      ; 0x0027 + 2 = 41-byte payload
@16   aa 00 04 00 02 04          ; pl2  dst logical (VAX2)
@22   01 00                      ; pl8
@24   aa 00 04 00 01 04          ; pl10 src logical (VAX1)
@30   48                         ; pl16 msgtype 0x48 = credit return
@31   13                         ; pl17 format 0x13 (GROUNDED 622/622)
@32   05 00                      ; pl18 acknowledged sequence
@34   00 00                      ; pl20 send_seq == 0: a credit return emits no
                                 ;      new sequence number (GROUNDED 622/622)
@36   01 00                      ; pl22 constant 0x0001 (622/622)
@38   12 00                      ; pl24 NISCS_LAN_OVRHD 18 (622/622)
@40   05 00                      ; pl26 acknowledged-sequence mirror (622/622)
@42   00 00                      ; pl28 zero
;
; pl30 (abs 44) is the "secondary counter" sec 4h(3) leaves INFERRED -- not
; cleanly a function of pl18. Uncited.
;
@46   00 00                      ; pl32 zero
@48   05 00                      ; pl34 acknowledged-sequence 3rd repeat (616/622)
@50   00 00                      ; pl36 zero
@52   01 00                      ; pl38 constant 0x0001 (inferred, 598/622)
@54   00                         ; pl40 zero pad -- last byte of the 41-byte payload
;
; abs 55..59 is the Ethernet runt pad. Grounded zero by sec 2; uncited because
; nothing here asserts on it.
