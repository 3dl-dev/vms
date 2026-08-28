# rd vms-d60 rung-VC evidence (2026-08-28)

Two OVMX SCSD --connect on one bridge. rung-0 (vms-f3e) + rung-VC (vms-d60) member-role initiate.

## OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1 -> rung-VC complete, sequencer climbs 5/8 steps
```
SCSD-I-STARTTX, INITIATED round-0 START (member role, OVMX_MCAST_SOLICIT; VC START SENT) to 3a:e8:8e:7f:9a:80 (sysid=1601 node='OVMXA' send_seq=1 incarnation=1 sys_incarnation=0x00bc1a4a784793df)
SCSD-I-STARTTX, sent round-1 STACK (VC START RECEIVED)
SCSD-I-STARTDONE, START/config complete with peer 3a:e8:8e:7f:9a:80 -- VC reset (send_seq=1 recv_seq=0), awaiting 0x4b connect
 SCSD-I-VCOPEN, path block OPEN, node learned for the first time (configuration queue holds 2 system blocks, 1 path block(s) on this node's SB)
SCSD-I-JOINSEQ, opened OUR SCS$DIRECTORY client connect local=0x846A0008 seq=1 (join step 1/8)
SCSD-I-DIRCONN, bound SCS$DIRECTORY: remote=0x95320008 local=0x846A0007 with peer 3a:e8:8e:7f:9a:80
SCSD-I-OWNDIRBOUND, member accepted OUR SCS$DIRECTORY connect: local=0x846A0008 remote=0x95320007
SCSD-I-JOINSEQ, dir confirm + lookup MSCP$TAPE seq=5 (steps 2-3/8)
SCSD-I-JOINSEQ, MSCP$TAPE miss; lookup MSCP$DISK seq=7 (step 4/8)
SCSD-I-JOINSEQ, MSCP$DISK HIT; 2nd lookup + MSCP$DISK connect local=0x846A000A seq=10 (step 5/8)
SCSD-I-MSCPBOUND, member accepted OUR MSCP$DISK connect: local=0x846A000A remote=0x9532000B
SCSD-I-JOINSEQ, lookup VMS$VAXcluster seq=16 (step 6/8)
SCSD-I-JOINSEQ, VMS$VAXcluster HIT; VC connect local=0x846A0002 seq=20 (step 7/8)
SCSD-I-JOINSEQ, retransmit join step 6 (retx 1)
SCSD-I-JOINSEQ, retransmit join step 6 (retx 2)
SCSD-I-JOINSEQ, retransmit join step 6 (retx 3)
SCSD-I-JOINSEQ, retransmit join step 6 (retx 4)
SCSD-I-JOINSEQ, retransmit join step 6 (retx 5)
SCSD-I-JOINSEQ, retransmit join step 6 (retx 6)
SCSD-I-JOINSEQ, retransmit join step 6 (retx 7)
...
  PEER 3a:e8:8e:7f:9a:80 vc=OPEN channel=UP directed_replies=1 incarnation=1 start_replied=0 start_acked=1 dir_connected=YES dir_lookups=4 connect_sent=0 connected=no rx->credit_sent=37 retx=0 remote_conid=0x00000000 cm_config=no cm_responses=0 sysap_send=0 sysap_recv=0 padded_replies=1 padded_init=1 peer_padded_sca=1500 vaxcluster_member=no conn[dir=DISC SENT member=untracked joiner=CONNECT SENT]
```

Next stall: step 6/7 retransmit to JOIN_RETX_MAX -- peer transport-accepts the VMS$VAXcluster VC connect but sends no ACCEPT_REQUEST (member-side accept = next rung). start_acked=1, dir_connected=YES, MSCPBOUND reached.

## Flag-off (OVMX_MCAST_SOLICIT absent) -> byte-identical: multicast HELLO only, zero initiate
```
STARTTX/DIRHELLO/MCASTSOLICIT count in flag-off run: 0 (confirmed)
markers present flag-off: HELLOSENT, FRAME, IDENT, LISTEN, DEPARTON, LASTGASP, SUMMARY, CONNSTUCK -- no member-role marker
```
