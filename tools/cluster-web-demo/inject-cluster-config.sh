#!/bin/sh
# inject-cluster-config.sh - offline cluster-config injector for the OVMX/x86
# browser-demo boot artifacts (rd vms-f0f, the Node-A injection step).
#
# Turns a stock demo initramfs (host-mode /vms, no cluster config) into a
# CLUSTER-CONFIGURED Node A: after boot, load_cluster_sysgen_params() reads a
# real VAXCLUSTER=2 OVMXVMSSYS.PAR + a CLUSTER_AUTHORIZE.DAT, hands them to the
# executive (VMS_IOCTL_SYSGEN_LOAD), and PEDRIVER starts and emits 0x6007 SCS
# HELLOs. WITHOUT booting an emulator: this is the pure offline injection +
# byte placement; booting is a separate step.
#
# It does NOT author config bytes itself. It REUSES the merged, tested config
# tool tools/cluster-web-demo/mk_democonfig.py (rd vms-8b38) for both artifacts:
#   - OVMXVMSSYS.PAR       via `mk_democonfig.py sysgen`
#   - CLUSTER_AUTHORIZE.DAT via `mk_democonfig.py authorize`  (uses the runtime's
#                           own C writer cluster_authorize_write() when cc exists)
#
# GROUND TRUTH -- how the OVMX/x86 boot consumes cluster config OFFLINE
# (cited file:line against origin/main as of #1280):
#
#  CLUSTER_AUTHORIZE.DAT:
#    src/libvms/include/cluster_authorize.h:cluster_authorize_read() does a plain
#    fopen(cluster_authorize_path()); cluster_authorize_path() returns
#    $OVMX_CLUSTER_AUTH_PATH or, unset, CLUSTER_AUTH_DEFAULT_PATH
#    "/etc/ovmx/cluster_authorize.dat". No ODS-2, no env needed -- drop the file
#    at /etc/ovmx/cluster_authorize.dat inside the initramfs and it is read
#    directly. (The stock demo initramfs already ships an empty /etc/ovmx/.)
#    This is exactly how distro/Dockerfile.bootable stages it (build-arg
#    CLUSTER_AUTH_GROUP -> /initramfs-slim/etc/ovmx/cluster_authorize.dat).
#
#  OVMXVMSSYS.PAR (the OVMX_SYSGEN_PATH mechanism):
#    src/libvms/include/sysgen_params.h:sysgen_load_current_db() -- the shared
#    reader behind sysgen_read_string()/sysgen_read_param() -- checks
#    getenv("OVMX_SYSGEN_PATH") FIRST; if set+non-empty it fopen()s that literal
#    file and validates SYSGEN_MAGIC(0x53595347)/version 2. Otherwise it reads
#    the ODS-2 system volume over the executive ACP (needs a mounted ODS-2
#    volume -- unavailable offline).
#    The x86 PID 1 (src/ovmx_init/ovmx_init.c) is itself the /init ELF -- it is
#    NOT a shell wrapper, so /init CANNOT be edited to `export OVMX_SYSGEN_PATH`.
#    Instead the boot code sets that env itself: read_boot_parameters() (line
#    ~1147) and load_cluster_sysgen_params() (line ~1331), both compiled with
#    OVMX_BOOT_ACP_BRIDGE (confirmed by the /run/ovmx-boot string in the shipped
#    /init), do:
#        staged = (access(OVMX_BOOT_STAGE_DIR "/OVMXVMSSYS.PAR", R_OK) == 0);
#        if (staged) setenv("OVMX_SYSGEN_PATH", OVMX_BOOT_STAGE_DIR "/OVMXVMSSYS.PAR", 1);
#    with OVMX_BOOT_STAGE_DIR == "/run/ovmx-boot" (ovmx_layout.h:252). On Linux
#    the initramfs IS the tmpfs root and ovmx_boot_prepare_stage_dir()
#    (ovmx_boot_linux.c:289) only mkdir()s /run and /run/ovmx-boot (EEXIST-
#    tolerant, never clobbers a file). So a file PRE-PLACED in the initramfs at
#    /run/ovmx-boot/OVMXVMSSYS.PAR is found by that access() check and becomes
#    OVMX_SYSGEN_PATH -- the guaranteed offline lever.
#
#    We ALSO place the identical bytes at the canonical host-mode home
#    /vms/SYS0/SYSCOMMON/SYSEXE/OVMXVMSSYS.PAR (where distro/Dockerfile.bootable
#    stages it on the system disk, and where stage_boot_images() reads it from
#    to stage into /run/ovmx-boot). Belt-and-suspenders: identical content, so
#    whichever reader/stager the demo build exercises resolves the same config.
#
# Standard tools only (python3, cpio, gzip, find, cc for the C caut writer);
# NO host installs.

set -eu

PROG=$(basename "$0")
HERE=$(cd "$(dirname "$0")" && pwd)
MK_DEMOCONFIG="$HERE/mk_democonfig.py"

usage() {
    cat >&2 <<EOF
usage: $PROG <in-initramfs.cpio.gz> <out-initramfs.cpio.gz> \\
             --scsnode NAME --scssystemid N \\
             [--votes N] [--expected-votes N] [--group N] \\
             [--password PW] [--alloclass N] [--vaxcluster N]

Injects a VAXCLUSTER OVMXVMSSYS.PAR + CLUSTER_AUTHORIZE.DAT into the demo
initramfs so the booted node comes up cluster-configured (PEDRIVER + 0x6007
SCS HELLOs). Reuses tools/cluster-web-demo/mk_democonfig.py for the bytes.

Defaults: --votes 1 --expected-votes 1 --group 0 --alloclass 0 --vaxcluster 2
EOF
    exit 2
}

[ $# -ge 2 ] || usage
IN=$1; OUT=$2; shift 2

SCSNODE=""; SCSSYSTEMID=""
VOTES=1; EXPVOTES=1; GROUP=0; PASSWORD=""; ALLOCLASS=0; VAXCLUSTER=2

while [ $# -gt 0 ]; do
    case "$1" in
        --scsnode)        SCSNODE=$2; shift 2 ;;
        --scssystemid)    SCSSYSTEMID=$2; shift 2 ;;
        --votes)          VOTES=$2; shift 2 ;;
        --expected-votes) EXPVOTES=$2; shift 2 ;;
        --group)          GROUP=$2; shift 2 ;;
        --password)       PASSWORD=$2; shift 2 ;;
        --alloclass)      ALLOCLASS=$2; shift 2 ;;
        --vaxcluster)     VAXCLUSTER=$2; shift 2 ;;
        -h|--help)        usage ;;
        *) echo "$PROG: unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "$SCSNODE" ] || { echo "$PROG: --scsnode is required" >&2; usage; }
[ -n "$SCSSYSTEMID" ] || { echo "$PROG: --scssystemid is required" >&2; usage; }
[ -f "$IN" ] || { echo "$PROG: input initramfs not found: $IN" >&2; exit 1; }
[ -f "$MK_DEMOCONFIG" ] || { echo "$PROG: mk_democonfig.py not found at $MK_DEMOCONFIG" >&2; exit 1; }

PYTHON=${PYTHON:-python3}
IN_ABS=$(cd "$(dirname "$IN")" && pwd)/$(basename "$IN")
OUT_DIR=$(cd "$(dirname "$OUT")" && pwd)
OUT_ABS="$OUT_DIR/$(basename "$OUT")"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/inject-cluster-config.XXXXXX")
trap 'rm -rf "$WORK"' EXIT
CONF="$WORK/conf"
ROOT="$WORK/root"
mkdir -p "$CONF" "$ROOT"

echo "== $PROG: authoring config via mk_democonfig.py (rd vms-8b38) =="
# (1) OVMXVMSSYS.PAR -- the SYSGEN store load_cluster_sysgen_params() reads.
"$PYTHON" "$MK_DEMOCONFIG" sysgen \
    "$CONF/OVMXVMSSYS.PAR" "$SCSNODE" "$SCSSYSTEMID" \
    --votes "$VOTES" --expected-votes "$EXPVOTES" \
    --alloclass "$ALLOCLASS" --vaxcluster "$VAXCLUSTER"
# (2) CLUSTER_AUTHORIZE.DAT -- the group/password record cluster_authorize_read() reads.
"$PYTHON" "$MK_DEMOCONFIG" authorize \
    "$CONF/CLUSTER_AUTHORIZE.DAT" "$GROUP" "$PASSWORD" --writer auto

echo "== $PROG: unpacking input initramfs =="
( cd "$ROOT" && gunzip -c "$IN_ABS" | cpio -idm --quiet )

echo "== $PROG: placing config into the initramfs tree =="
# (3) CLUSTER_AUTHORIZE.DAT -> /etc/ovmx/cluster_authorize.dat (cluster_authorize.h default path).
mkdir -p "$ROOT/etc/ovmx"
cp "$CONF/CLUSTER_AUTHORIZE.DAT" "$ROOT/etc/ovmx/cluster_authorize.dat"

# (4) OVMXVMSSYS.PAR -> the guaranteed OVMX_SYSGEN_PATH lever, and the canonical
#     host-mode SYS$SYSTEM home (identical bytes at both).
mkdir -p "$ROOT/run/ovmx-boot"
cp "$CONF/OVMXVMSSYS.PAR" "$ROOT/run/ovmx-boot/OVMXVMSSYS.PAR"
mkdir -p "$ROOT/vms/SYS0/SYSCOMMON/SYSEXE"
cp "$CONF/OVMXVMSSYS.PAR" "$ROOT/vms/SYS0/SYSCOMMON/SYSEXE/OVMXVMSSYS.PAR"

echo "== $PROG: repacking initramfs (newc + gzip, same format as input) =="
# Deterministic pack, matching distro/Dockerfile.bootable's boot-initramfs step:
# normalize mtimes, sort member order, --reproducible, gzip -n (no embedded mtime).
SDE=${SOURCE_DATE_EPOCH:-0}
find "$ROOT" -exec touch -h -d "@${SDE}" {} +
( cd "$ROOT" && find . | LC_ALL=C sort | cpio -o -H newc --reproducible --quiet | gzip -n ) > "$OUT_ABS"

echo "== $PROG: done -> $OUT_ABS =="
echo "   SCSNODE=$SCSNODE SCSSYSTEMID=$SCSSYSTEMID VOTES=$VOTES EXPECTED_VOTES=$EXPVOTES VAXCLUSTER=$VAXCLUSTER GROUP=$GROUP"
