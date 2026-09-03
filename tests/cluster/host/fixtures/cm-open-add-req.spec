%OVMX-CLUSTER-SPECIMEN-1
name:      cm-open-add-req
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(d), 4(j), 4(p), 4(r)
capture:   formation-ci1-joinwindow.pcap
wire-len:  204
sha256:    645f0cab05aef675892a3b8b641218fc5237dfa45a17c31349e118135cfbb232
%bytes
; Full 32-byte SCS envelope prefix (spec sec 4(d)) -- required so this
; fixture classifies as 'scs-msg' and satisfies the shared harvest-span
; round-trip test (tests/cluster/host/test_codec_roundtrip.c), even though
; this item's own assertions are scoped to the body (see the file header).
@0    08 00 2b 4a b7 15          ; eth dst (VAX1 HW MAC, spec-composed pair)
@6    08 00 2b 78 56 b9          ; eth src (VAX2 HW MAC)
@16   aa 00 04 00 01 04          ; dst logical (VAX1)
@22   01 00                      ; connect flag
@24   aa 00 04 00 02 04          ; src logical (VAX2)
;
; cat 0x01 op 0x09 -- the coordinator's class-0x02 ADD transition-open
; (spec 4(p) table: "open M->J cat 0x01 op 0x09 body[16:18]=0x0240; carries
; the transition epoch at body[12:16] and the membership bitmap at body[55]").
; Only the classification prefix + the fields FC-P3.1's recipes/accessors
; touch are cited; everything else is honest zero residue.
;
@12   60 07                      ; ethertype 0x6007
@14   bc 00                      ; SCA content 190 (spec 4(d))
@30   4b                         ; msgtype (SCS sequenced-application)
@31   13                         ; format constant (GROUNDED)
@72   05 00                      ; body0  SYSAP send-msg# (sender's own)
@74   03 00                      ; body2  SYSAP ack-msg#
@76   07 00                      ; body4  txn (opaque correlation id)
@78   ab 00                      ; body6  token/checksum (opaque, echoed only)
@80   01                         ; body8  category 0x01
@81   09                         ; body9  opcode 0x09 (class-0x02 ADD open)
@84   06 00 00 00                ; body12:16 epoch = 6, LE u32
@88   40                         ; body16 role slot 0x40 (ROLE_XITION)
@89   02                         ; body17 transition class 0x02 (ADD)
@127  0e                         ; body55 membership bitmap 0x0e (M=3: bits 1,2,3)
