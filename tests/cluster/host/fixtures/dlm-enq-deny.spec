%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-enq-deny
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(f).1 (ac4-DENY)
capture:   scs-dlm-lockconflict.pcap
wire-len:  204
sha256:    33a501d253df8474302f405aaf1341bc87b6041fbc0cda93c7c3cdfc60b9c2bc
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
; abs 72-: the DLM body (cited). DENIED shape (SS$_NOTQUEUED, spec 4(f).1
; "Completion status"): body[20] is left as the REQUEST's PID placeholder
; (unchanged, not assigned), the mode byte is cleared to 0, and the
; resource name IS echoed back -- the exact opposite of the grant shape,
; which is how vms_dlm_enq_response_parse discriminates the two without a
; literal status code (none appears in the reply body, per spec).
@80   82                         ; body[8] category 0x82 (0x02 | response)
@81   01                         ; body[9] opcode 0x01
@92   1c 02 20 20                ; body[20:24] PID placeholder, UNCHANGED
                                 ; from the request (GROUNDED constant)
@102  00                         ; body[30] mode CLEARED to 0 (GROUNDED)
@118  03                         ; body[46] constant marker before the name
@119  08                         ; body[47] resource-name length
@120  4f 56 4d 58 41 41 41 41    ; body[48:56] "OVMXAAAA", ECHOED (GROUNDED)
; body[24:28] master lkid: a real RSB master-lkid value belongs here (the
; resource is already established, EX-held by the master) but no ac4-DENY
; specimen byte for it is recorded in the published spec text -- left
; UNCITED rather than guessed (INV-6: honest omission over a placeholder).
