#!/bin/bash
# TEST: RUN'd image inside a .COM reads SYS$INPUT from the procedure (vms-1a9)
#
# VMS behaviour (VSI OpenVMS User's Manual / DCL Concepts -- "Data Lines in a
# Command Procedure" / SYS$INPUT): when a command procedure invokes an image,
# the image's SYS$INPUT defaults to the procedure itself. The lines following
# the invoking command -- those that do NOT begin with '$' -- are fed to the
# image as its input, up to the next '$'-command or end-of-file. DCL then
# resumes at that first '$' line. This is how DEC install/config procedures
# drive AUTHORIZE/SYSGEN non-interactively.
#
# On origin/main the image inherits the terminal (or EOF) instead, so it never
# sees the procedure's data lines, and those data lines are (wrongly) executed
# as DCL commands -> %DCL-W-IVVERB. This test is RED there and GREEN with the
# fix.
#
# The image saw the two data lines:
# EXPECT: contains:SAW:SYSINLINE_ALPHA
# EXPECT: contains:SAW:SYSINLINE_BETA
# The image saw EOF right after the block (block terminated at the '$' line):
# EXPECT: contains:IMGEOF
# DCL resumed at the first '$' line after the input block:
# EXPECT: contains:RESUMED_AT_DOLLAR
# The '$' line was NOT fed to the image (input terminated at it, not past it):
# EXPECT_NOT: contains:SAW:$
# The data lines were NOT executed as DCL commands:
# EXPECT_NOT: contains:IVVERB
VMSDCL="${VMSDCL:-vmsdcl}"
TDIR="dcl_sysin_$$"
VDIR="$(echo "$TDIR" | tr a-z A-Z)"
mkdir -p "/vms/$TDIR"

# A tiny image that reads its SYS$INPUT (fd 0) and echoes each line with a
# distinctive prefix, then prints IMGEOF at end-of-file. The "SAW:" prefix is
# something only this image can emit -- a DCL error that happened to echo the
# same text would not carry it.
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

# The procedure: RUN the image, then two data lines (SYS$INPUT), then a
# '$'-command that must run AFTER the image consumes exactly the data block.
cat > "/vms/$TDIR/drive.com" << 'EOF'
$ RUN ECHOIN.EXE
SYSINLINE_ALPHA
SYSINLINE_BETA
$ WRITE SYS$OUTPUT "RESUMED_AT_DOLLAR"
$ EXIT
EOF

printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n@drive.com\n' "$VDIR" | $VMSDCL 2>&1

rm -rf "/vms/$TDIR"
