$! PARTS_SETUP.COM - Install the PARTS demo application (bead vms-977)
$!
$! Pre-PCSI layered-product install procedure for the OVMX 0.2 "killer app"
$! demo (parent vms-2579, docs/release-plan-0.2-to-0.5.md section 4/5 - "it
$! runs a real VMS app"). Before PCSI, a VMS layered product commonly shipped
$! its own small SYS$UPDATE:<product>_SETUP.COM that a SYSTEM manager ran by
$! hand after the kit's files were staged on the system disk; this procedure
$! is exactly that for PARTS, not a full VMSINSTAL/PCSI kit.
$!
$! PREREQUISITE: the PARTS kit -- this procedure plus PARTS.EXE, the VMS-
$! native RMS indexed-file demo image (src/apps/parts/, built cc -> LINK.EXE
$! -> IMGACT, proven under QEMU by tests/qemu/test_parts_rms_qemu.sh) -- is
$! staged in SYS$UPDATE:. Getting PARTS.EXE onto the system disk in the
$! first place (mastering it into the bootable image) is tracked separately
$! (vms-cde); this procedure only does the INSTALL step, the same way a
$! layered product's SETUP.COM is independent of how its kit reached the
$! system disk.
$!
$! Invoke from a SYSTEM DCL session as:
$!
$!     $ @SYS$UPDATE:PARTS_SETUP.COM
$!
$! WHAT THIS DOES (matching VMS layered-product convention):
$!
$!   1. COPYs PARTS.EXE from the install kit (SYS$UPDATE:) to SYS$SYSTEM:,
$!      the standard home for installed VMS utility images (where DCL.EXE,
$!      LOGINOUT.EXE, AUTHORIZE.EXE, etc. already live).
$!
$!   2. DEFINEs PARTS as a foreign command:
$!
$!          PARTS :== $SYS$SYSTEM:PARTS.EXE
$!
$!      the standard VMS idiom that turns an installed image into a DCL
$!      verb -- the leading "$" tells DCL the symbol names an image to
$!      activate, not a string to substitute.
$!
$! SCOPE OF "AVAILABLE": the PARTS :== symbol above is a GLOBAL DCL symbol,
$! which on VMS is job-wide, not system-wide or persistent across login --
$! exactly like every other foreign-command definition. Running this
$! procedure makes PARTS available for the REST OF THE CURRENT SESSION. A
$! manager who wants PARTS available to every user on every login adds
$!
$!     $ @SYS$UPDATE:PARTS_SETUP.COM
$!
$! to SYS$MANAGER:SYLOGIN.COM (or a per-user LOGIN.COM), the same place any
$! other layered product's foreign-command definition would go. That step
$! is a site choice, not this procedure's job, and is deliberately left to
$! the manager rather than hardcoded here.
$!
$ SET NOON
$!
$ WRITE SYS$OUTPUT "%PARTS-I-SETUP, OVMX PARTS demo installation starting"
$!
$! Step 1: copy the image from the install kit into SYS$SYSTEM:.
$ WRITE SYS$OUTPUT "%PARTS-I-COPY, copying SYS$UPDATE:PARTS.EXE to SYS$SYSTEM:"
$ COPY SYS$UPDATE:PARTS.EXE SYS$SYSTEM:PARTS.EXE
$!
$! Step 2: define PARTS as a foreign command -- the standard VMS idiom for
$! making an installed image runnable as a DCL verb.
$ WRITE SYS$OUTPUT "%PARTS-I-DEFINE, defining PARTS :== $SYS$SYSTEM:PARTS.EXE"
$ PARTS :== $SYS$SYSTEM:PARTS.EXE
$!
$ WRITE SYS$OUTPUT "%PARTS-I-DONE, PARTS demo installed - type PARTS to run it"
$!
$ EXIT
