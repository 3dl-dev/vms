%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-blkast
class:     scs-msg
origin:    capture
capture:   dlm-blk2-20260911.pcap
frame:     48
wire-len:  204
sha256:    4397fb126a78af409f4f95d37cd09657eb1e5c4712dc7ba937054ae0ffe39b52
%bytes
; Every cited byte below is VERBATIM from dlm-blk2-20260911.pcap record 48
; (1-based pcap record number; the vms-c03 GROUNDING.md's 0-based "f47"),
; t=1789136961.480, the master vax2 -> the EX holder VAX1. Uncited bytes are
; zero-filled here and are NOT asserted on.
;
; rd vms-ea1 -- WHY THIS IS RECORD 48 AND NOT THE "f58" THIS SPECIMEN USED TO
; CITE. The scenario: VAX1 took an EX lock on 'OVMXBLK2' with a BLKAST routine
; (record 19, an op-0x01 ENQ to the master vax2; record 20 the grant), then
; vax2 queued a conflicting EX request. Record 48 is the first frame after
; that contention, it is op 0x05, and it is the first frame anywhere in the
; capture to carry the OVMXBLK2 lock ids. The old "f58" (record 59) is a NAMED
; op 0x04 -- 'F11B$aSYSDSK1' at body[48] -- whose lock-id bytes are a stale copy
; of record 48's: record 61, another named op 0x04 ('DTI$SYSTEM$VAX2'), carries
; the same bytes, and record 43's op 0x04 carries record 38's. Named op 0x04 is
; the master removing its directory entry (rd vms-8219; 4881/4881 op-0x04
; frames in the dlmlab L1 capture are named). The independent second sample is
; in L1 as well: at the DLMBLK scenario the master vax1 sends the EX holder
; vax2 an op 0x05 in the same millisecond the contending ENQ arrives.
;
@0    aa 00 04 00 01 04          ; eth dst: VAX1 (sysid 1025), the blocked holder
@6    08 00 2b a9 a3 96          ; eth src: vax2's hardware MAC, the master
@12   60 07                      ; ethertype 0x6007
@14   bc 00                      ; SCA length field -> 190-byte content
@16   aa 00 04 00 01 04          ; dst logical: VAX1 (sysid 1025)
@22   01 00                      ; connect flag
@24   aa 00 04 00 02 04          ; src logical: vax2 (sysid 1026)
@30   4b 13                      ; msgtype 0x4b (sequenced msg), format 0x13
;
; abs 72-: the DLM SYSAP body.
@80   02                         ; body[8]  category 0x02 (request)
@81   05                         ; body[9]  opcode 0x05 -- BLKAST.
@92   af 03 00 0a                ; body[20:24] req_lkid 0x0a0003af: the id the
                                 ; record-20 grant assigned at body[20:24], i.e.
                                 ; the holder's own copy (spec 4(f).1, SDA row)
@96   e3 04 00 59                ; body[24:28] master_lkid 0x590004e3: the
                                 ; body[24:28] of the record-19 ENQ and its grant
@102  01 05                      ; body[30:32] the mode-context pair.
                                 ; *** OBSERVED, NOT PINNED. *** Here 01 05; the
                                 ; L1 sample reads 00 05. body[31] is the
                                 ; contender's EX (5) in both; two samples are
                                 ; not a one-variable diff, so the codec carries
                                 ; the pair labelled and opt-in and does not
                                 ; claim to know what it means.
;
; DELIBERATELY NOT CITED: body[16:20] (28 44 00 80 here, and in many unrelated
; frames of this capture) and everything past body[32]. A BLKAST names its lock
; by lock-id and by nothing else.
