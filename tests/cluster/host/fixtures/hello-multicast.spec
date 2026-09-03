%OVMX-CLUSTER-SPECIMEN-1
name:      hello-multicast-vax1
class:     hello
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(a), 4(a).2, 4(b)
capture:   scs-idle-baseline.pcap
frame:     1
wire-len:  134
sha256:    859fb363f565fcd47c92398e0bb52edc6b9aa10dd0c57d41993f2a3a80450f03
%bytes
@0    ab 00 04 01 01 01          ; eth dst = cluster multicast group 1 (sec 3)
@6    08 00 2b 4a b7 15          ; eth src = VAX1 real HW MAC (sec 4b abs 120)
@12   60 07                      ; ethertype 0x6007 DEC SCA/LAVC (sec 2)
@14   76 00                      ; SCA length field: 0x0076+2 = 120 content (sec 2)
@16   ab 00 04 01 01 01          ; dst/group LOGICAL addr = the group (sec 4a)
@22   01 00                      ; connect flag, observed constant 0x0001
@24   aa 00 04 00 01 04          ; src LOGICAL LAVC addr, VAX1 (sec 3, sec 4a)
@30   a0 00                      ; per-frame word a0 = multicast HELLO (sec 4a)
@32   08 00 00 80                ; constant prefix (sec 4a)
@36   05                         ; message-class byte 0x05 = HELLO (sec 4a)
@37   01 00 00                   ; constant suffix (sec 4a)
@40   06                         ; node-name length prefix, GROUNDED (sec 4a)
@41   56 41 58 31 20 20          ; "VAX1  ", ASCII space-padded (sec 4a)
;
; abs 47..67 is the discovery-format span. sec 4(a).2 now PUBLISHES it and
; grounds it as node-independent (11403 of 11575 HELLOs across 10 captures,
; five senders, 0 residuals among real nodes), so it is cited here rather
; than left zero. No meaning is claimed for any of these bytes.
;
@47   00 80 01 ff 83 00 04 00 00 00 00 00 00 00 00 00 18   ; abs 47..63 (sec 4a.2)
@64   03 00 00 00                ; abs 64..67 (sec 4a.2)
@68   00 00 00 00                ; join nonce: zero on a multicast HELLO (GROUNDED sec 4a)
@72   00 00 00 00 00 00 00 00 00 00
@82   00 00 00 00 00 00 00 00 00 00   ; abs 72..91 zero padding (sec 4b)
@92   00 00                      ; directed flag / incarnation: 0 on multicast (GROUNDED sec 4b)
@94   92 05                      ; constant trailer (sec 4b)
;
; abs 96..101 is the free-running timer/tick (sec 4b abs 96) -- a LIVE field,
; never a replayed constant. UNCITED by construction.
;
@102  bc 00 03 58 51 41 00 00 00 00   ; constant tail abs 102..111 (sec 4b)
@112  00 00 00 00 00 00 00 00    ; abs 112..119 zero padding (sec 4b)
@120  08 00 2b 4a b7 15          ; sender's real HW LAN MAC, GROUNDED (sec 4b)
@128  00 00                      ; poller-sweep marker: 0 on multicast, GROUNDED (sec 4b)
;
; abs 126..127 and abs 130..133 hold constants sec 4(b) prints as hex words
; (0x2600, 0x0064, 0x0000) without fixing their byte order; left UNCITED.
