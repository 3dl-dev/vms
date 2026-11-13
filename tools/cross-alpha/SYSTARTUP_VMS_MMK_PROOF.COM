$! SYSTARTUP_VMS.COM - MMK.EXE on OVMX/Alpha proof variant (vms-4ac2; harness-only).
$!
$! Staged onto the ODS-2 volume by tools/cross-alpha/build-alpha-bootimage.sh
$! ONLY when the MMK proof marker is present; the product rootfs-alpha file is
$! never modified.
$!
$! SYS$SYSTEM:MMK.EXE is MadGoat MMK (tests/corpus/tier3-mmk, unchanged) built
$! for the alpha-dec-vms host and linked by the VMS-native LINK.EXE
$! (build-host-mmk.sh). DCL writes a one-target description file; MMK, run as a
$! foreign command, builds the target through its DCL subprocess, finds it
$! current on a second run, and rebuilds it after its source gets a new
$! version. DCL TYPE and F$SEARCH -- readers independent of MMK -- show what
$! each run left on the volume.
$!
$ SET NOON
$ SET DEFAULT VDA0:[SYSTMP]
$ MMK := $SYS$SYSTEM:MMK.EXE
$ WRITE SYS$OUTPUT "MMK-PROOF: === DCL writes DESCRIP.MMS and HELLO.IN ==="
$ OPEN/WRITE D VDA0:[SYSTMP]DESCRIP.MMS
$ WRITE D "HELLO.OUT : HELLO.IN"
$ WRITE D "    WRITE SYS$OUTPUT ""MMK-ACTION: building HELLO.OUT from HELLO.IN"""
$ WRITE D "    COPY HELLO.IN HELLO.OUT"
$ CLOSE D
$ OPEN/WRITE S VDA0:[SYSTMP]HELLO.IN
$ WRITE S "MMK-SOURCE: first"
$ CLOSE S
$ WRITE SYS$OUTPUT "MMK-PROOF: DCL-WRITE-STATUS=''$STATUS'"
$!
$ WRITE SYS$OUTPUT "MMK-PROOF: === RUN 1: target missing, MMK builds it ==="
$ MMK
$ WRITE SYS$OUTPUT "MMK-PROOF: RUN1-STATUS=''$STATUS'"
$ V1 = F$SEARCH("VDA0:[SYSTMP]HELLO.OUT;1")
$ V2 = F$SEARCH("VDA0:[SYSTMP]HELLO.OUT;2")
$ WRITE SYS$OUTPUT "MMK-PROOF: RUN1-FILES V1=[''V1'] V2=[''V2']"
$ WRITE SYS$OUTPUT "MMK-PROOF: === RUN1 TYPE HELLO.OUT ==="
$ TYPE HELLO.OUT
$!
$ WRITE SYS$OUTPUT "MMK-PROOF: === RUN 2: target current, MMK does nothing ==="
$ MMK
$ WRITE SYS$OUTPUT "MMK-PROOF: RUN2-STATUS=''$STATUS'"
$ V1 = F$SEARCH("VDA0:[SYSTMP]HELLO.OUT;1")
$ V2 = F$SEARCH("VDA0:[SYSTMP]HELLO.OUT;2")
$ WRITE SYS$OUTPUT "MMK-PROOF: RUN2-FILES V1=[''V1'] V2=[''V2']"
$!
$ WRITE SYS$OUTPUT "MMK-PROOF: === touch: a new version of HELLO.IN ==="
$ WAIT 00:00:02
$ OPEN/WRITE S VDA0:[SYSTMP]HELLO.IN
$ WRITE S "MMK-SOURCE: second"
$ CLOSE S
$ WRITE SYS$OUTPUT "MMK-PROOF: TOUCH-STATUS=''$STATUS'"
$!
$ WRITE SYS$OUTPUT "MMK-PROOF: === RUN 3: source newer, MMK rebuilds ==="
$ MMK
$ WRITE SYS$OUTPUT "MMK-PROOF: RUN3-STATUS=''$STATUS'"
$ V1 = F$SEARCH("VDA0:[SYSTMP]HELLO.OUT;1")
$ V2 = F$SEARCH("VDA0:[SYSTMP]HELLO.OUT;2")
$ WRITE SYS$OUTPUT "MMK-PROOF: RUN3-FILES V1=[''V1'] V2=[''V2']"
$ WRITE SYS$OUTPUT "MMK-PROOF: === RUN3 TYPE HELLO.OUT ==="
$ TYPE HELLO.OUT
$ WRITE SYS$OUTPUT "MMK-PROOF: === END ==="
$!
$ EXIT
