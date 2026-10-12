$! SYLOGIN.COM - System-wide login command procedure
$!
$! This file is executed for every interactive login.
$! Place system-wide settings and announcements here (symbol definitions,
$! site-wide logical names -- NOT a welcome banner).
$!
$! NO WELCOME BANNER HERE (vms-2213). The post-login greeting is SYS$WELCOME,
$! which LOGINOUT prints once after authentication (src/tools/vms_login.c ->
$! ovmx_banner_welcome, src/libvms/include/ovmx_banner.h). SYLOGIN.COM used to
$! also WRITE a second "Welcome to OpenVMX ..." line, so every login printed
$! TWO different welcome messages -- a fidelity defect. Real OpenVMS prints
$! exactly one SYS$WELCOME and its SYLOGIN.COM does not emit a welcome of its
$! own (VSI OpenVMS System Manager's Manual, Vol I, "Customizing the System"
$! -- SYLOGIN.COM is the site login command procedure; the login greeting is
$! the SYS$WELCOME logical, displayed by LOGINOUT). The duplicate line is
$! removed; the single SYS$WELCOME remains.
$!
$! Turn on the processing of <Control/T>, as VMS's site template does (the
$! VAX V7.3 node's SYS$MANAGER:SYLOGIN.COM, keystroke probe Q.CTRLT; rd
$! vms-bd71). CTRL/T then shows a one-line status at an interactive terminal.
$! Identify the terminal on an interactive login, as the VMS site template
$! (SYS$MANAGER:SYLOGIN.TEMPLATE, VAX V7.3 -- docs/oracle/vax73-sylogin-template.md)
$! does: SET TERMINAL/INQUIRE, keeping the page length and width, for every
$! terminal but a network or virtual one (device prefixes TW RT WT TK WS PY FT
$! TN). A terminal that does not answer is left as it is, with %SET-W-NOTSET.
$ IF F$MODE() .NES. "INTERACTIVE" THEN GOTO NO_INQUIRE
$ NOINQ = "|TW|RT|WT|TK|WS|PY|FT|TN|"
$ TTPFX = F$EXTRACT(1, 2, F$GETDVI("TT", "TT_PHYDEVNAM"))
$ IF F$LOCATE("|''TTPFX'|", NOINQ) .NE. F$LENGTH(NOINQ) THEN GOTO NO_INQUIRE
$ TTPG = F$GETDVI("TT", "TT_PAGE")
$ TTWD = F$GETDVI("TT", "DEVBUFSIZ")
$ SET TERMINAL/INQUIRE/PAGE='TTPG'/WIDTH='TTWD'
$NO_INQUIRE:
$ IF F$MODE() .EQS. "INTERACTIVE" THEN SET CONTROL=T
$ EXIT
