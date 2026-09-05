%OVMX-CLUSTER-SPECIMEN-1
name:      scs-dir-lookup-request
class:     scs-applmsg-94
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(2), 4(h)(2a), 4(m) op=10 DATA/DIRECTORY-OP
capture:   formation-ci1-joinwindow.pcap
frame:     29
wire-len:  108
sha256:    595b578d04281a303893c82c157b6d43ca4f3192af793dbbfd4bf158af4b965a
%bytes
;
; The op-10 SCS$DIR_LOOKUP REQUEST (sec 4(h)(2)/(2a)): VAX1 asking
; VAX2 about MSCP$TAPE. Raw pcap frame 37, SCA 29, byte-exact.
; 94-byte content: envelope + marker + queried name [62:78] +
; all-zero result [78:94] (a REQUEST carries no result). Harvested
; from src/vmsscs/scs_dir.c dir_lookupreq_tmpl.
;
@0    08 00 2b 78 56 b9           ; eth dst = VAX2 HW MAC
@6    08 00 2b 4a b7 15           ; eth src = VAX1 HW MAC
@12   60 07                       ; ethertype 0x6007
@14   5c 00                       ; pl0  length = 0x005c (94-byte content)
@16   aa 00 04 00 02 04           ; pl2  dst logical (VAX2)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 01 04           ; pl10 src logical (VAX1)
@30   5b 13                       ; pl16 msgtype 0x5b / format 0x13
@32   02 00 03 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   02 00 00 00 03 00 00 00 02 00 00 00 01 00 00 02 32 00 ; pl26..43 counter mirrors + inner length=0x0032(50)
@58   04 00                       ; pl44 format word 0x0004
@60   0a 00                       ; pl46 op = 10 (application/directory data)
@62   00 00                       ; pl48 credit = 0 (sec 4h(2a): NOT a req/resp discriminator)
@64   07 00 59 33                 ; pl50 remote Con.ID
@68   08 00 05 63                 ; pl54 local Con.ID
@72   00 00 00 00                 ; pl58 REQUEST marker (GROUNDED, sec 4h)
@76   4d 53 43 50 24 54 41 50 45 20 20 20 20 20 20 20 ; pl62 queried name 'MSCP$TAPE       '
@92   00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 00 ; pl78 result: all-zero in a request
