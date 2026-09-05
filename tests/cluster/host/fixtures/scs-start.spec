%OVMX-CLUSTER-SPECIMEN-1
name:      scs-start-vax2-config-round0
class:     scs-start
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(g) phase 2, 4(h)(4), 4(i).B
capture:   formation-ci1-joinwindow.pcap
frame:     15
wire-len:  120
sha256:    a04893c1e5d87c9c6d7f8677dab7e9feb31421509889e69053a1f81b1d210c9a
%bytes
;
; The 106-byte-payload START/config frame (sec 4g phase 2). Payload offsets in
; the comments are the spec's; abs = payload + 14.
;
@0    08 00 2b 4a b7 15          ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9          ; eth src = VAX2 HW MAC
@12   60 07
@14   68 00                      ; 0x0068 + 2 = 106-byte payload (sec 2)
@16   aa 00 04 00 01 04          ; pl2  dst logical addr (VAX1)
@22   01 00                      ; pl8  connect flag
@24   aa 00 04 00 02 04          ; pl10 src logical addr (VAX2)
@30   41                         ; pl16 SCS message type 0x41 START/STACK/ACK
@31   13                         ; pl17 format constant 0x13 (GROUNDED 2975/2975)
@32   00 00                      ; pl18 counter region begins
@34   01 00                      ; pl20 send_seq: 1 on a fresh join (sec 4g/4h(4))
@36   01 00                      ; pl22 the node-incarnation the member advertised,
                                 ;      echoed by the joiner (THE GATE, sec 4i.B)
@38   12 00                      ; pl24 0x0012 = 18 = SYSGEN NISCS_LAN_OVRHD (GROUNDED)
@40   00 00 00 00                ; pl26 zero
@44   01 00                      ; pl30 mirror of pl20 (GROUNDED 17758/17758)
@46   00 00 00 00 00 00          ; pl32 zero
@52   01 00                      ; pl38 constant 0x0001
@54   00 00                      ; pl40 zero
@56   3e 00                      ; pl42 inner length = payload - 44 = 62 (GROUNDED 42/42)
@58   00 00                      ; pl44 config-round counter: 0 = START (GROUNDED)
@60   02 04                      ; pl46 SCSSYSTEMID 1026 = VAX2 (GROUNDED 28/28)
@62   00 00 00 00                ; pl48 SCSSYSTEMIDH region, zero
@66   01 00                      ; pl52 constant 0x0001 (28/28)
@68   40 02                      ; pl54 constant 0x0240 = 576 (28/28, inferred role)
@70   d8 00                      ; pl56 constant 0x00d8 = 216 (28/28, inferred role)
@72   56 4d 53 20 56 37 2e 33    ; pl58 software version "VMS V7.3" (GROUNDED 28/28)
;
; pl66..73 (abs 80..87) is THIS SYSTEM'S INCARNATION, a live VMS absolute-time
; quadword = this boot's time. UNCITED on purpose: OVMX once replayed a
; captured value on every boot for six days, which is precisely the condition
; that earns a CLUEXIT bugcheck on a surviving member (sec 4g correction,
; vms-2f3). A specimen must not carry a replayable incarnation.
;
@88   56 41 58 20                ; pl74 hardware type "VAX " (GROUNDED 28/28)
@92   06 00                      ; pl78 constant 0x0006 (28/28)
@94   00 0a                      ; pl80 CLUSTER_CREDITS 10 at pl81 (GROUNDED)
@96   00 00 00 00 00 00          ; pl82 zero
@102  77 00                      ; pl88 constant 0x0077 (28/28)
@104  56 41 58 32 20 20 20 20    ; pl90 node name, FIXED 8-byte blank-padded
                                 ;      "VAX2    " -- a different encoding from
                                 ;      the length-prefixed HELLO name (sec 4g)
;
; pl98..105 (abs 112..119) is the frame-composition time, a second live
; absolute-time quadword. Also UNCITED: no real node ever sends a stale one.
