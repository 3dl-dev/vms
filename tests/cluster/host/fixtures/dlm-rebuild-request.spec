%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-rebuild-request
class:     scs-msg
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(p) "cat 0x02" (op 0x0d)
capture:   ovmx-760-MEMBER-achieved-20260730.pcap
wire-len:  204
sha256:    9fe33dcaffc66d2e55a90273f65cdaa3f0d1c0324e4cc78c4738ef7eb88299ad
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
; abs 72-204 (body[0:132)): the WHOLE DLM SYSAP body, CITED in full -- the
; exact span the op-0d response recipe's "memcpy(resp_body, req_body, 132)"
; copies verbatim (spec 4(p)), so the round-trip test proves the untouched
; majority of the body survives the echo unchanged, not just the mutated
; fields. body[0:2]/[2:4] (envelope send/ack counters) and body[16] (L1
; length) are plausible test values, not independently grounded bytes;
; body[8]/[9] (category/opcode), body[12:14]/[14:16] (the two invariants)
; and body[47]/[48:58] (name length/"SYS$SYS_ID") ARE spec-4(p)-GROUNDED.
; The rest of the 132-byte span is zero -- an honest codec-composed value,
; never someone else's uninitialised memory (spec 4(p) "Residue" warning).
@72   07 00 00 00 00 00 00 00 02 0d 00 00 01 00 03 00
@88   0a 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@104  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 0a
@120  53 59 53 24 53 59 53 5f 49 44 00 00 00 00 00 00
@136  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@152  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@168  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@184  00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00
@200  00 00 00 00
