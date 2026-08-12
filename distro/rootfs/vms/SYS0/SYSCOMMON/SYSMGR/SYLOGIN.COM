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
$ EXIT
