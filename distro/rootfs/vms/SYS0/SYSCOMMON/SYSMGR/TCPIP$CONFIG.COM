$! TCPIP$CONFIG.COM - TCP/IP Services for OVMX core configuration procedure
$! (vms-67f).
$!
$! The DCL-invokable configuration front end for the TCP/IP Services layered
$! product. On real OpenVMS, TCPIP$CONFIG is the menu-driven tool that sets up
$! TCP/IP: it defines the core environment (host name, domain), configures the
$! network interfaces and routes, and enables services. This procedure is the
$! OVMX core-environment equivalent: it records the local host name, domain and
$! interface address THE ORDINARY VMS WAY, so the sockets veneer and the
$! TCPIP tools read the same configuration back.
$!
$! WHAT IT CONFIGURES (the foundational rung, vms-67f):
$!   - The core environment: TCPIP$INET_HOST and TCPIP$INET_DOMAIN, defined as
$!     EXECUTIVE-mode SYSTEM logical names -- executive-resident, shared by
$!     every process on the node (the same names real TCP/IP Services defines).
$!   - One interface's IP address/mask, applied to the interface over the
$!     substrate stack and recorded in TCPIP$INET_HOSTADDR (TCPIP SET INTERFACE).
$!   - Optionally a default gateway and a DNS name server.
$!
$! HONEST DEFERRALS (docs/design-tcpip-services-ovmx.md sec 5): this core rung
$! does NOT do DHCP/auto-configuration, multiple interfaces, or the persistent
$! service/proxy databases. Those are later rungs and are not faked here.
$!
$! INVOCATION:
$!   @SYS$MANAGER:TCPIP$CONFIG                        ! interactive (prompts)
$!   @SYS$MANAGER:TCPIP$CONFIG host domain iface addr mask [gateway] [nameserver]
$!                                                    ! parameter-driven
$!
$! INV-6: the TCPIP$ SYSTEM logicals are executive-resident. With no executive
$! (/dev/vms absent) DEFINE/SYSTEM and TCPIP SET INTERFACE report the honest
$! failure -- this procedure never fabricates a durable configuration.
$!
$ SET NOON
$ SAY := WRITE SYS$OUTPUT
$ SAY ""
$ SAY "        OVMX TCP/IP Services -- Core Configuration (TCPIP$CONFIG)"
$ SAY ""
$!
$! --- Core environment: host name + domain -----------------------------------
$ HOSTNAME = P1
$ IF HOSTNAME .EQS. "" THEN INQUIRE HOSTNAME "    Local host name"
$ IF HOSTNAME .EQS. "" THEN GOTO NO_HOST
$ DOMAIN = P2
$ IF DOMAIN .EQS. "" .AND. P1 .EQS. "" THEN INQUIRE DOMAIN "    Local domain name"
$!
$! /SYSTEM alone puts these in the executive-resident SYSTEM logical-name table
$! (dcl_cmd_io.c: SYSTEM -> the shared, executive-resident table -- shared by every
$! process on the node, which is the "EXECUTIVE-mode SYSTEM logical" intent above).
$! The OVMX logical-name model has no separate access-mode dimension, so DCL DEFINE
$! takes /SYSTEM|/GROUP|/JOB scope qualifiers but NOT the real-VMS /EXECUTIVE_MODE
$! access-mode qualifier -- passing /EXECUTIVE yielded %DCL-W-IVQUAL and the DEFINE
$! silently did nothing (host/domain read back "(not configured)"). Adding
$! /EXECUTIVE_MODE acceptance to DCL DEFINE is tracked separately; /SYSTEM is the
$! correct OVMX spelling and achieves the executive-resident, node-shared intent.
$! CONFIG_OK tracks whether every core step succeeded, so the closing message is
$! HONEST (INV-6): the procedure must NOT print %TCPIP-S-CONFIGURED if a DEFINE or
$! the interface SET failed (e.g., no executive -> SS$_NOSUCHDEV, or an unknown
$! interface -> the SET is refused). Each risky step's $SEVERITY is captured into
$! SEV in ONE assignment immediately after it (a symbol assignment resets $STATUS/
$! $SEVERITY to success, so a later read would lose the real result) and tested
$! off SEV. Severity 1 == success.
$ CONFIG_OK = 1
$ DEFINE/SYSTEM TCPIP$INET_HOST "''HOSTNAME'"
$ SEV = $SEVERITY
$ IF SEV .NE. 1 THEN CONFIG_OK = 0
$ IF DOMAIN .NES. "" THEN DEFINE/SYSTEM TCPIP$INET_DOMAIN "''DOMAIN'"
$ SEV = $SEVERITY
$ IF DOMAIN .NES. "" .AND. SEV .NE. 1 THEN CONFIG_OK = 0
$ SAY "%TCPIP-I-CORE, core environment set: host ''HOSTNAME', domain ''DOMAIN'"
$!
$! --- Interface address -------------------------------------------------------
$ IFNAME = P3
$ IF IFNAME .EQS. "" .AND. P1 .EQS. "" THEN INQUIRE IFNAME "    Interface (e.g. SE0)"
$ IF IFNAME .EQS. "" THEN GOTO DO_GATEWAY
$ IPADDR = P4
$ IF IPADDR .EQS. "" .AND. P1 .EQS. "" THEN INQUIRE IPADDR "    IP address"
$ IF IPADDR .EQS. "" THEN GOTO DO_GATEWAY
$ NETMASK = P5
$ IF NETMASK .EQS. "" .AND. P1 .EQS. "" THEN INQUIRE NETMASK "    Network mask"
$!
$ IF NETMASK .EQS. "" THEN TCPIP SET INTERFACE 'IFNAME' /HOST='IPADDR'
$ IF NETMASK .NES. "" THEN TCPIP SET INTERFACE 'IFNAME' /HOST='IPADDR' /NETWORK_MASK='NETMASK'
$ SEV = $SEVERITY
$ IF SEV .NE. 1 THEN CONFIG_OK = 0
$!
$DO_GATEWAY:
$! --- Optional default gateway ------------------------------------------------
$ GATEWAY = P6
$ IF GATEWAY .NES. "" THEN TCPIP SET ROUTE /DEFAULT /GATEWAY='GATEWAY'
$!
$! --- Optional DNS name server ------------------------------------------------
$ NAMESERVER = P7
$ IF NAMESERVER .NES. "" .AND. DOMAIN .NES. "" THEN -
     TCPIP SET NAME_SERVICE /SYSTEM /SERVER='NAMESERVER' /DOMAIN='DOMAIN'
$ IF NAMESERVER .NES. "" .AND. DOMAIN .EQS. "" THEN -
     TCPIP SET NAME_SERVICE /SYSTEM /SERVER='NAMESERVER'
$!
$ SAY ""
$ SAY "    Current TCP/IP core configuration:"
$ TCPIP SHOW CONFIGURATION
$ IF .NOT. CONFIG_OK THEN GOTO CONFIG_INCOMPLETE
$ SAY "%TCPIP-S-CONFIGURED, TCP/IP Services core configuration complete"
$ EXIT 1
$!
$CONFIG_INCOMPLETE:
$! INV-6: a core step (a host/domain logical DEFINE or the interface SET) failed,
$! so DO NOT claim success. The failing command already reported its specific error
$! above; report the aggregate honestly and exit with an error severity rather than
$! the false %TCPIP-S-CONFIGURED the procedure used to print unconditionally.
$ SAY "%TCPIP-E-CFGINCOMPLETE, TCP/IP Services core configuration INCOMPLETE -- a step failed (see the error above); not claiming success"
$ EXIT 2
$!
$NO_HOST:
$ SAY "%TCPIP-I-NOCONFIG, no host name supplied -- nothing configured"
$ EXIT 1
