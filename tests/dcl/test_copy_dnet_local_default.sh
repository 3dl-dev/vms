#!/bin/bash
# TEST: DCL COPY node:: hands DECNETD the LOCAL spec completed with the default directory (vms-ea8)
#
# DECNETD.EXE gives its local spec straight to RMS, which completes a missing
# device/directory from SYS$DISK's root -- not from DCL's SET DEFAULT. Found
# live on a booted OVMX: `COPY 1.1"SYSTEM pw"::TOOVMX.TXT FROMVAX.TXT` reported
# success and the file landed in VDA0:[000000] instead of the user's default
# SYS$SYSROOT:[SYSMGR]. DCL now completes the local side the way its own file
# commands do before activating DECNETD; the NODE:: side is left for the
# remote FAL. A stand-in DECNETD.EXE (this test only) prints the argv it got.
# NEGCTL: before the fix argv carried the bare "B.TXT" / "C.TXT".
#
# EXPECT: regex:FAKE-DECNETD-ARGV: --copy 1\.1"SYSTEM"::A\.TXT \S*\]\S*B\.TXT --password-fd
# EXPECT: regex:FAKE-DECNETD-ARGV: --copy \S*\]\S*C\.TXT 1\.1::D\.TXT
# EXPECT_NOT: contains:::A.TXT B.TXT
# EXPECT_NOT: contains:--copy C.TXT
# EXPECT_NOT: contains:secret
VMSDCL="${VMSDCL:-vmsdcl}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf '#!/bin/sh\necho "FAKE-DECNETD-ARGV: $*"\n' > "$T/DECNETD.EXE"
chmod +x "$T/DECNETD.EXE"
printf 'DEFINE SYS$SYSTEM "%s/"\nCOPY 1.1"SYSTEM secret"::A.TXT B.TXT\nCOPY C.TXT 1.1::D.TXT\n' "$T" | $VMSDCL 2>&1
