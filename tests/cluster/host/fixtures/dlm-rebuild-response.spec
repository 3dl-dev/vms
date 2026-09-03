%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-rebuild-response
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "cat 0x02" (op 0x0d response recipe)
capture:   ovmx-760-MEMBER-achieved-20260730.pcap
wire-len:  204
sha256:    cbb2b38c62f7ea0c16f7d1dcead35211d7542f523ea219766e2d3650477f0645
%bytes
; The op-0d RESPONSE to dlm-rebuild-request.spec, per the spec's own recipe
; applied to that request's body: memcpy 132 bytes verbatim, then
; body[0:2]=own send-msg# (here 5), body[2:4]=ack of the peer's send# (here
; 7, the request's OWN send-msg#), body[8] |= 0x80, body[34] = 0xf9. Every
; other byte is IDENTICAL to dlm-rebuild-request.spec's body -- this pair
; is the round-trip proof that vms_dlm_rebuild_response_build() changes
; exactly those four spans and nothing else.
@0    08 00 2b 78 56 b9
@6    08 00 2b 4a b7 15
@12   60 07
@14   bc 00
@16   aa 00 04 00 02 04
@22   01 00
@24   aa 00 04 00 01 04
@30   4b 13
@72   05 00 07 00 00 00 00 00 82 0d 00 00 01 00 03 00
@88   0a 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@104  00 00 f9 00 00 00 00 00 00 00 00 00 00 00 00 0a
@120  53 59 53 24 53 59 53 5f 49 44 00 00 00 00 00 00
@136  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@152  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@168  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@184  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@200  00 00 00 00
