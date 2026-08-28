# rd vms-45c rung-ADD evidence -- OVMX<->OVMX JOIN COMPLETES (2026-08-28)

Two OVMX SCSD --connect on one bridge. Full member-role stack:
rung-0 (vms-f3e) solicit + rung-VC (vms-d60) START-initiate + rung-ADD (vms-45c) 0x5b accept.

## OVMX_MCAST_SOLICIT=1 OVMX_JOIN_SEQ=1 -> VERDICT: JOIN COMPLETES
```
rung                                            OVMXA   OVMXB 
----------------------------------------------  ------  ------
rung-0: multicast HELLO emitted                 YES     YES   
rung-0: directed HELLO exchanged (NISCA channel)  YES     YES   
rung-VC: 0x41 START sent                        YES     YES   
rung-VC: START handshake done (start_acked)     YES     YES   
rung-VC: virtual circuit OPEN                   YES     YES   
seq step 1: our SCS$DIRECTORY connect accepted  YES     YES   
seq step 5: our MSCP$DISK connect accepted      YES     YES   
VMS$VAXcluster CONNECT-REQUEST sent (non-seq path)  no      no    
peer CONNECT-REQUEST answered/accepted          YES     YES   
VMS$VAXcluster SYSAP connection OPEN            YES     YES   
CM config/topology exchanged                    YES     YES   
cluster membership state learned                no      no    

highest rung OVMXA : CM config/topology exchanged
highest rung OVMXB : CM config/topology exchanged

VERDICT: JOIN COMPLETES -- both OVMX nodes reached VMS$VAXcluster OPEN.
```

## Both nodes: membership markers
```
OVMXA: 
OVMXB: 
OVMXA/OVMXB each log exactly one: JOINBOUND, VAXCLMEMBER, MSCPBOUND, CMCONFIG
peer detail: vaxcluster_member=CONNECTED(reached-OPEN) connected=YES cm_config=YES sysap_send=3
pcap: 1571 x 0x6007 frames on br0
```

## Flag-off (OVMX_MCAST_SOLICIT absent) -> byte-identical
```
flag-off marker census: HELLOSENT + FRAME + IDENT/LISTEN/DEPARTON/LASTGASP/SUMMARY/CONNSTUCK only
member-role markers (MCASTSOLICIT/STARTTX/JOINBOUND/VAXCLMEMBER/CMCONFIG/SCSENV): 0
```
