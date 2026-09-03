%OVMX-CLUSTER-SPECIMEN-1
name:      hello-directed-vax2-to-vax1
class:     hello
origin:    spec-composed
spec:      docs/cluster-protocol-spec.md 2, 3, 4(a), 4(a).0, 4(a).1, 4(b)
capture:   scs-idle-baseline.pcap
frame:     2
wire-len:  134
sha256:    6edcabbffab3b28418d95157b227df136b04bedf3dbefc59199fb72360e828d1
%bytes
@0   08 00 2b 4a b7 15          ; eth dst = VAX1's HARDWARE MAC (sec 4a.0)
@6    08 00 2b 78 56 b9          ; eth src = VAX2's HW MAC (sec 2, sec 3)
@12   60 07                      ; ethertype 0x6007
@14   76 00                      ; 120-byte SCA content
@16   aa 00 04 00 01 04          ; dst = VAX1's cluster-LOGICAL addr, NOT its HW
                                 ; MAC. sec 4(a).0: mirroring abs 0 into abs 16
                                 ; makes the peer silently drop every reply.
@22   01 00                      ; connect flag
@24   aa 00 04 00 02 04          ; src LOGICAL addr, VAX2 (sec 3)
@30   b2 00                      ; channel-verify REQUEST b2 (sec 4a.1)
@32   08 00 00 80
@36   05                         ; HELLO
@37   01 00 00
@40   06
@41   56 41 58 32 20 20          ; "VAX2  "
@68   ee 05 39 5b                ; cluster join nonce on a DIRECTED hello (GROUNDED sec 4a)
@92   01 00                      ; node-incarnation the sender attributes to the
                                 ; peer: 1 on fresh contact (GROUNDED sec 4b/4i.B)
@94   92 05
@102  bc 00 03 58 51 41 00 00 00 00
@112  00 00 00 00 00 00 00 00
@120  08 00 2b 78 56 b9          ; sender's real HW LAN MAC (VAX2)
@128  1f 00                      ; poller sweep 31 on a directed HELLO, byte-exact
                                 ; to SDA SHOW PORTS "Poller Sweep 31" (sec 4b)
