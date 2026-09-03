%OVMX-CLUSTER-SPECIMEN-1
name:      scs-dir-connect-echo
class:     scs-conn-ctrl
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(1), 4(m) op=1 CONNECT-ECHO
capture:   formation-ci1-joinwindow.pcap
frame:     23
wire-len:  80
sha256:    e0fa4ee7c5e2e54e8a8857b1eec568f59e1460e7191d404387039663ec68b786
%bytes
;
; The op-1 CONNECT_RSP/ECHO (sec 4(m)): VAX2 echoing VAX1's op-0
; CONNECT_REQ. 66-byte content, byte-exact, harvested from
; src/vmsscs/scs_dir.c dir_echo_tmpl (SCA 23). Note the TRUNCATED
; 4-byte name fragment at pl62 ('SCS$') -- the 66-byte class is too
; short for the full 16-byte name field the 94/110 classes carry.
;
@0    08 00 2b 4a b7 15           ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9           ; eth src = VAX2 HW MAC
@12   60 07                       ; ethertype 0x6007
@14   40 00                       ; pl0  length = 0x0040 (66-byte content)
@16   aa 00 04 00 01 04           ; pl2  dst logical (VAX1)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 02 04           ; pl10 src logical (VAX2)
@30   5b 13                       ; pl16 msgtype 0x5b / format 0x13
@32   01 00 01 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   01 00 00 00 01 00 00 00 01 00 00 00 01 00 00 02 16 00 ; pl26..43 counter mirrors + inner length=0x0016(22)
@58   04 00                       ; pl44 format word 0x0004
@60   01 00                       ; pl46 op = 1 CONNECT_RSP/ECHO
@62   00 00                       ; pl48 credit = 0 (sec 4d)
@64   08 00 05 63                 ; pl50 remote Con.ID (VAX1's offered handle, echoed)
@68   00 00 00 00                 ; pl54 local Con.ID = 0 (not yet assigned)
@72   00 00 01 00                 ; pl58 marker 00 00 01 00
@76   53 43 53 24                 ; pl62 truncated name fragment 'SCS$' (66-content quirk)
