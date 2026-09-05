%OVMX-CLUSTER-SPECIMEN-1
name:      scs-dir-connect-response
class:     scs-conn-ctrl
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(1), 4(h)(2), 4(m) op=2 CONNECT-RESPONSE
capture:   formation-ci1-joinwindow.pcap
frame:     25
wire-len:  124
sha256:    82bae938247cda92eff6470449fa5d828136a6487b10d399a4a2ec828793f9b8
%bytes
;
; The op-2 CONNECT_RESPONSE/ACCEPT_REQ (sec 4(m)): VAX2 accepting
; VAX1's SCS$DIRECTORY connect, supplying its own Con.ID and binding
; the pair. 110-byte content, byte-exact, harvested from
; src/vmsscs/scs_dir.c dir_resp_tmpl (SCA 25). Names swap direction
; vs the CONNECT_REQ (sec 4(h)(2a)): here [62:78]='SCS$DIR_LOOKUP'
; (the RESPONDER's own SYSAP) and [78:94]='SCS$DIRECTORY' (the
; requester's), confirming the (destination, source) reading.
;
@0    08 00 2b 4a b7 15           ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9           ; eth src = VAX2 HW MAC
@12   60 07                       ; ethertype 0x6007
@14   6c 00                       ; pl0  length = 0x006c (110-byte content)
@16   aa 00 04 00 01 04           ; pl2  dst logical (VAX1)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 02 04           ; pl10 src logical (VAX2)
@30   5b 13                       ; pl16 msgtype 0x5b / format 0x13
@32   01 00 02 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   01 00 00 00 02 00 00 00 01 00 00 00 01 00 00 02 42 00 ; pl26..43 counter mirrors + inner length=0x0042(66)
@58   04 00                       ; pl44 format word 0x0004
@60   02 00                       ; pl46 op = 2 CONNECT_RESPONSE
@62   01 00                       ; pl48 credit = 1
@64   08 00 05 63                 ; pl50 remote Con.ID (VAX1's, bound)
@68   07 00 59 33                 ; pl54 local Con.ID (VAX2's own, newly supplied)
@72   00 00 00 00                 ; pl58 marker 00 00 00 00
@76   53 43 53 24 44 49 52 5f 4c 4f 4f 4b 55 50 20 20 ; pl62 'SCS$DIR_LOOKUP  ' (responder's own SYSAP)
@92   53 43 53 24 44 49 52 45 43 54 4f 52 59 20 20 20 ; pl78 'SCS$DIRECTORY   ' (requester's, swapped)
@108  20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 20 ; pl94 trailing blanks
