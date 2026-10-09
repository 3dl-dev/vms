#!/bin/bash
# TEST: a FAILED image activation says so -- never "the image exited with %X00000001" (vms-06c)
#
# DCL's fork fallback (dcl_activate_image_inner, src/vmsdcl/dcl_cmd_process.c)
# fork()s and execve()s the image. When execve() ITSELF fails -- the activator
# the image names (its PT_INTERP, OVMX's staged IMGACT.EXE) is not where the
# image names it, or the file is not executable -- the image NEVER RUNS. The
# child used to die silently with POSIX exit code 1 and DCL then reported
#
#     %DCL-E-ABORT, image <name> exited with error status %X00000001
#
# which asserts the opposite of what happened: that the image ran and returned
# 1. Nothing at all reached SYS$OUTPUT or SYS$ERROR, so there was no other
# signal either. On the ci.6 lab that mis-diagnosis hid a real PT_INTERP
# mismatch (an EVACWL.EXE linked with the /vms-flavoured activator path booted
# on a node whose activator is staged elsewhere) behind a message that said the
# program had run and failed.
#
# DCL now carries the real execve() errno out of the child over a CLOEXEC
# handshake pipe (dcl_exec_handshake_*) and reports the OpenVMS CLI's own
# activation-failure condition instead.
#
# The subject here is a file DCL will happily resolve and hand to execve(), and
# which execve() then refuses because the interpreter it names does not exist --
# exactly the booted-node failure, reproduced with no executive needed.
#
# EXPECT: contains:%DCL-W-ACTIMAGE, error activating image
# EXPECT: contains:-CLI-E-IMGNAME, image file
# EXPECT: contains:-RMS-E-FNF, file not found
# EXPECT: contains:-DCL-I-ACTIVATOR, the image is present
# EXPECT_NOT: contains:exited with error status
VMSDCL="${VMSDCL:-vmsdcl}"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# An activatable file whose named activator is absent. A DCL image spec is
# upper-cased, so the scratch directory is reached through a logical name rather
# than a literal (lower-case) POSIX path.
printf '#!%s/NO_SUCH_ACTIVATOR.EXE\n' "$WORK" > "$WORK/NOACT.EXE"
chmod +x "$WORK/NOACT.EXE"

printf 'DEFINE ACTDIR "%s/"\nRUN ACTDIR:NOACT.EXE\n' "$WORK" | $VMSDCL 2>&1
