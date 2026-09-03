%OVMX-CLUSTER-SPECIMEN-1
name:      scs-dir-lookup-response-negative
class:     scs-applmsg-94
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 4(h)(2), 4(m) op=10 DATA/DIRECTORY-OP
capture:   formation-ci1-joinwindow.pcap
frame:     31
wire-len:  108
sha256:    b89d6b370741258731fa394a4fb1fed2a3c3bf8c8f2189a0adae75d8709acb8a
%bytes
;
; The op-10 SCS$DIR_LOOKUP negative RESPONSE (sec 4(h)(2)): VAX2
; telling VAX1 that MSCP$TAPE is 'NOT PRESENT HERE' -- SCA #31,
; byte-exact. Same 94-byte shape as the request, with the queried
; name echoed at [62:78] and the literal negative-resolution marker
; at the result field [78:94]. Harvested from src/vmsscs/scs_dir.c
; dir_lookup_tmpl.
;
@0    08 00 2b 4a b7 15           ; eth dst = VAX1 HW MAC
@6    08 00 2b 78 56 b9           ; eth src = VAX2 HW MAC
@12   60 07                       ; ethertype 0x6007
@14   5c 00                       ; pl0  length = 0x005c (94-byte content)
@16   aa 00 04 00 01 04           ; pl2  dst logical (VAX1)
@22   01 00                       ; pl8  connect flag
@24   aa 00 04 00 02 04           ; pl10 src logical (VAX2)
@30   5b 13                       ; pl16 msgtype 0x5b / format 0x13
@32   03 00 03 00 01 00 12 00     ; pl18..25 recv_ack/send_seq/incarn/NISCS_LAN_OVRHD
@40   03 00 00 00 03 00 00 00 03 00 00 00 01 00 00 02 32 00 ; pl26..43 counter mirrors + inner length=0x0032(50)
@58   04 00                       ; pl44 format word 0x0004
@60   0a 00                       ; pl46 op = 10 (application/directory data)
@62   01 00                       ; pl48 credit = 1
@64   08 00 05 63                 ; pl50 remote Con.ID
@68   07 00 59 33                 ; pl54 local Con.ID
@72   01 00 00 00                 ; pl58 RESPONSE marker (GROUNDED, sec 4h)
@76   4d 53 43 50 24 54 41 50 45 20 20 20 20 20 20 20 ; pl62 queried name echoed 'MSCP$TAPE       '
@92   4e 4f 54 20 50 52 45 53 45 4e 54 20 48 45 52 45 ; pl78 result == literal 'NOT PRESENT HERE' (GROUNDED)
