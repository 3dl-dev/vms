#!/bin/sh
# mk_loginout.sh — build recipe for LOGINOUT.EXE, the OVMX login program, as a
# VMS-native EXECUTABLE image (vms-c39, pillar vms-ade). Modeled directly on
# mk_dcl.sh (vms-b65.6): LOGINOUT is the OTHER real EXECUTABLE consumer of the
# six-library VMS-native shareable graph, and until this script existed there
# was NO VMS-native (LINK.EXE) build path for it at all -- src/vmslink held
# only mk_dcl.sh, and run_dcl_native.sh never mentions LOGINOUT. vms-b65.6's
# "login chain" framing was therefore aspirational: DCL.EXE proved the graph
# for ONE executable consumer, not for the chain STARTUP -> LOGINOUT -> DCL a
# real boot runs. Baron's ruling 2026-08-06: the 0.1 dynamic-linking proof must
# cover the FULL login chain, not just DCL.EXE.
#
# LOGINOUT.EXE is tools/vms_login.c plus two login-local TUs it calls --
# loginout_display.c (the OpenVMS-faithful login-info block) and mail_notify.c
# (the shared new-mail counter), both linked in below (vms-417). Unlike DCL it
# has no vmsdcl/vmsqueue split, just the login program: prompt Username:/Password:,
# authenticate against SYSUAF, stamp the identity into the executive
# (vms_kif_setident), name the session (vms_kif_setprn), drop Linux
# credentials to the SYSUAF UIC, and execl() DCL.EXE with --login. It is an
# ET_DYN EXECUTABLE (PT_INTERP=IMGACT.EXE, entry via the C runtime), same
# shape as DCL.EXE:
#   (a) has NO symbol vector (it exports nothing -- it is the program, not a
#       library);
#   (b) imports its externals, bound at activation via .vms$imp, across the
#       SAME seven --use producers DCL.EXE links against:
#         - libc/POSIX CALL + DATA imports (malloc/str*/printf/the stdio FILE
#           ops, termios/tcgetattr/tcsetattr/isatty, setuid/setgid/setgroups,
#           getpid/getuid/geteuid/getgid/getegid, strerror, localtime,
#           setenv/getenv, chdir, stdin/stdout/stderr)     -> DECC$SHR;
#         - sysuaf_lookup/sysuaf_authenticate, ovmx_accounting_get_lastlogin/
#           record_login, str_trim/str_upcase (all rtl/* in libvms)
#                                                           -> LIBVMS$SHR;
#         - vms_pcb_init/set_identity/set_default_dir      -> LIBVMSPROCESS$SHR;
#         - vmsfs_device_add/to_linux_path                 -> LIBVMSFS$SHR;
#         - lnm_setup_defaults/lnm_get_manager              -> LIBVMSLNM$SHR;
#         - vms_kif_setident/setprn/getjpi_self (the /dev/vms identity ioctls)
#                                                           -> LIBVMSSYS$SHR.
#       LIBVMSRMS$SHR is --use'd for graph parity with DCL.EXE (LOGINOUT does
#       not itself call an RMS entry point, but IMGACT loads the full producer
#       set transitively through LIBVMS$SHR/LIBVMSFS$SHR regardless, and a
#       LOGINOUT-specific graph that silently diverged from DCL's would be
#       exactly the kind of drift Baron's ruling is trying to close off).
#       ovmx_banner_welcome/announce and parse_privilege_string are `static
#       inline` in their headers (ovmx_banner.h / vms/privs.h) -- they compile
#       straight into vms_login.o and are NOT cross-image imports.
#   (c) IMGACT.EXE recovers argc/argv off the kernel process stack and
#       activates it, exactly as it does for DCL.EXE.
#
# vms_kif_setident IS THE GAP THIS SCRIPT DOES NOT PAPER OVER. The inline
# LIBVMSSYS$SHR recipe run_dcl_native.sh carries (DCL never calls
# vms_kif_setident) does not export it -- LOGINOUT would link against a
# LIBVMSSYS$SHR.EXE missing the one universal its identity-establishment path
# needs, and IMGACT would refuse to activate it (undefined import at
# activation, not a silent no-op -- CLAUDE.md Rule 9's "fail honestly", not
# invented here, just correctly triggered). The sibling harness
# (src/imgact/test/run_login_native.sh) builds LIBVMSSYS$SHR with
# vms_kif_setident ADDED to the vector; that is a harness-side symbol-vector
# fix, not a change to link.c/imgact.c (still out of the Systems-Engineer
# file-domain).
#
# Compiled -fPIC musl with the same proven lib-shareable flags mk_dcl.sh uses:
#   -fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics
#   -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE
#
# Usage:  mk_loginout.sh <LINK.EXE> <out-LOGINOUT.EXE> \
#             <DECC$SHR.EXE> <LIBVMS$SHR.EXE> <LIBVMSPROCESS$SHR.EXE> \
#             <LIBVMSFS$SHR.EXE> <LIBVMSLNM$SHR.EXE> <LIBVMSRMS$SHR.EXE> \
#             <LIBVMSSYS$SHR.EXE> [vms-login-src-file] [repo-src-dir]
# Env:    CC (default gcc), CFLAGS (env-overridable, default aarch64 musl
#         flags; the x86_64 caller sets CFLAGS with -mtls-dialect=gnu2 in
#         place of -mno-outline-atomics -- same convention every other
#         mk_*_shr.sh took on since vms-cb5f; this one hadn't yet, vms-6da)
# Must run in the target-arch musl container where the producer .EXE already
# exist (mirrors mk_dcl.sh's requirement). LINK.EXE's x86_64 backend landed
# vms-bdf/vms-cb5f/vms-a66; x86_64 is wired the same way as aarch64 now.
set -e

LINK_EXE=${1:?usage: mk_loginout.sh <LINK.EXE> <out> <DECC\$SHR> <LIBVMS\$SHR> <LIBVMSPROCESS\$SHR> <LIBVMSFS\$SHR> <LIBVMSLNM\$SHR> <LIBVMSRMS\$SHR> <LIBVMSSYS\$SHR> [vms_login.c] [repo-src]}
OUT=${2:?need output LOGINOUT.EXE path}
DECC_SHR=${3:?need DECC\$SHR.EXE}
VMS_SHR=${4:?need LIBVMS\$SHR.EXE}
PROC_SHR=${5:?need LIBVMSPROCESS\$SHR.EXE}
FS_SHR=${6:?need LIBVMSFS\$SHR.EXE}
LNM_SHR=${7:?need LIBVMSLNM\$SHR.EXE}
RMS_SHR=${8:?need LIBVMSRMS\$SHR.EXE}
SYS_SHR=${9:?need LIBVMSSYS\$SHR.EXE (vms_kif_setident/setprn/getjpi_self producer)}
HERE=$(cd "$(dirname "$0")" && pwd)                          # src/vmslink
REPO_SRC=${11:-$(cd "$HERE/.." && pwd)}                       # src
LOGIN_SRC=${10:-$(cd "$HERE/../.." && pwd)/tools/vms_login.c} # tools/vms_login.c
TOOLS_DIR=$(cd "$(dirname "$LOGIN_SRC")" && pwd)             # tools/
CC=${CC:-gcc}

# LOGINOUT is no longer ONE translation unit (vms-417): vms_login.c now calls
# loginout_display_session_info() (the OpenVMS-faithful login-info block) and
# mail_count_unread() (the shared new-mail counter). Both live in their own
# tools/ TUs and are LINKED INTO LOGINOUT.EXE -- they are executable-local, not
# cross-image shareable symbols, so they compile here beside vms_login.o rather
# than being --use'd from a producer image. The libc/RTL calls they add
# (localtime_r, strtok, atoi, ...) are already in DECC$SHR; the VMS-RTL calls
# they make (str_upcase/str_trim -> LIBVMS$SHR, vmsfs_to_linux_path ->
# LIBVMSFS$SHR) resolve from the producers already --use'd below.
LOGINOUT_EXTRA_SRCS="$TOOLS_DIR/loginout_display.c $TOOLS_DIR/mail_notify.c"
for f in $LOGINOUT_EXTRA_SRCS; do
    [ -f "$f" ] || { echo "mk_loginout: required TU not found: $f"; exit 1; }
done

for f in "$DECC_SHR" "$VMS_SHR" "$PROC_SHR" "$FS_SHR" "$LNM_SHR" "$RMS_SHR" "$SYS_SHR"; do
    [ -f "$f" ] || { echo "mk_loginout: producer image not found: $f"; exit 1; }
done
[ -f "$LOGIN_SRC" ] || { echo "mk_loginout: vms_login.c not found: $LOGIN_SRC"; exit 1; }

WORK=${WORK:-/tmp/mk-loginout}
mkdir -p "$WORK"

CFLAGS="${CFLAGS:--fPIC -O2 -ffreestanding -fno-builtin -fno-stack-protector -mno-outline-atomics -U_FORTIFY_SOURCE}"
DEFS="-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
INCS="-I$TOOLS_DIR -I$REPO_SRC/libvms/include -I$REPO_SRC/vmsfs/include \
-I$REPO_SRC/vmslnm/include -I$REPO_SRC/vmsrms/include \
-I$REPO_SRC/vmsprocess/include -I$REPO_SRC/libvmssys"

echo "mk_loginout: LINK.EXE=$LINK_EXE  CC=$CC"
echo "mk_loginout: --use DECC\$SHR LIBVMS\$SHR LIBVMSPROCESS\$SHR LIBVMSFS\$SHR LIBVMSLNM\$SHR LIBVMSRMS\$SHR LIBVMSSYS\$SHR"

echo "  cc $(basename "$LOGIN_SRC")"
$CC $CFLAGS $DEFS $INCS -c -o "$WORK/vms_login.o" "$LOGIN_SRC"

LOGINOUT_OBJS="$WORK/vms_login.o"
for src in $LOGINOUT_EXTRA_SRCS; do
    obj="$WORK/$(basename "${src%.c}").o"
    echo "  cc $(basename "$src")"
    $CC $CFLAGS $DEFS $INCS -c -o "$obj" "$src"
    LOGINOUT_OBJS="$LOGINOUT_OBJS $obj"
done

echo "mk_loginout: LINK.EXE --executable --use {7 producers} -> $OUT"
"$LINK_EXE" --executable \
    --use "$DECC_SHR" --use "$VMS_SHR" --use "$PROC_SHR" \
    --use "$FS_SHR" --use "$LNM_SHR" --use "$RMS_SHR" --use "$SYS_SHR" \
    -o "$OUT" $LOGINOUT_OBJS

echo "mk_loginout: created $OUT"
