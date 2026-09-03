%OVMX-CLUSTER-SPECIMEN-1
name:      hello-padded-vax1-channel-size-verify
class:     hello-padded
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(a), 4(a).0, 4(a).2, 4(b), 4(k)
capture:   ci3-addmember-20260728.pcap
frame:     85
wire-len:  1514
sha256:    7e906dd921ffa25677c543249b1740cbe1dcafe89b2706a2099628a98859758f
%bytes
;
; The NISCA channel packet-size verification frame (sec 4k): a genuine
; directed HELLO zero-padded to NISCS_MAX_PKTSZ. Its distinguishing feature is
; its SIZE, not any opcode -- which is exactly what this codec's frame-class
; registry has to get right (the campaign spent a lab week calling it an
; "op-0xb3 block transfer").
;
; The sec 4(k) specimen is VAX1 -> an OVMX joiner whose cluster-LOGICAL address
; is not published in the spec, so the DESTINATION here is composed from sec 3's
; VAX2 identity instead (a directed HELLO to a member is the same frame class).
; Every VAX1-side field below is the sec 4(k) grounded value.
;
@0    08 00 2b 78 56 b9          ; eth dst = peer HW MAC (sec 4a.0)
@6    08 00 2b 4a b7 15          ; eth src = VAX1 HW MAC (sec 4k pl106)
@12   60 07
@14   da 05                      ; SCA length field 0x05da + 2 = 1500 content
                                 ; == NISCS_MAX_PKTSZ 1498 + 2 (sec 4k)
@16   aa 00 04 00 02 04          ; dst LOGICAL addr (sec 4k pl2)
@22   01 00                      ; connect flag (sec 4k pl8)
@24   aa 00 04 00 01 04          ; src LOGICAL addr, VAX1 (sec 4k pl10)
@30   b3                         ; per-frame word 0xb3 (sec 4k pl16) -- a
                                 ; directed-HELLO channel-verify value, NOT a
                                 ; block-transfer opcode
@31   00                         ; sec 4k pl17: 0x00, NOT the 0x13 SCS format byte
@32   08 00 00 80
@36   05                         ; HELLO class byte, never SOLICIT (sec 4k pl22)
@37   01 00 00
@40   06                         ; sec 4k pl26: name length + name
@41   56 41 58 31 20 20          ; "VAX1  "
@47   00 80 01 ff 83 00 04 00 00 00 00 00 00 00 00 00 18   ; abs 47..63 (sec 4a.2)
@64   03 00 00 00                ; abs 64..67 (sec 4a.2) -- the padded probe is
                                 ; a genuine directed HELLO, so it carries the
                                 ; same discovery-format span
@68   ee 05 39 5b                ; join nonce (sec 4k pl54)
@92   01 00                      ; node incarnation, golden fresh (sec 4k pl78)
@120  08 00 2b 4a b7 15          ; sender's real HW LAN MAC (sec 4k pl106)
@128  1f 00                      ; poller sweep 31 (sec 4k pl114)
;
; abs 134..1513 is the zero pad. sec 4(k) grounds it as entirely zero for the
; 1500/1069/853-byte classes (1380/1380 bytes zero), and the loader zero-fills
; uncited bytes, so the assembled frame is byte-correct there -- it is simply
; not CITED, because nothing in FC-P0.6 asserts on it.
