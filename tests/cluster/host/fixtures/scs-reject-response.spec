%OVMX-CLUSTER-SPECIMEN-1
name:      scs-reject-response
class:     scs-conn-ctrl
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(1a), 4(h)(1b), 4(h)(1c), 4(m) op=5 REJECT_RSP
capture:   formation-ci1.pcap
frame:     5
wire-len:  72
sha256:    318c1225a1d8f0b026a171f5823ffa9ce6371675ae1c093a173c2df9f7853d2c
%bytes
;
; The op-5 REJECT_RSP -- resolved naming (sec 4(h)(1h)/(m) + the
; independent $SCSDEF confirmation: CON_REQ 0 .. REJ_REQ 4, REJ_RSP 5,
; DISC_REQ 6, DISC_RSP 7, CR_REQ 8, CR_RSP 9, APPL_MSG 10). This is a
; CENSUS-GROUNDED structural template, not one captured frame: 336 op-5
; frames across 4 sender nodes and 15 captures share this EXACT shape
; (src/vmsscs/scs_dir.c dir_confirm5_tmpl's own grounding comment).
; The 58-byte content class is the short response/control shape (sec
; 4(h)(1b)): envelope + handle pair, NO marker, NO name field -- the
; frame ends at abs 72. credit == 0 is GROUNDED 100% for type 5 over
; the real-VAX population (sec 4h(1c)).
;
; The census spans 4 different sender nodes, so no single dialogue's
; Con.ID pair is asserted as THE value -- cited as a real template
; value, not one dialogue's specific pair. The Ethernet MAC is
; INFERRED from the payload's VAX1/VAX2 logical addresses, same basis
; as scs-dir-connect-confirm.spec above.
;
@0    08 00 2b 4a b7 15           ; eth dst = VAX1 HW MAC (inferred, see above)
@6    08 00 2b 78 56 b9           ; eth src = VAX2 HW MAC (inferred, see above)
@12   60 07                       ; ethertype 0x6007
@14   38 00                       ; pl0  length = 0x0038 (58-byte content)
@16   aa 00 04 00 01 04           ; pl2  dst logical
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 02 04           ; pl10 src logical
@30   4b 13                       ; pl16 msgtype 0x4b (data-phase, sec 4(m) phase rule) / fmt 0x13
@32   02 00 02 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   02 00 00 00 02 00 00 00 02 00 00 00 01 00 00 02 0e 00 ; pl26..43 counter mirrors + inner length=0x000e(14)
@58   04 00                       ; pl44 format word 0x0004
@60   05 00                       ; pl46 op = 5 REJECT_RSP
@62   00 00                       ; pl48 credit = 0 (GROUNDED 100%, sec 4h(1c))
@64   08 00 dc e2                 ; pl50 remote Con.ID (peer's)
@68   07 00 00 00                 ; pl54 local Con.ID (ours) -- frame ENDS here, no marker
