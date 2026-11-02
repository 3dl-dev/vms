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
# rd vms-ad71: a PASSWORD travels only in a temporary VMS mailbox DCL creates
# ($CREMBX) and names to DECNETD (--password-mbx). This host run has no
# executive, so there is no mailbox: DCL refuses honestly and activates
# nothing -- the password is never put on argv or a host fd instead.
#
# EXPECT: regex:FAKE-DECNETD-ARGV: --copy 1\.1"SYSTEM"::A\.TXT \S*\]\S*B\.TXT$
# EXPECT: regex:FAKE-DECNETD-ARGV: --copy \S*\]\S*C\.TXT 1\.1::D\.TXT
# EXPECT: contains:%COPY-F-NETPWMBX, could not create the DECnet password mailbox (status
# EXPECT_NOT: contains:::A.TXT B.TXT
# EXPECT_NOT: contains:--copy C.TXT
# EXPECT_NOT: contains:secret
# EXPECT_NOT: contains:--password-fd
# EXPECT_NOT: regex:FAKE-DECNETD-ARGV:.*E\.TXT
VMSDCL="${VMSDCL:-vmsdcl}"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
printf '#!/bin/sh\necho "FAKE-DECNETD-ARGV: $*"\n' > "$T/DECNETD.EXE"
chmod +x "$T/DECNETD.EXE"
printf 'DEFINE SYS$SYSTEM "%s/"\nCOPY 1.1"SYSTEM"::A.TXT B.TXT\nCOPY C.TXT 1.1::D.TXT\nCOPY 1.1"SYSTEM secret"::E.TXT F.TXT\n' "$T" | $VMSDCL 2>&1
