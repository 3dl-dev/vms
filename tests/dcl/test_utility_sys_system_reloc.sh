#!/bin/bash
# TEST: DCL utility activation resolves through the SYS$SYSTEM logical (vms-7d8)
#
# THE DOGFOOD (rd vms-7d8, parent vms-704, program vms-8ad). Operator: "we
# don't just *can* use logicals, we *do* -- and users can see it." On OpenVMS
# every SYS$SYSTEM: image is located by TRANSLATING the SYS$SYSTEM logical, so
# redefining SYS$SYSTEM relocates where the DCL utilities (SYSGEN, MAIL,
# AUTHORIZE, ...) activate from -- the same machinery `$ RUN` uses (VSI OpenVMS
# DCL Dictionary, RUN entry; VSI OpenVMS System Manager's Manual, the SYS$SYSTEM
# system logical name).
#
# THE SPLIT-BRAIN THIS GATES. dcl_exec_utility() (src/vmsdcl/dcl_cmd_misc.c)
# used to build the image path directly from the compile-time VMS_SYSTEM_DIR
# (SYSDISK_MOUNT/SYS0/SYSCOMMON/SYSEXE, ovmx_layout.h), BYPASSING the SYS$SYSTEM
# logical -- while RUN/foreign-command activation DID translate it. So a
# redefinition of SYS$SYSTEM relocated RUN but NOT utility activation. This
# gate redefines SYS$SYSTEM to a private directory holding a marker "SYSGEN.EXE"
# and invokes the SYSGEN verb: the marker only appears if activation went
# through the logical.
#
# FAILS-ON-FACADE. On the pre-fix code the SYSGEN verb ignores SYS$SYSTEM,
# looks under VMS_SYSTEM_DIR (absent here) and falls to a bare-name PATH search
# (denied below by the pruned PATH), so the marker NEVER prints -- this test is
# red. After the fix SYS$SYSTEM:SYSGEN.EXE resolves to the relocated marker and
# it prints -- green. The unique marker string is emitted by no other binary,
# so even a fully provisioned /vms with a real SYSGEN.EXE cannot make it pass by
# accident.
#
# EXPECT: contains:OVMX-VMS7D8-UTILITY-VIA-SYS$SYSTEM
# EXPECT_NOT: contains:-F-NOIMG

set -u

VMSDCL="${VMSDCL:-vmsdcl}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT INT TERM

# A private, RELOCATED SYS$SYSTEM holding a marker utility image. The marker is
# unique to this file, and echoes its own activation path ($0) so a human can
# see WHERE it came from.
RELOC="$WORK/relocated_sysexe"
mkdir -p "$RELOC"
cat > "$RELOC/SYSGEN.EXE" <<'EOF'
#!/bin/sh
echo "OVMX-VMS7D8-UTILITY-VIA-SYS\$SYSTEM activated-from=$0"
exit 0
EOF
chmod +x "$RELOC/SYSGEN.EXE"

# Prune PATH so the ONLY way a "SYSGEN.EXE" can be found is through the
# SYS$SYSTEM translation under test -- a bare-name execvp() PATH fallback (which
# could otherwise reach the build tree's real SYSGEN.EXE) is denied. $VMSDCL is
# invoked by absolute path, so pruning PATH does not affect launching DCL.
printf 'DEFINE SYS$SYSTEM "%s"\nSYSGEN\nEXIT\n' "$RELOC" \
    | env PATH="/usr/bin:/bin" "$VMSDCL" 2>&1
