#!/bin/sh
# inject-decnet-config.sh - give a demo node's ODS-2 system disk its DECnet
# Phase IV permanent databases, so boot-time STARTNET starts NETACP and a
# visitor can SET HOST between the OVMX nodes (rd vms-5843).
#
# WHAT A VMS SITE DOES: run NCP once -- SET EXECUTOR ADDRESS/NAME/STATE ON and
# DEFINE NODE for its peers -- and reboot; @SYS$MANAGER:STARTNET finds
# SYS$SYSTEM:NETNODE_LOCAL.DAT and starts the network. The browser demo cannot
# boot each node once to type that, so this does the same thing offline:
#
#   1. the COMMANDS come from the roster SSOT (mk_democonfig.py decnet-ncp
#      <SCSNODE>): this node's address and name, and every other DECnet node;
#   2. NCP.EXE ITSELF -- built from this tree, through its documented host hook
#      (OVMX_DECNET_EXECUTOR/_NODEDB/_OBJECTDB, src/vmsdecnet/ncp/include/
#      dnet_ncpstore.h) -- runs them and writes NETNODE_LOCAL.DAT and
#      NETNODE_REMOTE.DAT. Nothing here knows the record layout; a second
#      writer of it would be a second source of truth;
#   3. ods2_inject_sysgen puttext adds them to SYS$SYSTEM on the genuine ODS-2
#      volume as text files (RFM=STMLF, how vmsfs_master masters a .DAT), and
#      gettext reads them back and they must match byte for byte.
#
# usage: inject-decnet-config.sh <in-sysdisk> <out-sysdisk> --scsnode NAME [--ncp NCP.EXE]
#   <in>/<out> may be qcow2 or raw (qemu-img auto-detects). --ncp names an
#   already-built host NCP.EXE; without it one is built from this tree.
# Exit 0 = injected and read back. Exit 3 = the roster gives this node no
# DECnet address (nothing written -- honest, not an error to paper over).

set -eu

PROG=$(basename "$0")
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
PYTHON=${PYTHON:-python3}
CC=${CC:-cc}

[ $# -ge 2 ] || { echo "usage: $PROG <in-sysdisk> <out-sysdisk> --scsnode NAME [--ncp NCP.EXE]" >&2; exit 2; }
IN=$1; OUT=$2; shift 2
SCSNODE=""; NCP=""
while [ $# -gt 0 ]; do
    case "$1" in
        --scsnode) SCSNODE=$2; shift 2;;
        --ncp)     NCP=$2; shift 2;;
        *) echo "$PROG: unknown argument $1" >&2; exit 2;;
    esac
done
[ -n "$SCSNODE" ] || { echo "$PROG: --scsnode is required" >&2; exit 2; }

WORK=$(mktemp -d "${TMPDIR:-/tmp}/inject-decnet-config.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

# --- (1) the commands, from the roster -----------------------------------------
if ! "$PYTHON" "$HERE/mk_democonfig.py" decnet-ncp "$SCSNODE" > "$WORK/ncp.cmds" 2> "$WORK/ncp.err"; then
    if grep -q "has no DECnet address" "$WORK/ncp.err"; then
        echo "== $PROG: roster node $SCSNODE has no DECnet address -- nothing to inject"
        exit 3
    fi
    cat "$WORK/ncp.err" >&2
    exit 1
fi

# --- (2) NCP.EXE writes the databases --------------------------------------------
if [ -z "$NCP" ]; then
    echo "== $PROG: building NCP.EXE from this tree (cmake target ncp_exe) =="
    cmake -S "$REPO" -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release \
        -DBUILD_TESTS=OFF -DBUILD_TOOLS=ON >"$WORK/cmake.log" 2>&1 \
        || { tail -20 "$WORK/cmake.log" >&2; exit 1; }
    cmake --build "$WORK/build" --target ncp_exe -j"$(nproc 2>/dev/null || echo 2)" \
        >"$WORK/build.log" 2>&1 || { tail -30 "$WORK/build.log" >&2; exit 1; }
    NCP="$WORK/build/bin/NCP.EXE"
fi
[ -x "$NCP" ] || { echo "$PROG: no NCP.EXE at $NCP" >&2; exit 1; }
mkdir -p "$WORK/db"
export OVMX_DECNET_EXECUTOR="$WORK/db/NETNODE_LOCAL.DAT"
export OVMX_DECNET_NODEDB="$WORK/db/NETNODE_REMOTE.DAT"
export OVMX_DECNET_OBJECTDB="$WORK/db/NETOBJECT.DAT"
echo "== $PROG: NCP configures $SCSNODE (commands from mk_democonfig.py decnet-ncp) =="
while IFS= read -r cmd; do
    [ -n "$cmd" ] || continue
    echo "   NCP> $cmd"
    # shellcheck disable=SC2086
    "$NCP" $cmd > "$WORK/ncp.out" 2>&1 || { cat "$WORK/ncp.out" >&2; exit 1; }
    if grep -q '%NCP-[EF]-' "$WORK/ncp.out"; then cat "$WORK/ncp.out" >&2; exit 1; fi
done < "$WORK/ncp.cmds"
for f in NETNODE_LOCAL.DAT NETNODE_REMOTE.DAT; do
    [ -s "$WORK/db/$f" ] || { echo "$PROG: NCP wrote no $f" >&2; exit 1; }
done
"$NCP" LIST EXECUTOR | tee "$WORK/list.txt"
"$NCP" LIST KNOWN NODES | tee -a "$WORK/list.txt"

# --- (3) onto the ODS-2 volume, then read back ------------------------------------
echo "== $PROG: building ods2_inject_sysgen from src/vmsfs/ods2 =="
ODS2_DIR="$REPO/src/vmsfs/ods2"
# shellcheck disable=SC2086
"$CC" -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -O2 -Wall -Wextra \
    -I "$REPO/src/vmsfs/include" "$HERE/ods2_inject_sysgen.c" \
    "$ODS2_DIR/ods2_reader.c" "$ODS2_DIR/ods2_writer.c" "$ODS2_DIR/ods2_edit.c" \
    "$ODS2_DIR/ods2_bdev.c" "$ODS2_DIR/ods2_path.c" "$ODS2_DIR/ods2_block_posix.c" \
    -o "$WORK/inj"
qemu-img convert -O raw "$IN" "$WORK/sysdisk.raw"
for f in NETNODE_LOCAL.DAT NETNODE_REMOTE.DAT; do
    "$WORK/inj" puttext "$WORK/sysdisk.raw" "$f" "$WORK/db/$f"
done
case "$OUT" in
    *.qcow2) qemu-img convert -O qcow2 "$WORK/sysdisk.raw" "$OUT";;
    *)       cp "$WORK/sysdisk.raw" "$OUT";;
esac
qemu-img convert -O raw "$OUT" "$WORK/verify.raw"
for f in NETNODE_LOCAL.DAT NETNODE_REMOTE.DAT; do
    "$WORK/inj" gettext "$WORK/verify.raw" "$f" "$WORK/back.$f"
    cmp "$WORK/db/$f" "$WORK/back.$f" \
        || { echo "$PROG: read-back of SYS\$SYSTEM:$f differs from what NCP wrote" >&2; exit 1; }
done
echo "PASS: $SCSNODE's DECnet databases are on the ODS-2 volume, as NCP wrote them -> $OUT"
