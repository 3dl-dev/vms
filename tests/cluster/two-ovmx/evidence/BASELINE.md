# rd vms-f3e two-OVMX SCS baseline evidence (2026-08-28)

## Run 1: UNMODIFIED main, default flags -> STALLS at rung 0 (channel formation)
```
===================== TWO-OVMX SCS BASELINE VERDICT =====================
run dir : /out
pcap    : out/two-ovmx.pcap  (432 x 0x6007 frames)

rung                                            OVMXA   OVMXB 
----------------------------------------------  ------  ------
multicast HELLO emitted                         YES     YES   
directed HELLO exchanged (NISCA channel)        no      no    
VMS$VAXcluster CONNECT-REQUEST sent             no      no    
peer CONNECT-REQUEST answered/accepted          no      no    
virtual circuit OPEN                            no      no    
VMS$VAXcluster SYSAP connection OPEN            no      no    
CM START sent                                   no      no    
CM START handshake done                         no      no    
CM config/topology exchanged                    no      no    
cluster membership state learned                no      no    

highest rung OVMXA : multicast HELLO emitted
highest rung OVMXB : multicast HELLO emitted

VERDICT: JOIN DID NOT COMPLETE (OVMX<->OVMX join gap).
         See the per-node highest rung above and the pcap for the frame
         that was sent but drew no expected response.

---- tail scsd-OVMXA.log ----
SCSD-I-CONNSTUCK, 0 of 0 in-use connection(s) parked off OPEN
  PB-OPEN: new-sb=0 refreshed=0 existing-sb=0 errors=0 abandoned-masquerade=0
  PEER-DEPARTURES=0 CONNECTIONS-LOST=0 TEARDOWNS-REFUSED=0 LISTEN-TIMEOUT-MS=20000
  DGRAM: conid=0x0F580020 VMS$VAXcluster buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0x0F580021 SCS$DIRECTORY buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0x0F580022 MSCP$DISK buffers=0/0 delivered=0 discarded-no-quota=0
  CM-CONFIG-FRAMES=0 CM-RESPONSES-SENT=0 PADDED-HELLO-SENT=0
  CM-OP04-BARRIER-SEEN=0 (cat 0x01 op 0x04 role 0x50; NON-AUTHORITATIVE, occurs on successful joins too -- spec 4(O.33))
  CREDIT-RETX-ANSWERED=0
  MSCP-SERVER-ACCEPTS-SENT=0
  PSC-UNGATED=0
  SDIR listening=3 [VMS$VAXcluster conid=0x0F580020 LISTEN] [SCS$DIRECTORY conid=0x0F580021 LISTEN] [MSCP$DISK conid=0x0F580022 LISTEN] scans=0 hits=0 delivered=0 retransmits=0 connect_scans=0 no-such-sysap-sent=0 busy-sent=0 refusals-unsent=0 enabled=YES
  POLLER SCS$DIR_LOOKUP interval=30s state=IDLE cycles=0 completed=0 abandoned=0 disc-closed=0 disc-unclosed=0 connect-refused=0 last-connect-status=OK last-refused-node=00:00:00:00:00:00 connects-sent=0 disconnects-sent=0 inquiries-sent=0 answers=0 yes=0 no=0 unknown=0 unsolicited=0 notified=0 skipped=0 forced-cdt-release=0 enabled=no (ships OFF; OVMX_PROCESS_POLLER=1 opts in, OVMX_NO_PROCESS_POLLER=1 forces off)
  READMITMAP-CAVEAT: verdicts below are OVMX-SIDE OBSERVATIONS (NON-AUTHORITATIVE). The authoritative membership signal is each MEMBER's F$GETSYI("CLUSTER_NODES") / CDT layer, not any OVMX-derived verdict (spec 4(O.32)/4(O.33)).
  READMITMAP-SUMMARY incarnation_presented=0x00bc1a3c28b44bc0(live) membership_committed=no(no op 0x06 membership burst this run) members_reached=0 admitted=0 engaged=0 reclaimed_nojoin=0 join_abandoned=0 no_engage=0 -- OVMX-SIDE OBSERVATION (NON-AUTHORITATIVE): mixed/partial -- see the per-peer OBSERVED buckets. Confirm actual membership via the member's F$GETSYI("CLUSTER_NODES"), spec 4(O.33). (spec 4(O.32)/4(O.33)/4(O.38), docs/design-rejoin-cm-state-map.md)

---- tail scsd-OVMXB.log ----
SCSD-I-CONNSTUCK, 0 of 0 in-use connection(s) parked off OPEN
  PB-OPEN: new-sb=0 refreshed=0 existing-sb=0 errors=0 abandoned-masquerade=0
  PEER-DEPARTURES=0 CONNECTIONS-LOST=0 TEARDOWNS-REFUSED=0 LISTEN-TIMEOUT-MS=20000
  DGRAM: conid=0xE08A0020 VMS$VAXcluster buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0xE08A0021 SCS$DIRECTORY buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0xE08A0022 MSCP$DISK buffers=0/0 delivered=0 discarded-no-quota=0
  CM-CONFIG-FRAMES=0 CM-RESPONSES-SENT=0 PADDED-HELLO-SENT=0
  CM-OP04-BARRIER-SEEN=0 (cat 0x01 op 0x04 role 0x50; NON-AUTHORITATIVE, occurs on successful joins too -- spec 4(O.33))
  CREDIT-RETX-ANSWERED=0
  MSCP-SERVER-ACCEPTS-SENT=0
  PSC-UNGATED=0
  SDIR listening=3 [VMS$VAXcluster conid=0xE08A0020 LISTEN] [SCS$DIRECTORY conid=0xE08A0021 LISTEN] [MSCP$DISK conid=0xE08A0022 LISTEN] scans=0 hits=0 delivered=0 retransmits=0 connect_scans=0 no-such-sysap-sent=0 busy-sent=0 refusals-unsent=0 enabled=YES
  POLLER SCS$DIR_LOOKUP interval=30s state=IDLE cycles=0 completed=0 abandoned=0 disc-closed=0 disc-unclosed=0 connect-refused=0 last-connect-status=OK last-refused-node=00:00:00:00:00:00 connects-sent=0 disconnects-sent=0 inquiries-sent=0 answers=0 yes=0 no=0 unknown=0 unsolicited=0 notified=0 skipped=0 forced-cdt-release=0 enabled=no (ships OFF; OVMX_PROCESS_POLLER=1 opts in, OVMX_NO_PROCESS_POLLER=1 forces off)
  READMITMAP-CAVEAT: verdicts below are OVMX-SIDE OBSERVATIONS (NON-AUTHORITATIVE). The authoritative membership signal is each MEMBER's F$GETSYI("CLUSTER_NODES") / CDT layer, not any OVMX-derived verdict (spec 4(O.32)/4(O.33)).
  READMITMAP-SUMMARY incarnation_presented=0x00bc1a3c28b504db(live) membership_committed=no(no op 0x06 membership burst this run) members_reached=0 admitted=0 engaged=0 reclaimed_nojoin=0 join_abandoned=0 no_engage=0 -- OVMX-SIDE OBSERVATION (NON-AUTHORITATIVE): mixed/partial -- see the per-peer OBSERVED buckets. Confirm actual membership via the member's F$GETSYI("CLUSTER_NODES"), spec 4(O.33). (spec 4(O.32)/4(O.33)/4(O.38), docs/design-rejoin-cm-state-map.md)
```

## Run 2: OVMX_MCAST_SOLICIT=1 PoC -> advances to directed-HELLO/channel-up, then stalls at 0x41 START
```
===================== TWO-OVMX SCS BASELINE VERDICT =====================
run dir : /out
pcap    : out/two-ovmx.pcap  (658 x 0x6007 frames)

rung                                            OVMXA   OVMXB 
----------------------------------------------  ------  ------
multicast HELLO emitted                         YES     YES   
directed HELLO exchanged (NISCA channel)        YES     YES   
VMS$VAXcluster CONNECT-REQUEST sent             no      no    
peer CONNECT-REQUEST answered/accepted          no      no    
virtual circuit OPEN                            no      no    
VMS$VAXcluster SYSAP connection OPEN            no      no    
CM START sent                                   no      no    
CM START handshake done                         no      no    
CM config/topology exchanged                    no      no    
cluster membership state learned                no      no    

highest rung OVMXA : directed HELLO exchanged (NISCA channel)
highest rung OVMXB : directed HELLO exchanged (NISCA channel)

VERDICT: JOIN DID NOT COMPLETE (OVMX<->OVMX join gap).
         See the per-node highest rung above and the pcap for the frame
         that was sent but drew no expected response.

---- tail scsd-OVMXA.log ----
  PEER-DEPARTURES=0 CONNECTIONS-LOST=0 TEARDOWNS-REFUSED=0 LISTEN-TIMEOUT-MS=20000
  DGRAM: conid=0x955B0020 VMS$VAXcluster buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0x955B0021 SCS$DIRECTORY buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0x955B0022 MSCP$DISK buffers=0/0 delivered=0 discarded-no-quota=0
  CM-CONFIG-FRAMES=0 CM-RESPONSES-SENT=0 PADDED-HELLO-SENT=1
  CM-OP04-BARRIER-SEEN=0 (cat 0x01 op 0x04 role 0x50; NON-AUTHORITATIVE, occurs on successful joins too -- spec 4(O.33))
  CREDIT-RETX-ANSWERED=0
  MSCP-SERVER-ACCEPTS-SENT=0
  PSC-UNGATED=0
  SDIR listening=3 [VMS$VAXcluster conid=0x955B0020 LISTEN] [SCS$DIRECTORY conid=0x955B0021 LISTEN] [MSCP$DISK conid=0x955B0022 LISTEN] scans=0 hits=0 delivered=0 retransmits=0 connect_scans=0 no-such-sysap-sent=0 busy-sent=0 refusals-unsent=0 enabled=YES
  POLLER SCS$DIR_LOOKUP interval=30s state=IDLE cycles=0 completed=0 abandoned=0 disc-closed=0 disc-unclosed=0 connect-refused=0 last-connect-status=OK last-refused-node=00:00:00:00:00:00 connects-sent=0 disconnects-sent=0 inquiries-sent=0 answers=0 yes=0 no=0 unknown=0 unsolicited=0 notified=0 skipped=0 forced-cdt-release=0 enabled=no (ships OFF; OVMX_PROCESS_POLLER=1 opts in, OVMX_NO_PROCESS_POLLER=1 forces off)
  PEER 06:41:41:31:86:7d vc=CLOSED channel=UP directed_replies=1 incarnation=1 start_replied=0 start_acked=0 dir_connected=no dir_lookups=0 connect_sent=0 connected=no rx->credit_sent=0 retx=0 remote_conid=0x00000000 cm_config=no cm_responses=0 sysap_send=0 sysap_recv=0 padded_replies=1 padded_init=1 peer_padded_sca=1500 vaxcluster_member=no conn[dir=untracked member=untracked joiner=untracked]
  READMITMAP-CAVEAT: verdicts below are OVMX-SIDE OBSERVATIONS (NON-AUTHORITATIVE). The authoritative membership signal is each MEMBER's F$GETSYI("CLUSTER_NODES") / CDT layer, not any OVMX-derived verdict (spec 4(O.32)/4(O.33)).
  READMITMAP peer 06:41:41:31:86:7d cm_responses=0 membership_bursts=0 member_vc=untracked verdict=OBS-0-CM-RESP(OVMX-observed only: cm_responses=0, membership latch not set, OVMX did not drive op 0x02 to this peer. NON-AUTHORITATIVE -- 0 CM responses on OVMX's side does NOT mean the member did not engage: on the authoritative oracle the member DOES open its connection and propose the addition (spec 4(O.32)). Authoritative signal: the member's F$GETSYI("CLUSTER_NODES") / CDT layer.)
  READMITMAP-SUMMARY incarnation_presented=0x00bc1a3c51e856ab(live) membership_committed=no(no op 0x06 membership burst this run) members_reached=1 admitted=0 engaged=0 reclaimed_nojoin=0 join_abandoned=0 no_engage=1 -- OVMX-SIDE OBSERVATION (NON-AUTHORITATIVE): every reached member shows cm_responses=0 with no OVMX membership latch. This is NOT proof of a coordinator refusal or non-admission: on the authoritative oracle the members DO open their connections and propose the addition (spec 4(O.32)), and where op 0x02 was driven it is delivered under the recv_ack ceiling (spec 4(O.27)); the true outcome for a returning identity is a residual-CSB reclaim RACE, not a refusal (spec 4(O.33)). Read the member's F$GETSYI("CLUSTER_NODES") to know the actual membership. (spec 4(O.32)/4(O.33)/4(O.38), docs/design-rejoin-cm-state-map.md)

---- tail scsd-OVMXB.log ----
  PEER-DEPARTURES=0 CONNECTIONS-LOST=0 TEARDOWNS-REFUSED=0 LISTEN-TIMEOUT-MS=20000
  DGRAM: conid=0x9CA10020 VMS$VAXcluster buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0x9CA10021 SCS$DIRECTORY buffers=0/0 delivered=0 discarded-no-quota=0
  DGRAM: conid=0x9CA10022 MSCP$DISK buffers=0/0 delivered=0 discarded-no-quota=0
  CM-CONFIG-FRAMES=0 CM-RESPONSES-SENT=0 PADDED-HELLO-SENT=1
  CM-OP04-BARRIER-SEEN=0 (cat 0x01 op 0x04 role 0x50; NON-AUTHORITATIVE, occurs on successful joins too -- spec 4(O.33))
  CREDIT-RETX-ANSWERED=0
  MSCP-SERVER-ACCEPTS-SENT=0
  PSC-UNGATED=0
  SDIR listening=3 [VMS$VAXcluster conid=0x9CA10020 LISTEN] [SCS$DIRECTORY conid=0x9CA10021 LISTEN] [MSCP$DISK conid=0x9CA10022 LISTEN] scans=0 hits=0 delivered=0 retransmits=0 connect_scans=0 no-such-sysap-sent=0 busy-sent=0 refusals-unsent=0 enabled=YES
  POLLER SCS$DIR_LOOKUP interval=30s state=IDLE cycles=0 completed=0 abandoned=0 disc-closed=0 disc-unclosed=0 connect-refused=0 last-connect-status=OK last-refused-node=00:00:00:00:00:00 connects-sent=0 disconnects-sent=0 inquiries-sent=0 answers=0 yes=0 no=0 unknown=0 unsolicited=0 notified=0 skipped=0 forced-cdt-release=0 enabled=no (ships OFF; OVMX_PROCESS_POLLER=1 opts in, OVMX_NO_PROCESS_POLLER=1 forces off)
  PEER d2:e0:5f:61:18:08 vc=CLOSED channel=UP directed_replies=1 incarnation=1 start_replied=0 start_acked=0 dir_connected=no dir_lookups=0 connect_sent=0 connected=no rx->credit_sent=0 retx=0 remote_conid=0x00000000 cm_config=no cm_responses=0 sysap_send=0 sysap_recv=0 padded_replies=1 padded_init=1 peer_padded_sca=1500 vaxcluster_member=no conn[dir=untracked member=untracked joiner=untracked]
  READMITMAP-CAVEAT: verdicts below are OVMX-SIDE OBSERVATIONS (NON-AUTHORITATIVE). The authoritative membership signal is each MEMBER's F$GETSYI("CLUSTER_NODES") / CDT layer, not any OVMX-derived verdict (spec 4(O.32)/4(O.33)).
  READMITMAP peer d2:e0:5f:61:18:08 cm_responses=0 membership_bursts=0 member_vc=untracked verdict=OBS-0-CM-RESP(OVMX-observed only: cm_responses=0, membership latch not set, OVMX did not drive op 0x02 to this peer. NON-AUTHORITATIVE -- 0 CM responses on OVMX's side does NOT mean the member did not engage: on the authoritative oracle the member DOES open its connection and propose the addition (spec 4(O.32)). Authoritative signal: the member's F$GETSYI("CLUSTER_NODES") / CDT layer.)
  READMITMAP-SUMMARY incarnation_presented=0x00bc1a3c51e85897(live) membership_committed=no(no op 0x06 membership burst this run) members_reached=1 admitted=0 engaged=0 reclaimed_nojoin=0 join_abandoned=0 no_engage=1 -- OVMX-SIDE OBSERVATION (NON-AUTHORITATIVE): every reached member shows cm_responses=0 with no OVMX membership latch. This is NOT proof of a coordinator refusal or non-admission: on the authoritative oracle the members DO open their connections and propose the addition (spec 4(O.32)), and where op 0x02 was driven it is delivered under the recv_ack ceiling (spec 4(O.27)); the true outcome for a returning identity is a residual-CSB reclaim RACE, not a refusal (spec 4(O.33)). Read the member's F$GETSYI("CLUSTER_NODES") to know the actual membership. (spec 4(O.32)/4(O.33)/4(O.38), docs/design-rejoin-cm-state-map.md)
```
