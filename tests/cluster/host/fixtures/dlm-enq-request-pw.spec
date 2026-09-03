%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-enq-request-pw
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(f).1 (ac4-MPW2)
capture:   scs-dlm-lockconflict.pcap
wire-len:  204
sha256:    0d2cc9282ae90894668b2438f412731233c7f58325b00aae4cdec6a4e5c464a9
%bytes
; abs 0-71: shared SCA header + generic SYSAP envelope (msgtype/format/
; recv_ack/send_seq/Con.ID) -- NOT cited: this fixture proves only the
; DLM-specific body span vms_dlm_enq_request_parse/_build own (abs 72-),
; same division of labour the header doc comment documents. These bytes
; are filled with a plausible SCS_MSG-classifying header so
; vms_frame_classify() succeeds, but are deliberately left UNCITED.
@0    08 00 2b 78 56 b9          ; eth dst: VAX2 HW MAC (the master)
@6    08 00 2b 4a b7 15          ; eth src: VAX1 HW MAC (the requester)
@12   60 07                      ; ethertype 0x6007
@14   bc 00                      ; SCA length field -> 190-byte content
@16   aa 00 04 00 02 04          ; dst logical: VAX2 (sysid 1026)
@22   01 00                      ; connect flag
@24   aa 00 04 00 01 04          ; src logical: VAX1 (sysid 1025)
@30   4b 13                      ; msgtype 0x4b (sequenced msg), format 0x13
; abs 72-: the DLM SYSAP body (spec 4(f).1) -- CITED, this item's scope.
@80   02                         ; body[8]  category 0x02 (request)
@81   01                         ; body[9]  opcode 0x01 (ENQ)
@92   1c 02 20 20                ; body[20:24] req PID placeholder
                                 ; 0x2020021c, GROUNDED constant across all
                                 ; six console-issued ac4 captures
@96   00 00 00 00                ; body[24:28] master lkid: 0 (fresh ENQ,
                                 ; lock not yet established)
@102  04                         ; body[30] requested mode PW=4 (GROUNDED
                                 ; six-value one-variable diff, ac4-MPW2)
@118  03                         ; body[46] constant marker before the name
@119  08                         ; body[47] resource-name length
@120  4f 56 4d 58 41 41 41 41    ; body[48:56] "OVMXAAAA"
