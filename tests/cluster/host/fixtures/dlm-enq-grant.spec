%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-enq-grant
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(f).1 (ac4-LKID2)
capture:   scs-dlm-lockconflict.pcap
wire-len:  204
sha256:    f4d2d49f87b42bacf2747dab20b375d8f99a2d813a8501e5534ea84adafe266a
%bytes
; abs 0-71: not cited, see dlm-enq-request-pw.spec's header comment.
@0    08 00 2b 4a b7 15
@6    08 00 2b 78 56 b9
@12   60 07
@14   bc 00
@16   aa 00 04 00 01 04
@22   01 00
@24   aa 00 04 00 02 04
@30   4b 13
; abs 72-: the DLM body (cited). GRANTED shape (spec 4(f).1 "Completion
; status"): body[20] now holds the requester's REAL assigned lock-id
; (the PID placeholder is gone) and the resource name is NOT echoed.
@80   82                         ; body[8] category 0x82 (0x02 | response)
@81   01                         ; body[9] opcode 0x01
@92   ab 00 00 31                ; body[20:24] req lkid 0x310000AB
                                 ; (GROUNDED byte-exact vs SDA, ac4-LKID2)
@96   af 06 00 52                ; body[24:28] master lkid 0x520006AF
                                 ; (GROUNDED byte-exact vs SDA, ac4-LKID2)
@102  04                         ; body[30] granted mode PW=4
