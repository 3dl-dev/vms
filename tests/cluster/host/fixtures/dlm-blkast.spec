%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-blkast
class:     scs-msg
origin:    spec-composed
spec:      tests/lab/captures/vms-c03-dlm-opcodes-20260911/GROUNDING.md (op 0x04 = BLKAST, frame f58)
capture:   dlm-blk2-20260911.pcap
wire-len:  204
sha256:    7100df43fdf313681e3eb9401d7063d9bfb04c468d770c6e81c22c762a7ef00f
%bytes
; abs 0-71: plausible classifying prefix only -- see dlm-deq-release.spec.
; NOTE THE DIRECTION: a BLKAST travels MASTER -> HOLDER, so vax2 (the master
; in this scenario) is the source and VAX1 (the EX holder) is the destination
; -- the reverse of every other DLM specimen in this directory.
@0    08 00 2b 4a b7 15          ; eth dst: VAX1 HW MAC (the blocked holder)
@6    08 00 2b 78 56 b9          ; eth src: VAX2 HW MAC (the master)
@12   60 07                      ; ethertype 0x6007
@14   bc 00                      ; SCA length field -> 190-byte content
@16   aa 00 04 00 01 04          ; dst logical: VAX1 (sysid 1025)
@22   01 00                      ; connect flag
@24   aa 00 04 00 02 04          ; src logical: VAX2 (sysid 1026)
@30   4b 13                      ; msgtype 0x4b (sequenced msg), format 0x13
;
; abs 72-: the DLM SYSAP body -- CITED.
;
; PROVENANCE. dlm-blk2-20260911.pcap frame f58, vax2 -> VAX1. The scenario:
; VAX1 took an EX lock on resource 'OVMXBLK2' with a BLKAST AST routine (f18,
; an op-0x01 ENQ), then vax2 -- which masters the tree -- queued a conflicting
; EX request, and the master sent the blocking AST below to the remote holder.
@80   02                         ; body[8]  category 0x02 (request)
@81   04                         ; body[9]  opcode 0x04 -- BLKAST.
                                 ; NOT the "completion" an earlier PROVISIONAL
                                 ; codec table claimed lived at 0x04; that op
                                 ; does not exist on a real wire.
@92   af 03 00 0a                ; body[20:24] req_lkid 0x0a0003af == the
                                 ; blocked holder's own local handle
@96   e3 04 00 59                ; body[24:28] master_lkid 0x590004e3 == the
                                 ; master handle f18's EX ENQ for 'OVMXBLK2'
                                 ; carried. THIS is how a BLKAST names its
                                 ; lock -- by id, and by nothing else.
@102  01 05                      ; body[30:32] the mode-context pair.
                                 ; *** OBSERVED, NOT PINNED. *** Two BLKASTs
                                 ; for this lock (f58, f60) read 01 05; a third
                                 ; (f42, a different F11B$a lock) read 01 00.
                                 ; Three samples across two locks is not a
                                 ; one-variable diff, so the codec carries
                                 ; these bytes labelled and opt-in and does not
                                 ; claim to know what they mean.
;
; DELIBERATELY NOT CITED, AND THE MOST IMPORTANT LINE IN THIS FILE: the real
; f58 carries readable ASCII 'F11B$aSYSDSK1' at body[48]. That is STALE
; BUFFER, not a field. The resource this BLKAST is actually about --
; 'OVMXBLK2' -- appears in the whole 83-frame capture exactly once, in the
; op-0x01 ENQ at f18, and never in an op-0x04. A codec that read body[48]
; here would hand its caller a resource name belonging to a different lock:
; a wire value that looks exactly like data and is not. So this specimen
; leaves the span zero and vms_dlm_blkast_parse_body() has no name field.
