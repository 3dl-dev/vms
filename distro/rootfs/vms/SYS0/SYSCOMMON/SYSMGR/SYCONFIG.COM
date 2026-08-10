$! SYCONFIG.COM - Site-specific device/configuration startup command
$! procedure (vms-21a).
$!
$! Invoked by SYS$MANAGER:STARTUP.COM's phase driver (STDRV) during the
$! CONFIG phase, before SYLOGICALS.COM and well before SYSTARTUP_VMS.COM's
$! site-specific commands (VSI OpenVMS System Manager's Manual,
$! "Customizing Startup with Site-Specific Files" -- SYCONFIG.COM is the
$! documented home for device and controller configuration commands, run
$! early enough that later startup phases see the configured devices).
$! Edits on the system disk persist across reboots and survive a PRODUCT
$! INSTALL upgrade (seed-once, vms-2c9 -- see
$! tools/ovmx_kit_pack.c is_seed_once_filename()).
$!
$! THIS PROCEDURE CONFIGURES NO DEVICE, AND THAT IS DELIBERATE. OVMX has no
$! device-configuration facility yet (no SET DEVICE-style controller setup)
$! -- a worked example here would name a command that does not exist on
$! OVMX, which would fail on every boot and teach the reader that the
$! failure is normal. Same honesty rule SYSTARTUP_VMS.COM's own header
$! documents for service startup.
$!
$ SET NOON
$!
$ EXIT
