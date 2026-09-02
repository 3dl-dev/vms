%OVMX-CLUSTER-SPECIMEN-1
name:      scs-disc-response
class:     scs-seq
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(1a), 4(h)(1b), 4(m) op=7 DISCONNECT-RESPONSE
capture:   formation-ci1.pcap
frame:     55
wire-len:  72
sha256:    3ab7381867c39190c0cd0732e8a40b8b848db6a2f5d14b454ec05be835ac1cac
%bytes
;
; The op-7 DISCONNECT_RSP (sec 4(h)(1b), 4(m)): raw pcap frame 63,
; SCA #55, byte-exact -- answers frame 64/SCA#56 above (Figure 2-16
; matched pair). 58-byte content: envelope + handle pair, no marker,
; frame ends at abs 72. Harvested from src/vmsscs/scs_disc.c
; disc_response_tmpl.
;
@0    08 00 2b 4a b7 15           ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9           ; eth src = VAX2 HW MAC
@12   60 07                       ; ethertype 0x6007
@14   38 00                       ; pl0  length = 0x0038 (58-byte content)
@16   aa 00 04 00 01 04           ; pl2  dst logical (VAX1)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 02 04           ; pl10 src logical (VAX2)
@30   4b 13                       ; pl16 msgtype 0x4b / format 0x13
@32   0d 00 0e 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   0d 00 00 00 0e 00 00 00 0d 00 00 00 01 00 00 02 0e 00 ; pl26..43 counter mirrors + inner length=0x000e(14)
@58   04 00                       ; pl44 format word 0x0004
@60   07 00                       ; pl46 op = 7 DISCONNECT_RSP
@62   00 00                       ; pl48 credit = 0 (GROUNDED 100%, sec 4h(1c))
@64   08 00 05 63                 ; pl50 remote Con.ID
@68   07 00 59 33                 ; pl54 local Con.ID -- frame ENDS here, no marker
