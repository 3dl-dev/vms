%OVMX-CLUSTER-SPECIMEN-1
name:      scs-disc-request
class:     scs-conn-ctrl
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(1a), 4(h)(1b), 4(m) op=6 DISCONNECT-REQUEST
capture:   formation-ci1.pcap
frame:     56
wire-len:  76
sha256:    09775e9265c668027fab25c6592b958b264fbf4b6a49ede96206cd5458e65c1a
%bytes
;
; The op-6 DISCONNECT_REQ (sec 4(h)(1b), 4(m)): raw pcap frame 64,
; SCA #56, byte-exact. This is the MATCHING half of a Figure 2-16
; teardown (pl60/abs74 matching flag == 0x0001, sec 4(h)(1b) CENSUS-E
; rank 1). Harvested from src/vmsscs/scs_disc.c disc_request_tmpl.
;
@0    08 00 2b 4a b7 15           ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9           ; eth src = VAX2 HW MAC
@12   60 07                       ; ethertype 0x6007
@14   3c 00                       ; pl0  length = 0x003c (62-byte content)
@16   aa 00 04 00 01 04           ; pl2  dst logical (VAX1)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 02 04           ; pl10 src logical (VAX2)
@30   4b 13                       ; pl16 msgtype 0x4b / format 0x13
@32   0e 00 0f 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   0e 00 00 00 0f 00 00 00 0e 00 00 00 01 00 00 02 12 00 ; pl26..43 counter mirrors + inner length=0x0012(18)
@58   04 00                       ; pl44 format word 0x0004
@60   06 00                       ; pl46 op = 6 DISCONNECT_REQ
@62   00 00                       ; pl48 credit = 0 (GROUNDED 100%, sec 4h(1c))
@64   08 00 05 63                 ; pl50 remote Con.ID
@68   07 00 59 33                 ; pl54 local Con.ID
@72   00 00 01 00                 ; pl58 marker: reason=0x0000, matching-flag=0x0001 (rank 1)
