$! TCPIP$REAPPLY.COM - reapply the persisted TCP/IP configuration at startup
$! (rd vms-b97, P3 of the config-persistence tree vms-a0b2).
$!
$! On real OpenVMS, TCPIP$STARTUP reapplies the configuration the operator saved
$! with TCPIP$CONFIG so a configured node comes back configured after a reboot.
$! OVMX persists that configuration in three SYS$SYSTEM: stores over the Files-11
$! ACP (rd vms-210/vms-402): TCPIP$INTERFACE.DAT, TCPIP$ROUTE.DAT,
$! TCPIP$NAMESERVICE.DAT. This procedure reads each store over the ACP and
$! RE-APPLIES each record by re-running its TCPIP SET verb with /REAPPLY.
$!
$! /REAPPLY IS APPLY-ONLY (rd vms-b679): the SET verb performs the live effect
$! but does NOT re-write the store -- so re-applying the saved config does NOT
$! append a duplicate record every boot. The store is the SOURCE; startup applies
$! FROM it, never TO it.
$!
$! ROBUSTNESS (this MUST NOT wedge system startup -- it runs before the login
$! prompt, from TCPIP$STARTUP): use a BARE "OPEN/READ" (no /ERROR= qualifier --
$! SYS$MANAGER:STARTUP.COM, the proven ACP OPEN/READ template, does NOT use it),
$! then test $STATUS: a store that does not exist / cannot be opened is skipped
$! ("IF .NOT. $STATUS THEN GOTO <next>"). The READ loop terminates on ANY
$! non-success ("IF .NOT. $STATUS"), i.e. EOF (SS$_ENDOFFILE) OR a read error --
$! never only SS$_ENDOFFILE, or a non-EOF failure would loop forever.
$!
$! INVOCATION: @SYS$STARTUP:TCPIP$REAPPLY  (called by TCPIP$STARTUP before the
$! auxiliary server starts; runs privileged so the SET verbs actually apply).
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$!
$! --- Interfaces -------------------------------------------------------------
$ OPEN/READ IFF SYS$SYSTEM:TCPIP$INTERFACE.DAT
$ IF .NOT. $STATUS THEN GOTO REAPPLY_ROUTES
$IF_LOOP:
$ READ IFF IFLINE
$ IF .NOT. $STATUS THEN GOTO IF_DONE
$ IF IFLINE .EQS. "" THEN GOTO IF_LOOP
$ IF F$EXTRACT(0,1,IFLINE) .EQS. "!" THEN GOTO IF_LOOP
$ IFNAME = F$ELEMENT(0," ",IFLINE)
$ IFADDR = F$ELEMENT(1," ",IFLINE)
$ IFMASK = F$ELEMENT(2," ",IFLINE)
$ IF IFNAME .EQS. "" .OR. IFADDR .EQS. " " THEN GOTO IF_LOOP
$ IF IFMASK .EQS. " " THEN TCPIP SET INTERFACE 'IFNAME' /HOST='IFADDR' /REAPPLY
$ IF IFMASK .NES. " " THEN TCPIP SET INTERFACE 'IFNAME' /HOST='IFADDR' /NETWORK_MASK='IFMASK' /REAPPLY
$ SAY "%TCPIP-I-REAPPLY, reapplied interface ''IFNAME' ''IFADDR'"
$ GOTO IF_LOOP
$IF_DONE:
$ CLOSE IFF
$!
$! --- Routes -----------------------------------------------------------------
$REAPPLY_ROUTES:
$ OPEN/READ RTF SYS$SYSTEM:TCPIP$ROUTE.DAT
$ IF .NOT. $STATUS THEN GOTO REAPPLY_NAMESERVICE
$RT_LOOP:
$ READ RTF RTLINE
$ IF .NOT. $STATUS THEN GOTO RT_DONE
$ IF RTLINE .EQS. "" THEN GOTO RT_LOOP
$ IF F$EXTRACT(0,1,RTLINE) .EQS. "!" THEN GOTO RT_LOOP
$ RT0 = F$ELEMENT(0," ",RTLINE)
$ RTGW = F$ELEMENT(1," ",RTLINE)
$ IF RTGW .EQS. " " THEN GOTO RT_LOOP
$ IF RT0 .EQS. "DEFAULT" THEN GOTO RT_DEFAULT
$ RTMASK = F$ELEMENT(2," ",RTLINE)
$ IF RTMASK .EQS. " " THEN TCPIP SET ROUTE /DESTINATION='RT0' /GATEWAY='RTGW' /REAPPLY
$ IF RTMASK .NES. " " THEN TCPIP SET ROUTE /DESTINATION='RT0' /GATEWAY='RTGW' /NETWORK_MASK='RTMASK' /REAPPLY
$ SAY "%TCPIP-I-REAPPLY, reapplied route ''RT0' via ''RTGW'"
$ GOTO RT_LOOP
$RT_DEFAULT:
$ TCPIP SET ROUTE /DEFAULT /GATEWAY='RTGW' /REAPPLY
$ SAY "%TCPIP-I-REAPPLY, reapplied default route via ''RTGW'"
$ GOTO RT_LOOP
$RT_DONE:
$ CLOSE RTF
$!
$! --- Name service -----------------------------------------------------------
$REAPPLY_NAMESERVICE:
$ NS_SERVER = ""
$ NS_DOMAIN = ""
$ OPEN/READ NSF SYS$SYSTEM:TCPIP$NAMESERVICE.DAT
$ IF .NOT. $STATUS THEN GOTO REAPPLY_DONE
$NS_LOOP:
$ READ NSF NSLINE
$ IF .NOT. $STATUS THEN GOTO NS_DONE
$ IF NSLINE .EQS. "" THEN GOTO NS_LOOP
$ IF F$EXTRACT(0,1,NSLINE) .EQS. "!" THEN GOTO NS_LOOP
$ NS_KEY = F$ELEMENT(0,"=",NSLINE)
$ NS_VAL = F$ELEMENT(1,"=",NSLINE)
$ IF NS_KEY .EQS. "SERVER" THEN NS_SERVER = NS_VAL
$ IF NS_KEY .EQS. "DOMAIN" THEN NS_DOMAIN = NS_VAL
$ GOTO NS_LOOP
$NS_DONE:
$ CLOSE NSF
$ IF NS_SERVER .NES. "" .AND. NS_DOMAIN .EQS. "" THEN -
       TCPIP SET NAME_SERVICE /SYSTEM /SERVER='NS_SERVER' /REAPPLY
$ IF NS_SERVER .NES. "" .AND. NS_DOMAIN .NES. "" THEN -
       TCPIP SET NAME_SERVICE /SYSTEM /SERVER='NS_SERVER' /DOMAIN='NS_DOMAIN' /REAPPLY
$ IF NS_SERVER .NES. "" THEN SAY "%TCPIP-I-REAPPLY, reapplied name service ''NS_SERVER'"
$!
$REAPPLY_DONE:
$ EXIT
