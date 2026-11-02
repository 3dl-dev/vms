%OVMX-CLUSTER-SPECIMEN-1
name:      dlm-deq-valblk-write
class:     scs-msg
origin:    spec-composed
spec:      src/kernel-core/vms_cluster_codec_dlm.h (struct vms_dlm_deq, rd vms-d63); tools/cluster/f11b_ser_valblk_census.py
capture:   mscp-read-write.pcap
wire-len:  204
sha256:    0b29e302f8b44d962acb1755e1f21b4df79f03955f4546a5e8713769e726f1d2
%bytes
; PROVENANCE. mscp-read-write.pcap frame 1481 (1-based pcap record index
; as tools/cluster/remaster_census.sca_frames counts), VAX2 -> VAX1, a real
; two-node OpenVMS VAX V7.3 cluster with no OVMX node on the bridge: VAX2
; COPYing a 36-block file onto DATA2, which VAX1 serves over MSCP. The
; driving sequence is on the same wire: frame 1474 is VAX2's PW request for
; 'F11B$s' 0d 00 00 00 (file 13, the new file), frame 1475 VAX1's grant
; carrying the value block (2,1), frame 1477 the header write, this frame
; the release, and frame 1563 VAX1's next grant of the same resource
; handing out (3,1) -- the block this release carried.
;
; Bytes past body[52] are the sender's stale buffer (frame-grounding.tsv's
; dlm_deq rows) and are deliberately not cited.
@0    aa 00 04 00 01 04        ; eth dst: VAX1 (aa-00-04-00-01-04), the MASTER of F11B$vDATA2's tree
@6    08 00 2b 58 e6 f1        ; eth src: VAX2's NIC, the releasing node
@12   60 07                    ; ethertype 0x6007
@14   bc 00                    ; SCA length field -> 190-byte content
@16   aa 00 04 00 01 04        ; dst logical: VAX1
@22   01 00                    ; connect flag
@24   aa 00 04 00 02 04        ; src logical: VAX2
@30   4b 13                    ; msgtype 0x4b (sequenced msg), format 0x13
@80   02                       ; body[8]  category 0x02 (request)
@81   03                       ; body[9]  opcode 0x03 -- $DEQ
@92   a0 06 00 07              ; body[20:24] master_lkid 0x070006a0 == the handle VAX1's GRANT (frame 1475) named
@96   88 03 00 0b              ; body[24:28] req_lkid 0x0b000388 == VAX2's own handle on its PW request (frame 1474) for 'F11B$s' 0d 00 00 00
@102  01                       ; body[30] THE CELL 1: this release WRITES its value block (rd vms-d63)
@104  01                       ; body[32] its mirror, equal as on every real release
@108  03 00 00 00              ; body[36:40] the value block's HEADER counter: 3 -- the grant (frame 1475) handed out 2; the close's header write (MSCP WRITE LBN 445654, frame 1477) stepped it
@112  01 00 00 00              ; body[40:44] the DATA counter: 1, unchanged -- 36 data blocks written meanwhile are RMS's, not the XQP's
@116  00 00 00 00 00 00 00 00  ; body[44:52] zero, as on every real F11B$s value block
