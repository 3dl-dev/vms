#!/bin/bash
# TEST: DCL apostrophe substitution on RUN'd-image SYS$INPUT data lines (vms-963)
#
# VMS behaviour (VSI OpenVMS User's Manual / DCL Concepts -- "Symbol
# Substitution"): DCL performs forced apostrophe ('symbol') substitution on the
# data lines of a command procedure, not only on '$'-command lines. When those
# data lines are the SYS$INPUT of an image invoked from the procedure (vms-1a9),
# the substitution still happens BEFORE the image reads the line. This is what
# lets the classic non-interactive idiom
#     $ RUN SYS$SYSTEM:AUTHORIZE
#     MODIFY SYSTEM/PASSWORD='NEWPW'
#     EXIT
# pass a run-time symbol (a password entered earlier in the procedure) into a
# utility's own input. OVMX$INSTALL.COM depends on exactly this to set the
# SYSTEM password on the target during install.
#
# On origin/main dcl_sysinput_setup() wrote the data lines to the image's
# SYS$INPUT VERBATIM, so 'PWSYM' reached the image literally and the password
# was never interpolated. This test is RED there and GREEN with the fix.
#
# 1) 'PWSYM' in a SYS$INPUT data line interpolates to the symbol's value:
# EXPECT: contains:SAW:MODIFY SYSTEM/PASSWORD=INTERP_SECRET_OK
# 2) A data line with no substitution marker is passed byte-for-byte (no
#    regression for substitution-free blocks -- the vms-1a9 contract):
# EXPECT: contains:SAW:PLAINVERBATIM_NOAPOS
# 3) A lone apostrophe not introducing a symbol is KEPT (VMS literal-apostrophe
#    behaviour -- the substituter does not eat it):
# EXPECT: contains:SAW:LONE ' KEPT
# 4) The raw, un-substituted marker must NOT reach the image:
# EXPECT_NOT: contains:PASSWORD='PWSYM'
# 5) Data lines were fed to the image, not executed as DCL commands:
# EXPECT_NOT: contains:IVVERB
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_sysin_apos_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

# Same echo-image as test_run_sysinput_procedure.sh: read SYS$INPUT (fd 0),
# echo each line with a distinctive "SAW:" prefix only this image emits.
SRC="/vms/$TDIR/echoin.c"
cat > "$SRC" << 'EOF'
#include <stdio.h>
#include <string.h>
int main(void) {
    char line[4096];
    while (fgets(line, sizeof(line), stdin)) {
        size_t n = strlen(line);
        if (n && line[n-1] == '\n') line[n-1] = '\0';
        printf("SAW:%s\n", line);
    }
    printf("IMGEOF\n");
    fflush(stdout);
    return 0;
}
EOF
cc -O0 -o "/vms/$TDIR/echoin.exe" "$SRC" 2>/dev/null || {
    echo "BUILD_FAILED: could not compile the SYS\$INPUT test image"
    rm -rf "/vms/$TDIR"
    exit 1
}

# The procedure defines a symbol, RUNs the image, then feeds three data lines:
#   - one carrying 'PWSYM' (must interpolate to the symbol value),
#   - one plain line (must survive verbatim),
#   - one with a lone apostrophe (must survive verbatim).
cat > "/vms/$TDIR/drive.com" << 'EOF'
$ PWSYM = "INTERP_SECRET_OK"
$ RUN ECHOIN.EXE
MODIFY SYSTEM/PASSWORD='PWSYM'
PLAINVERBATIM_NOAPOS
LONE ' KEPT
$ WRITE SYS$OUTPUT "RESUMED_AT_DOLLAR"
$ EXIT
EOF

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@drive.com\n' "$VDIR" | $VMSDCL 2>&1

rm -rf "/vms/$TDIR"
