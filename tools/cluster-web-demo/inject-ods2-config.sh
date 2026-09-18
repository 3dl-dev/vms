#!/bin/sh
# inject-ods2-config.sh - offline ODS-2 SYSTEM-DISK cluster-config injector for
# the OVMX/x86 browser-demo (rd vms-f0f, CORRECTED to ODS-2-resident).
#
# WHY THIS EXISTS (root cause, cited file:line against origin/main):
#   The OVMX/x86 boot reads the HIGHEST version of SYS$SYSTEM:OVMXVMSSYS.PAR off
#   the GENUINE Files-11 (ODS-2) system disk (sysdisk = VDA0:) over the executive
#   ACP (src/libvms/include/sysgen_params.h:293). PID 1 ACP-stages that DISK copy
#   into OVMX_BOOT_STAGE_DIR ("/run/ovmx-boot") BEFORE read_boot_parameters()
#   (src/ovmx_init/ovmx_init.c:~1046-1049), and read_boot_parameters() then points
#   OVMX_SYSGEN_PATH at that staged copy (ovmx_init.c:1147). So the .PAR that
#   inject-cluster-config.sh pre-places in the initramfs at
#   /run/ovmx-boot/OVMXVMSSYS.PAR is CLOBBERED by the disk's stock copy -- proven
#   by the guest console ("node name OVMX set from SYS$SYSTEM:OVMXVMSSYS.PAR" =
#   stock). The VAXCLUSTER=2 config must therefore be written ONTO THE ODS-2 DISK.
#
#   CLUSTER_AUTHORIZE.DAT is UNAFFECTED: cluster_authorize_read() does a plain
#   fopen("/etc/ovmx/cluster_authorize.dat") (cluster_authorize.h) -- it is read
#   from the initramfs, never re-staged from the disk. inject-cluster-config.sh
#   still owns that file; only the .PAR moves here, onto the disk.
#
# REUSE (HARD GUARDRAIL 1 -- no parallel ODS-2 writer):
#   The actual ODS-2 placement is done by ods2_inject_sysgen (this directory),
#   a THIN driver over the project's OWN genuine-ODS-2 codec (src/vmsfs/ods2),
#   using ods2_wvolume_open_bdev + create_file_raw + dir_insert -- the EXACT
#   primitives tools/vmsfs_master.c's emit_tree_ods2() masters the distribution
#   image with. Read-back uses the project's OWN block-backed ODS-2 reader
#   (ods2_bdev_*). qcow2<->raw is qemu-img (the same tool openvmx-site's
#   track-release.yml uses to produce sysdisk.qcow2).
#
# CONFIG BYTES (HARD GUARDRAIL 2 -- same arch-agnostic vms-8b38 artifact):
#   The OVMXVMSSYS.PAR bytes come from mk_democonfig.py `sysgen` (VAXCLUSTER=2,
#   SCSNODE per node). No new config path, nothing VAX-specific.
#
# APPROACH: OPTION A (in-place). ods2_wvolume_open_bdev() reattaches the
#   project's writer to the EXISTING shipped sysdisk and adds a higher version
#   of SYS$SYSTEM:OVMXVMSSYS.PAR -- no full source tree, no re-master. Option B
#   (re-master a system tree) is impossible offline: vmsfs_master's ODS-2 mode
#   has no `extract`, and the demo ships only the finished sysdisk, not the
#   /system-stage/vms tree a re-master needs.
#
# DOES NOT BOOT AN EMULATOR. Pure offline byte placement + read-back proof.
# Standard host tools only (python3, cc, qemu-img); NO host installs -- all
# build output + scratch images land under a /tmp workdir.

set -eu

PROG=$(basename "$0")
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
MK_DEMOCONFIG="$HERE/mk_democonfig.py"
INJECTOR_SRC="$HERE/ods2_inject_sysgen.c"
ODS2_DIR="$REPO/src/vmsfs/ods2"
ODS2_INC="$REPO/src/vmsfs/include"

usage() {
    cat >&2 <<EOF
usage: $PROG <in-sysdisk> <out-sysdisk> \\
             --scsnode NAME --scssystemid N \\
             [--votes N] [--expected-votes N] [--alloclass N] [--vaxcluster N]

Writes VAXCLUSTER OVMXVMSSYS.PAR onto the ODS-2 SYSTEM DISK (SYS\$SYSTEM, as a
new higher file version), so the booted OVMX/x86 node comes up cluster-
configured. <in-sysdisk> / <out-sysdisk> may be qcow2 or raw (auto-detected via
qemu-img). Reuses tools/cluster-web-demo/mk_democonfig.py for the config bytes
and the project's own src/vmsfs/ods2 codec for the ODS-2 write + read-back.

CLUSTER_AUTHORIZE.DAT is NOT handled here (it stays in the initramfs, injected
by inject-cluster-config.sh); '--group' therefore does not apply.

Defaults: --votes 1 --expected-votes 2 --alloclass 0 --vaxcluster 2
EOF
    exit 2
}

[ $# -ge 2 ] || usage
IN=$1; OUT=$2; shift 2

SCSNODE=""; SCSSYSTEMID=""
VOTES=1; EXPVOTES=2; ALLOCLASS=0; VAXCLUSTER=2

while [ $# -gt 0 ]; do
    case "$1" in
        --scsnode)        SCSNODE=$2; shift 2 ;;
        --scssystemid)    SCSSYSTEMID=$2; shift 2 ;;
        --votes)          VOTES=$2; shift 2 ;;
        --expected-votes) EXPVOTES=$2; shift 2 ;;
        --alloclass)      ALLOCLASS=$2; shift 2 ;;
        --vaxcluster)     VAXCLUSTER=$2; shift 2 ;;
        -h|--help)        usage ;;
        *) echo "$PROG: unknown argument: $1" >&2; usage ;;
    esac
done

[ -n "$SCSNODE" ] || { echo "$PROG: --scsnode is required" >&2; usage; }
[ -n "$SCSSYSTEMID" ] || { echo "$PROG: --scssystemid is required" >&2; usage; }
[ -f "$IN" ] || { echo "$PROG: input sysdisk not found: $IN" >&2; exit 1; }
[ -f "$MK_DEMOCONFIG" ] || { echo "$PROG: mk_democonfig.py not found" >&2; exit 1; }
[ -f "$INJECTOR_SRC" ] || { echo "$PROG: ods2_inject_sysgen.c not found" >&2; exit 1; }
[ -d "$ODS2_DIR" ] || { echo "$PROG: ODS-2 codec not found at $ODS2_DIR" >&2; exit 1; }

command -v qemu-img >/dev/null 2>&1 || { echo "$PROG: qemu-img not on PATH" >&2; exit 1; }
CC=${CC:-cc}
command -v "$CC" >/dev/null 2>&1 || { echo "$PROG: C compiler ($CC) not on PATH" >&2; exit 1; }
PYTHON=${PYTHON:-python3}

IN_ABS=$(cd "$(dirname "$IN")" && pwd)/$(basename "$IN")
OUT_DIR_ABS=$(cd "$(dirname "$OUT")" && pwd)
OUT_ABS="$OUT_DIR_ABS/$(basename "$OUT")"

WORK=$(mktemp -d "${TMPDIR:-/tmp}/inject-ods2-config.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- (1) build the injector from the project's OWN ods2 codec sources ---------
echo "== $PROG: building ods2_inject_sysgen from src/vmsfs/ods2 (project codec) =="
ODS2_SRCS="$ODS2_DIR/ods2_reader.c $ODS2_DIR/ods2_writer.c $ODS2_DIR/ods2_edit.c \
$ODS2_DIR/ods2_bdev.c $ODS2_DIR/ods2_path.c $ODS2_DIR/ods2_block_posix.c"
# shellcheck disable=SC2086
"$CC" -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 -Wall -Wextra \
    -I "$ODS2_INC" "$INJECTOR_SRC" $ODS2_SRCS -o "$WORK/ods2_inject_sysgen"
INJ="$WORK/ods2_inject_sysgen"

# --- (2) author the config bytes (vms-8b38 artifact, mk_democonfig sysgen) -----
echo "== $PROG: authoring OVMXVMSSYS.PAR via mk_democonfig.py sysgen (vms-8b38) =="
"$PYTHON" "$MK_DEMOCONFIG" sysgen \
    "$WORK/OVMXVMSSYS.PAR" "$SCSNODE" "$SCSSYSTEMID" \
    --votes "$VOTES" --expected-votes "$EXPVOTES" \
    --alloclass "$ALLOCLASS" --vaxcluster "$VAXCLUSTER"

# --- (3) qcow2/raw -> raw working copy (qemu-img auto-detects input format) ----
echo "== $PROG: converting input sysdisk -> raw working image (qemu-img) =="
qemu-img convert -O raw "$IN_ABS" "$WORK/sysdisk.raw"

# --- (4) inject onto the ODS-2 disk in place (Option A, project writer) --------
echo "== $PROG: injecting SYS\$SYSTEM:OVMXVMSSYS.PAR onto the ODS-2 disk =="
"$INJ" inject "$WORK/sysdisk.raw" "$WORK/OVMXVMSSYS.PAR"

# --- (5) raw -> output sysdisk.qcow2 (same as track-release.yml) ---------------
echo "== $PROG: converting injected raw -> $OUT_ABS (qcow2) =="
qemu-img convert -O qcow2 "$WORK/sysdisk.raw" "$OUT_ABS"

# --- (6) READ-BACK PROOF from the OUTPUT artifact (project reader + parser) ----
echo "== $PROG: read-back verify from the OUTPUT sysdisk (project's ODS-2 reader) =="
qemu-img convert -O raw "$OUT_ABS" "$WORK/verify.raw"
"$INJ" readback "$WORK/verify.raw" "$WORK/readback.par"
"$PYTHON" - "$HERE" "$WORK/readback.par" "$SCSNODE" "$VAXCLUSTER" <<'PY'
import sys
here, parfile, want_scsnode, want_vax = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
sys.path.insert(0, here)
import mk_democonfig as m
d = m.parse_sysgen_store(open(parfile, "rb").read())
p = d["params"]
assert d["magic"] == m.SYSGEN_MAGIC == 0x53595347, "magic=%#x" % d["magic"]
assert p["VAXCLUSTER"]["current"] == want_vax, "VAXCLUSTER=%r" % p["VAXCLUSTER"]["current"]
assert p["SCSNODE"]["str_current"] == want_scsnode, "SCSNODE=%r" % p["SCSNODE"]["str_current"]
print("   magic      = 0x%08X (SYSGEN_MAGIC 'SYSG')" % d["magic"])
print("   VAXCLUSTER = %d" % p["VAXCLUSTER"]["current"])
print("   SCSNODE    = %r" % p["SCSNODE"]["str_current"])
print("   SCSSYSTEMID= %d" % p["SCSSYSTEMID"]["current"])
print("PASS: OUTPUT sysdisk carries the injected config on the ODS-2 volume")
PY

echo "== $PROG: done -> $OUT_ABS =="
echo "   SCSNODE=$SCSNODE SCSSYSTEMID=$SCSSYSTEMID VOTES=$VOTES EXPECTED_VOTES=$EXPVOTES VAXCLUSTER=$VAXCLUSTER"
