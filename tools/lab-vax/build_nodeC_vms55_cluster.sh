#!/bin/bash
# build_nodeC_vms55_cluster.sh -- rd vms-735 (Node-C lane)
#
# Builds the Node-C cluster-member disk for the V0.7 browser cluster demo: a
# REAL OpenVMS VAX V5.5-2H4 system disk (ODS-2), configured for VMScluster
# membership the VMS way (CLUSTER_CONFIG.COM -> SYSGEN/AUTOGEN -- never
# hand-injected bytes), matching the identity frozen in the demo's single
# source of truth:
#
#   .claude/worktrees/vms-web-demo/tools/cluster-web-demo/mk_democonfig.py
#   DEMO_NODE_C = SCSNODE=VAXC, SCSSYSTEMID=1989, group=257,
#                 votes=1, expected_votes=1 (Node C is cluster GENESIS --
#                 it forms the 1-node cluster alone; A and B raise CN as
#                 they join over the shared L2. See docs/design/
#                 cluster-web-demo.md section 5/11 in that worktree.)
#
# PREREQUISITES (not fetched by this script -- source them first, see
# "SOURCING THE BASE IMAGE" below):
#   - A real, hobbyist-licensed OpenVMS VAX V5.5-2H4 RD54 system-disk image,
#     159,334,400 bytes (311200 blocks), e.g. $HOME/vms55-rd54.dsk. This is
#     the same media pcjs's browser/vax.html measures against (see
#     ~/projects/pcjs/machines/dec/vax/README.md). DEC-copyright media is
#     NEVER checked into any repo (HANDOFF.md section 8 / AGENTS.md
#     clean-room rule) -- point BASE_DSK at your own copy.
#   - A SIMH MicroVAX 3900 binary WITH console-ROM support, e.g.
#     open-simh's microvax3900 (~/projects/pcjs-vax/open-simh/BIN/).
#     NOTE: if this binary reports "network support not available in
#     simulator" (`echo 'show eth' | ./microvax3900`), you cannot verify a
#     LIVE quorum-formed boot with this script -- PEDRIVER's NISCA startup
#     needs a real (even disconnected) Ethernet device to get past
#     "waiting to form or join a VAXcluster system". The CLUSTER_CONFIG
#     configuration itself still lands correctly and is verifiable via
#     SYSBOOT> SHOW (see step 8) or by transiently booting with
#     VAXCLUSTER=0 (step 9) to reach DCL and run SHOW CLUSTER. The REAL
#     pcjs KA655 DELQA (pcjsvax-d1bf/pcjsvax-636) has a working loopback
#     path and is expected to clear quorum=1 on its own.
#   - data/training/vax/cluster/nodedrv.py -- the console driver (pty +
#     FIFO injection, echo-verified boot-command typing, ENTER DATE AND
#     TIME auto-answer). Vendor or symlink your own copy if that lab path
#     is not present on your host.
#
# SOURCING THE BASE IMAGE (clean-room note): this script does not download
# or embed VMS media. VMS V5.5 has been distributed by DEC/Compaq/HP under a
# hobbyist license for years (the same terms that license the copy this repo
# already measures pcjs against); Baron has authorized public shipment of
# this exact disk for the demo because V5.5's LMF is advisory -- clustering
# needs a group-257 CLUSTER_AUTHORIZE.DAT, not a PAK (already measured;
# do not re-litigate). Point BASE_DSK at that licensed copy.
#
# USAGE
#   BASE_DSK=$HOME/vms55-rd54.dsk \
#   SIMH_BIN=$HOME/projects/pcjs-vax/open-simh/BIN/microvax3900 \
#   NODEDRV=/data/training/vax/cluster/nodedrv.py \
#   WORKDIR=/tmp/vms-cd3-nodeC \
#   OUT=/tmp/vms55-nodeC-cluster.dsk.gz \
#     ./build_nodeC_vms55_cluster.sh
#
# OUTPUT: $OUT -- gzip -9 of the cluster-configured system disk, ready to
# drop in next to ovmx-vax-nodeB.img.gz for the pcjs cluster-node page
# (browser/ovmx-cluster.html ?diskgz=<url>).
set -euo pipefail

BASE_DSK="${BASE_DSK:-$HOME/vms55-rd54.dsk}"
SIMH_BIN="${SIMH_BIN:-$HOME/projects/pcjs-vax/open-simh/BIN/microvax3900}"
NODEDRV="${NODEDRV:-/data/training/vax/cluster/nodedrv.py}"
WORKDIR="${WORKDIR:-/tmp/vms-cd3-nodeC}"
OUT="${OUT:-/tmp/vms55-nodeC-cluster.dsk.gz}"

SYSTEM_PW="${SYSTEM_PW:-QUOKKA1953}"          # pcjs README's measured SYSTEM password
NEW_SYSTEM_PW="${NEW_SYSTEM_PW:-OVMXCLUSTER1}" # only used if the account's password has expired
SCSNODE="VAXC"
SCSSYSTEMID=1989                               # DECnet 1.965 -> area*1024+node = 1989
DECNET_ADDR="1.965"
CLUSTER_GROUP=257
CLUSTER_PW="${CLUSTER_PW:?set CLUSTER_PW -- the demo real cluster password is an operator fact, never committed}"
BOOT_DATE="${BOOT_DATE:-$(date '+%d-%b-%Y %H:%M' | tr a-z A-Z)}"

[ -f "$BASE_DSK" ]  || { echo "FATAL: BASE_DSK not found: $BASE_DSK" >&2; exit 1; }
[ -x "$SIMH_BIN" ]  || { echo "FATAL: SIMH_BIN not executable: $SIMH_BIN" >&2; exit 1; }
[ -f "$NODEDRV" ]   || { echo "FATAL: NODEDRV not found: $NODEDRV" >&2; exit 1; }

rm -rf "$WORKDIR"
mkdir -p "$WORKDIR/data"
cp "$BASE_DSK" "$WORKDIR/data/d0.dsk"
touch "$WORKDIR/data/nvram.bin"
ln -sf "$SIMH_BIN" "$WORKDIR/vax"

cat > "$WORKDIR/vax.ini" <<'INI'
attach nvr data/nvram.bin
set cpu conhalt
set cpu 64m
set idle=vms

set rl disable
set rq enable
set rq0 rd54
attach rq0 data/d0.dsk
set rq1 disable
set rq2 disable
set rq3 disable

set xq enable
set tto 8b

b
INI

LOG="$WORKDIR/console.log"
FIFO="$LOG.in"
rm -f "$LOG" "$FIFO" "$LOG.pid" "$LOG.bootfail"

# --- helpers -----------------------------------------------------------
wait_for() {  # wait_for <regex> <timeout-seconds>
    local pat="$1" timeout="${2:-60}" waited=0
    while ! grep -qE "$pat" "$LOG" 2>/dev/null; do
        sleep 2; waited=$((waited + 2))
        if [ "$waited" -ge "$timeout" ]; then
            echo "TIMEOUT waiting for /$pat/ after ${timeout}s -- tail:" >&2
            tail -c 800 "$LOG" >&2
            return 1
        fi
    done
}
send() { printf '%s\r' "$1" > "$FIFO"; sleep "${2:-2}"; }  # send <text> [settle-seconds]

# wait_for_nudge: like wait_for, but sends a bare CR into the console every
# ~15s while it waits. vms-902 MEASURED: on this open-simh microvax3900
# build, the "Welcome to VAX/VMS.../Username:" banner does not paint on its
# own once STARTUP.COM's accounting record prints -- the emulated OPA0:
# console sits idle (SIMH's own idle-loop detection puts the host thread to
# sleep) until it receives one keystroke, the classic DEC serial-console
# autobaud/wake behavior. A plain wait_for hangs here forever; nudging is
# harmless (VMS ignores a bare CR at a blank line) and unsticks it.
wait_for_nudge() {  # wait_for_nudge <regex> <timeout-seconds>
    local pat="$1" timeout="${2:-60}" waited=0
    while ! grep -qE "$pat" "$LOG" 2>/dev/null; do
        sleep 3; waited=$((waited + 3))
        if [ $((waited % 15)) -eq 0 ]; then
            printf '\r' > "$FIFO" 2>/dev/null || true
        fi
        if [ "$waited" -ge "$timeout" ]; then
            echo "TIMEOUT waiting for /$pat/ after ${timeout}s -- tail:" >&2
            tail -c 800 "$LOG" >&2
            return 1
        fi
    done
}

# --- boot ----------------------------------------------------------------
( cd "$WORKDIR" && python3 "$NODEDRV" "$WORKDIR" "$LOG" \
      --date "$BOOT_DATE" --boot "B DUA0" --no-detach ) &
DRVPID=$!
trap 'kill "$DRVPID" 2>/dev/null || true' EXIT

wait_for_nudge 'Username:' 180   # vms-902: cold VAX boot + full STARTUP.COM (license/audit/
                                  # TP server) measured 70-90s under host contention, PLUS
                                  # the console-wake quirk above.

# --- login (handle both a live and an expired SYSTEM password) -----------
send "SYSTEM" 1.5
send "$SYSTEM_PW" 3
if grep -q 'password has expired' "$LOG"; then
    wait_for 'New password:' 10
    send "$NEW_SYSTEM_PW" 2
    wait_for 'Verification:' 10
    send "$NEW_SYSTEM_PW" 4
fi
wait_for '\$ $' 60   # DCL prompt

# --- vms-902 root cause + fix -------------------------------------------
# MEASURED (rd vms-902): the STOCK base disk's SYS$SYSTEM:MODPARAMS.DAT
# already carries a factory standalone-node stub:
#   SCSSYSTEMID=65534
#   SCSNODE=""
#   VAXCLUSTER=0
# CLUSTER_CONFIG.COM does not replace MODPARAMS.DAT -- it APPENDS its own
# block underneath (SCSNODE="VAXC" SCSSYSTEMID=1989 NISCS_LOAD_PEA0=1
# VAXCLUSTER=2 ...), leaving TWO conflicting VAXCLUSTER directives in one
# file. Extracting MODPARAMS.DAT from the previously-shipped volume
# (tools ods2 extractor, vms-902 investigation) confirmed exactly this
# duplicate, and the live post-AUTOGEN-reboot SHOW VAXCLUSTER on that
# volume read back Current=0 -- the stock stub line won, not
# CLUSTER_CONFIG's override. AUTOGEN reads MODPARAMS.DAT as a permanent
# per-run override table; a live duplicate key is undefined and, measured
# here, resolves to the WRONG (pre-cluster) value every time.
#
# Fix the real VMS way: purge the stock MODPARAMS.DAT before
# CLUSTER_CONFIG.COM ever runs, so its append lands in a clean file with
# exactly ONE VAXCLUSTER directive (=2). This is what CLUSTER_CONFIG.COM
# should have found on a truly "unconfigured" node -- we are restoring
# that precondition, not hand-injecting cluster bytes.
send 'DELETE SYS$SYSTEM:MODPARAMS.DAT;*' 2
wait_for '\$ $' 15

# --- configure clustering the VMS way: CLUSTER_CONFIG.COM ----------------
send '@SYS$MANAGER:CLUSTER_CONFIG' 3
wait_for "DECnet node name" 15
send "$SCSNODE" 2
wait_for "DECnet node address" 10
send "$DECNET_ADDR" 2
wait_for "Ethernet be used" 10
send "Y" 2
wait_for "group number" 10
send "$CLUSTER_GROUP" 2
wait_for "cluster's password:" 10
send "$CLUSTER_PW" 2
wait_for "for verification" 10
send "$CLUSTER_PW" 3
wait_for "disk server" 10
send "N" 2
wait_for "quorum disk" 10
send "N" 3
wait_for "AUTOGEN" 20

# AUTOGEN's SETPARAMS phase (which just ran) already computed and WROTE the
# new SYS$SYSTEM:VAXVMSSYS.PAR to disk, then did its own clean VMS
# SHUTDOWN.COM-driven shutdown (log shows "shutdown was requested by the
# operator" / logfile closed / operator disabled) before triggering this
# automatic reboot -- the disk is already filesystem-consistent as of RIGHT
# NOW, independent of whether the reboot below ever reaches DCL.
#
# The stock VMB console prompts for a boot device on this unattended reboot
# ("Device? [XQA0]:") because no default is persisted in NVRAM. We do NOT
# try to answer it and ride this boot to completion: MEASURED (vms-902) --
# this open-simh microvax3900 build has no network support at all
# (`echo 'show eth' | $SIMH_BIN` -> "network support not available in
# simulator"), so with VAXCLUSTER=2 correctly active, VMS's connection
# manager legitimately blocks FOREVER at "waiting to form or join a
# VAXcluster system" / "%VAXcluster-I-LOADSECDB" -- there is no live DCL
# prompt to reach in this tool. (That live-cluster-formation proof belongs
# to the pcjs/browser environment, which has a real NIC bridge -- see the
# nodeC-drive.js-style harness used to capture the actual 0x6007 SCA frame.)
#
# So: let this boot attempt begin (confirms AUTOGEN's reboot actually
# started), then kill it without waiting for Username -- and verify the
# landed parameters the hang-proof way: a SEPARATE, FRESH conversational
# boot (console flag /1, i.e. "B/1 DUA0") against the SAME disk. SYSBOOT>
# reads the literal VAXVMSSYS.PAR the next real boot will use and prints it
# via SHOW, without mounting any filesystem or touching a single block --
# this is both more authoritative (it is exactly what ships) and immune to
# the no-network hang.
wait_for 'Device\? \[' 180
kill "$DRVPID" 2>/dev/null || true
wait "$DRVPID" 2>/dev/null || true
trap - EXIT
sleep 1

# --- vms-902 hard assertion: never silently ship a non-clustered volume
#     again -----------------------------------------------------------
PLOG="$WORKDIR/sysboot-check.log"
PFIFO="$PLOG.in"
rm -f "$PLOG" "$PFIFO" "$PLOG.pid" "$PLOG.bootfail"
( cd "$WORKDIR" && python3 "$NODEDRV" "$WORKDIR" "$PLOG" \
      --date "$BOOT_DATE" --boot "B/1 DUA0" --no-detach ) &
PDRVPID=$!
trap 'kill "$PDRVPID" 2>/dev/null || true' EXIT

pwait_for() {  # pwait_for <regex> <timeout-seconds> -- bound to $PLOG
    local pat="$1" timeout="${2:-60}" waited=0
    while ! grep -qE "$pat" "$PLOG" 2>/dev/null; do
        sleep 2; waited=$((waited + 2))
        if [ "$waited" -ge "$timeout" ]; then
            echo "TIMEOUT waiting for /$pat/ on SYSBOOT probe after ${timeout}s -- tail:" >&2
            tail -c 800 "$PLOG" >&2
            return 1
        fi
    done
}
psend() { printf '%s\r' "$1" > "$PFIFO"; sleep "${2:-2}"; }

pwait_for 'SYSBOOT>' 90
psend 'SHOW VAXCLUSTER' 3
psend 'SHOW NISCS_LOAD_PEA0' 3
psend 'SHOW SCSNODE' 3
psend 'SHOW SCSSYSTEMID' 3

echo "== SYSBOOT probe evidence (the literal params the next real boot uses) ==" >&2
tail -c 2000 "$PLOG" >&2

VAXCLUSTER_OK=0
grep -qE 'VAXCLUSTER[[:space:]]+2[[:space:]]' "$PLOG" && VAXCLUSTER_OK=1
NISCS_OK=0
grep -qE 'NISCS_LOAD_PEA0[[:space:]]+1[[:space:]]' "$PLOG" && NISCS_OK=1

kill "$PDRVPID" 2>/dev/null || true
wait "$PDRVPID" 2>/dev/null || true
trap - EXIT

if [ "$VAXCLUSTER_OK" -ne 1 ] || [ "$NISCS_OK" -ne 1 ]; then
    echo "FATAL (vms-902 assertion): SYSBOOT> SHOW did not confirm VAXCLUSTER=2" \
         "and NISCS_LOAD_PEA0=1 in the parameter file the next boot will use --" \
         "refusing to package a non-clustered volume." >&2
    exit 1
fi
echo "OK (vms-902 assertion): SYSBOOT confirms VAXCLUSTER=2, NISCS_LOAD_PEA0=1 --" \
     "the live full boot legitimately then blocks at 'waiting to form or join a" \
     "VAXcluster system' on this no-network SIMH tool (expected, see header; the" \
     "pcjs/browser environment is where live cluster-formation + the real 0x6007" \
     "SCA frame get proven)." >&2

# --- package ---------------------------------------------------------------
# The disk is already filesystem-consistent (AUTOGEN's own clean shutdown,
# above); neither post-AUTOGEN boot attempt above mounted or wrote to it
# (SYSBOOT never touches the filesystem; the killed full-boot attempt died
# before STARTUP.COM/SYSINIT -- no audit-server/OPCOM startup messages ever
# appeared for it, confirming it never got that far).
gzip -9 -c "$WORKDIR/data/d0.dsk" > "$OUT"
echo "Wrote $OUT"
gzip -t "$OUT" && echo "gzip integrity OK"
