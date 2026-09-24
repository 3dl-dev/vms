%OVMX-CLUSTER-SPECIMEN-1
name:      cm-relay-oracle-resp
class:     scs-msg
origin:    capture
capture:   vax3-2to3-established-join-20260730.pcap
spec:      docs/cluster-protocol-spec.md 4(r) "op 0x12 | body[18] = 0x01; body[17] = the responder's own current class; body[20:24] = LE u32 copy of the request's body[12:16] (the epoch)"
wire-len:  204
sha256:    f7e1142f12a128bad34d95de94bbb9978f8070e6fab60ea3a2a2cbdaaff42f08
%bytes
; THE ORACLE. A real sitting OpenVMS VAX cluster MEMBER (aa:00:04:00:01:04)
; answering cm-relay-oracle-req.spec, 0.3 ms later, in the same capture. This
; is what OVMX's member side must put on the wire when a coordinator relays a
; third node to it (rd vms-4f0).
;
; Corpus-wide this answer is not optional: across every capture in both
; reference trees there are 151 cat-0x01 op-0x12 requests and 148 answers; of
; the three unanswered, two went to a node that had already departed and one
; went to OVMX itself.
;
; DIFF against the request, body-relative -- and it is a VERBATIM echo with
; exactly the spec's mutations and nothing else:
;   body[0:4]   send/ack  -- the RESPONDER's own dialogue numbers (the CSB's)
;   body[4:8]   txn/token -- ECHOED (01 00 a8 a0)
;   body[8]     0x01 -> 0x81
;   body[9]     opcode    -- ECHOED
;   body[17]    the RESPONDER's own current class (0x02 here)
;   body[18]    0x00 -> 0x01, the response marker, FORCED
;   body[20:24] 12 02 40 20 -> 03 00 00 00, a FRESH LE u32 copy of body[12:16]
;   body[10:16], body[16], body[19], body[24:132]  -- byte-identical echo
@0    08 00 2b 78 56 b9 aa 00 04 00 01 04 60 07 bc 00
@16   aa 00 04 00 02 04 01 00 aa 00 04 00 01 04 4b 13
@32   32 5d e2 48 01 00 12 00 32 5d 00 00 e2 48 00 00
@48   32 5d 00 00 01 00 00 02 92 00 04 00 0a 00 01 00
@64   09 00 98 a4 0a 00 51 35 1a 3e 6c 52 01 00 a8 a0
@80   81 12 00 00 03 00 00 00 10 02 01 00 03 00 00 00
@96   a9 03 00 03 64 00 00 00 00 00 f9 60 12 aa 87 00
@112  00 00 00 00 00 00 00 16 46 31 31 42 24 61 53 59
@128  53 44 53 4b 31 20 20 20 20 20 2a 00 00 00 00 00
@144  01 00 01 00 00 00 00 00 00 00 00 00 00 00 00 00
@160  01 00 00 00 01 00 00 00 00 00 00 00 00 00 00 00
@176  de 00 00 00 00 00 01 01 00 00 00 00 ff ff ff ff
@192  00 00 00 00 ff ff ff ff 6d 1b 50 48
