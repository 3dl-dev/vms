# rd vms-164d DLM rung-1b evidence -- LIVE cross-node $ENQ round-trip (2026-08-28)

Two OVMX SCSD --connect on one bridge, both joined (full VAXCLMEMBER).
SCSD_ENV: OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1 OVMX_DLM_ENQ=RESONE

## LIVE A->B->A round-trip (symmetric -- both nodes both directions)
```
-- node A --
SCSD-I-DLMENQ  sent $ENQ resnam='RESONE' mode=EX to peer DLM server 0xFE1D000F (our client 0x8074000E)
SCSD-I-DLMRX   cross-node ENQ from CSID=1602 -> executive status=0x00000A78
SCSD-I-DLMGRANT sent GRANT status=0x00000A78 back to CSID=1602
SCSD-I-DLMDONE round-trip COMPLETE: peer GRANT status=0x00000A78 -- LIVE A->B->A proven; lock NOT granted
(node B symmetric, CSIDs swapped)
```

status 0x00000A78 = 2680 = SS$_NOSUCHDEV -- the HONEST fail-status: the Docker
harness has no /dev/vms (Rule 9: Docker is not a runtime). On a real /dev/vms the
executive's rung-1 dispatch stub returns SS$_UNSUPPORTED (2296). Either way INV-6:
a GRANT carrying an error status is NOT a lock grant -- the transport worked, the lock did not.

## Wire: 152-byte MTYPE-10 DLM frames carrying the resource name
```
ethertype SCA (0x6007), length 152; DLM body [B_RESNAM 48:80] = 'RESONE'
pcap total: 1627 x 0x6007 frames (join + DLM exchange)
```

## Join still completes + flag-off byte-identical
```
both nodes: SCSD-I-VAXCLMEMBER (join unaffected)
flag-off (OVMX_MCAST_SOLICIT absent, OVMX_DLM_ENQ still set): multicast HELLO only,
  zero DLM/member-role markers -- the $ENQ send is fully suppressed.
```
