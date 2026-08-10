$! SYLOGICALS.COM - Site-specific logical name definitions (vms-21a).
$!
$! Invoked by SYS$MANAGER:STARTUP.COM's phase driver (STDRV) during the
$! BASEENVIRON phase -- after SYCONFIG.COM has configured devices and after
$! STARTUP.COM's own system logical names (SYS$STARTUP, SYS$LOGIN,
$! SYS$UPDATE) exist, but before SYSTARTUP_VMS.COM starts any site service
$! (VSI OpenVMS System Manager's Manual, "Customizing Startup with
$! Site-Specific Files" -- SYLOGICALS.COM is the documented home for a
$! site's own DEFINE/SYSTEM commands, run early enough that every later
$! startup phase and every login session sees them).
$!
$! This REPLACES the old SYS$MANAGER:SYLOGICALS.CONF (deleted, vms-21a): a
$! real VMS site file is a command procedure a manager edits with DCL
$! DEFINE commands, never a "TABLE NAME VALUE" config file for a daemon to
$! parse. Edits on the system disk persist across reboots and survive a
$! PRODUCT INSTALL upgrade (seed-once, vms-2c9 -- see
$! tools/ovmx_kit_pack.c is_seed_once_filename()).
$!
$ SET NOON
$!
$! ---------------------------------------------------------------------
$! Login banners (LOGINOUT, src/libvms/include/ovmx_banner.h).
$!
$! SYS$ANNOUNCE is displayed BEFORE the "Username:" prompt.
$! SYS$WELCOME  is displayed AFTER a successful login.
$!
$! Both are left UNDEFINED here, as OpenVMS ships them: with no SYS$WELCOME
$! defined, the built-in OVMX banner is displayed, and with no SYS$ANNOUNCE
$! nothing is displayed before the prompt. Uncomment to override.
$!
$! An equivalence string beginning with "@" names a FILE whose contents are
$! displayed -- use that for a multi-line site banner.
$!
$! $ DEFINE/SYSTEM SYS$ANNOUNCE "Unauthorized access is prohibited."
$! $ DEFINE/SYSTEM SYS$WELCOME "@SYS$MANAGER:WELCOME.TXT"
$!
$ EXIT
