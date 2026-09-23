%OVMX-CLUSTER-SPECIMEN-1
name:      hello-c3-vaxc-v55-directed-b2
class:     hello-c3
origin:    capture
spec:      docs/cluster-protocol-spec.md 2, 4(a), 4(a).0, 4(a).1, 4(b), 4(b.c3)
capture:   hubshapes.json
frame:     1
wire-len:  128
sha256:    05b465406566c049d28df33c1cf195932076c2b17972c5aada0184ae06641f80
%bytes
;
; rd vms-0f8. The DIRECTED class-0x03 HELLO: a real OpenVMS VAX V5.5-2H4
; member's sec 4(a).1 channel-verify REQUEST (abs 30 == b2) to OVMX -- the
; frame that opened a NISCA channel between OVMX and real V5.5 VMS.
;
; It exists because OVMX learned the class-0x03 revision. The multicast
; specimen (hello-c3-vaxc-v55.spec) is the frame OVMX could not classify; THIS
; is the frame that arrived once OVMX could answer in the peer's revision. The
; member sends exactly ONE b2 in a correct exchange (sec 4(a).1) and that is
; what the capture holds: n=1, against 186 OVMX b3 REQUESTs and 184 VAXC b4
; CONFIRMs of the same channel, steady-state.
;
; VERBATIM from tests/lab/captures/vms-0f8-browser-c03-channel-20260923/
; hubshapes.json (key "VAXC/128/w30=b200/b36=03", field `hex`), SHA-256 in
; docs/clean-room/reference-captures.sha256.
;
@0    52 54 00 00 00 0a          ; eth dst = OVMX's HARDWARE MAC (sec 4a.0)
@6    52 54 00 00 00 0c          ; eth src = VAXC's real HW MAC
@12   60 07                      ; ethertype 0x6007 DEC SCA/LAVC (sec 2)
@14   70 00                      ; SCA length: 0x0070+2 = 114 content (the C03 revision)
;
; sec 4(a).0: abs 0-5 and abs 16-21 are TWO DIFFERENT addresses on a directed
; frame. abs 16 is OVMX's cluster-LOGICAL address aa:00:04:00:<LE16(1987)>,
; not the hardware MAC at abs 0. Mirroring one into the other is the defect
; that makes a peer silently drop every reply.
;
@16   aa 00 04 00 c3 07          ; dst LOGICAL = OVMXA, SCSSYSTEMID 1987
@22   01 01                      ; abs 22, the C03 revision's value (0x05's is 01 00)
@24   aa 00 04 00 c5 07          ; src LOGICAL = VAXC, SCSSYSTEMID 1989
@30   b2 00                      ; sec 4(a).1 channel-verify REQUEST, phase b2
@32   08 00 00 80                ; discovery-family constant prefix (sec 4a)
@36   03                         ; message-class byte: the C03 revision
@37   01 00 00                   ; discovery-family constant suffix (sec 4a)
@40   06                         ; node-name length prefix
@41   56 41 58 43 20 20          ; "VAXC  "
@47   00 80 01 ff 83 00 04 00 00 00 00 00 00 00 00 00 10   ; abs 47..63 disc-format span
@64   03 00 00 00                ; abs 64..67
;
; sec 4(a)/4(g): the cluster-wide join nonce, NON-ZERO and in the clear on a
; directed frame (it is zero on the multicast specimen). This is the value
; vms_pe_fsm.c's pe_learn_join_nonce() learns live off a real peer -- never
; computed from (group#, password), which is unpublished (Rule 8).
;
@68   77 11 7a 7d                ; join nonce, GROUNDED non-zero on a directed frame
@72   00 00 00 00 00 00 00 00 00 00
@82   00 00 00 00 00 00 00 00 00 00   ; abs 72..91 zero padding (sec 4b)
;
; sec 4(i).B: on a DIRECTED HELLO abs 92 is the incarnation the sender
; attributes to US. 0x0001 = fresh contact -- and the multicast specimen's is
; 0x0000, which is the sec 4(b) multicast/directed distinction holding in this
; revision exactly as it does in the 0x05 one.
;
@92   01 00                      ; incarnation VAXC attributes to OVMXA
@94   90 05                      ; abs 94, the C03 revision's value (0x05's is 92 05)
;
; abs 96..101 is the LIVE tick. UNCITED by construction.
;
@102  bc 00 03 58 51 41 00 00 00 00   ; abs 102..111 constant tail (sec 4b) -- IDENTICAL across revisions
@112  00 00 00 00 00 00 00 00    ; abs 112..119 zero padding (sec 4b)
@120  52 54 00 00 00 0c          ; sender's real HW LAN MAC (sec 4b) -- matches abs 6
@126  21 00                      ; abs 126..127; the frame ENDS here (no abs 128-133 tail)
