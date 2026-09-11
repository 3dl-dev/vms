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
$! (ip route / interface address+up / resolver + the TCPIP$INET_HOSTADDR logical)
$! but does NOT re-write the store -- so re-applying the saved config does NOT
$! append a duplicate record every boot. The store is the SOURCE; startup applies
$! FROM it, never TO it.
$!
$! ORDERING: interfaces first (a route needs its interface up), then routes, then
$! name service. A store that does not exist yet is skipped honestly via the
$! OPEN/ERROR path (no F$SEARCH pre-check -- F$SEARCH of a $-in-name file over the
$! ACP does not resolve reliably; TCPIP$STARTUP.COM documents the same gap).
$!
$! EOF is detected by $STATUS .EQ. 2160 (SS$_ENDOFFILE) after READ -- OVMX DCL has
$! no READ/END_OF_FILE= qualifier (SYS$MANAGER:STARTUP.COM uses the same idiom).
$!
$! INVOCATION: @SYS$STARTUP:TCPIP$REAPPLY  (called by TCPIP$STARTUP before the
$! auxiliary server starts; runs privileged so the SET verbs actually apply).
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$!
$! --- Interfaces -------------------------------------------------------------
$ OPEN/READ/ERROR=REAPPLY_ROUTES IFF SYS$SYSTEM:TCPIP$INTERFACE.DAT
$IF_LOOP:
$ READ IFF IFLINE
$ IF $STATUS .EQ. 2160 THEN GOTO IF_DONE
$ IF IFLINE .EQS. "" THEN GOTO IF_LOOP
$ IF F$EXTRACT(0,1,IFLINE) .EQS. "!" THEN GOTO IF_LOOP
$ IFNAME = F$ELEMENT(0," ",IFLINE)
$ IFADDR = F$ELEMENT(1," ",IFLINE)
$ IFMASK = F$ELEMENT(2," ",IFLINE)
$ IF IFMASK .EQS. " " THEN TCPIP SET INTERFACE 'IFNAME' /HOST='IFADDR' /REAPPLY
$ IF IFMASK .NES. " " THEN TCPIP SET INTERFACE 'IFNAME' /HOST='IFADDR' /NETWORK_MASK='IFMASK' /REAPPLY
$ SAY "%TCPIP-I-REAPPLY, reapplied interface ''IFNAME' ''IFADDR'"
$ GOTO IF_LOOP
$IF_DONE:
$ CLOSE IFF
$!
$! --- Routes -----------------------------------------------------------------
$REAPPLY_ROUTES:
$ OPEN/READ/ERROR=REAPPLY_NAMESERVICE RTF SYS$SYSTEM:TCPIP$ROUTE.DAT
$RT_LOOP:
$ READ RTF RTLINE
$ IF $STATUS .EQ. 2160 THEN GOTO RT_DONE
$ IF RTLINE .EQS. "" THEN GOTO RT_LOOP
$ IF F$EXTRACT(0,1,RTLINE) .EQS. "!" THEN GOTO RT_LOOP
$ RT0 = F$ELEMENT(0," ",RTLINE)
$ IF RT0 .EQS. "DEFAULT" THEN GOTO RT_DEFAULT
$ RTGW = F$ELEMENT(1," ",RTLINE)
$ RTMASK = F$ELEMENT(2," ",RTLINE)
$ IF RTMASK .EQS. " " THEN TCPIP SET ROUTE /DESTINATION='RT0' /GATEWAY='RTGW' /REAPPLY
$ IF RTMASK .NES. " " THEN TCPIP SET ROUTE /DESTINATION='RT0' /GATEWAY='RTGW' /NETWORK_MASK='RTMASK' /REAPPLY
$ SAY "%TCPIP-I-REAPPLY, reapplied route ''RT0' via ''RTGW'"
$ GOTO RT_LOOP
$RT_DEFAULT:
$ RTGW = F$ELEMENT(1," ",RTLINE)
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
$ OPEN/READ/ERROR=REAPPLY_DONE NSF SYS$SYSTEM:TCPIP$NAMESERVICE.DAT
$NS_LOOP:
$ READ NSF NSLINE
$ IF $STATUS .EQ. 2160 THEN GOTO NS_DONE
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
