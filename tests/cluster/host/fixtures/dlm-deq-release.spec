%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-deq-release
class:     scs-msg
origin:    spec-composed
spec:      tests/lab/captures/vms-c03-dlm-opcodes-20260911/GROUNDING.md (op 0x03 = $DEQ, frame f14)
capture:   dlm-deq-20260911.pcap
wire-len:  204
sha256:    d7c9d7d3ec9ff871930389e95439beb3a8bf61685505eb38faf38105baff5ef7
%bytes
; abs 0-71: shared SCA header + generic SYSAP envelope -- a plausible
; SCS_MSG-classifying prefix so vms_frame_classify() succeeds, exactly as
; dlm-enq-request-pw.spec's header comment describes. NOT part of what this
; specimen proves; no builder in vms_cluster_codec_dlm.c touches this span.
@0    08 00 2b 78 56 b9          ; eth dst: VAX2 HW MAC (the master)
@6    08 00 2b 4a b7 15          ; eth src: VAX1 HW MAC (the releasing node)
@12   60 07                      ; ethertype 0x6007
@14   bc 00                      ; SCA length field -> 190-byte content
@16   aa 00 04 00 02 04          ; dst logical: VAX2 (sysid 1026)
@22   01 00                      ; connect flag
@24   aa 00 04 00 01 04          ; src logical: VAX1 (sysid 1025)
@30   4b 13                      ; msgtype 0x4b (sequenced msg), format 0x13
;
; abs 72-: the DLM SYSAP body -- CITED, and every cited byte below is a byte
; the real capture put there.
;
; PROVENANCE. dlm-deq-20260911.pcap frame f14, VAX1 -> vax2, captured on a
; private 2-node real OpenVMS VAX 7.3 cluster (GROUNDING.md). The driving
; sequence is in the same capture: f12 is VAX1's $ENQ at PW on resource
; 'OVMXDEQ1', f13 is vax2's cat-0x82 GRANT of it, f14 is the $DEQ below.
; That is the correlation -- the lock ids here are not "plausible", they are
; the ids of a specific real lock whose creation is two frames earlier in the
; same pcap.
@80   02                         ; body[8]  category 0x02 (request)
@81   03                         ; body[9]  opcode 0x03 -- $DEQ.
                                 ; NOT the "commit" an earlier PROVISIONAL
                                 ; codec table claimed lived at 0x03; that op
                                 ; does not exist on a real wire.
@92   cd 01 00 08                ; body[20:24] req_lkid 0x080001cd == the
                                 ; requester handle f13's GRANT assigned
@96   eb 04 00 3a                ; body[24:28] master_lkid 0x3a0004eb == the
                                 ; master handle f12's ENQ for 'OVMXDEQ1'
                                 ; carried. THE correlation that grounds this
                                 ; whole opcode.
@102  00                         ; body[30] mode NL(0) -- the mode the lock
                                 ; stood at, matching f13's granted mode
;
; DELIBERATELY NOT CITED, because they are NOT FIELDS: the real f14 carries
; 0x9a at body[46] and 0x87 at body[47], where an ENQ would carry the 0x03
; name marker and a name length. The second $DEQ in the same capture (f18)
; carries 0x00/0x00 there instead. Two different values for the "same field"
; across one capture is the proof that it is uninitialised buffer -- so this
; specimen leaves the span zero and the codec reads no name from a $DEQ.
