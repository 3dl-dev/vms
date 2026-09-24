#!/bin/bash
# build_nodeC_vms73_cluster.sh -- rd vms-735 (Node-C lane), rd vms-2570
#
# Builds the Node-C cluster-member disk for the browser cluster demo: a REAL
# OpenVMS VAX V7.3 system disk (ODS-2), configured for VMScluster membership
# the VMS way (first-boot personalization -> @SYS$MANAGER:CLUSTER_CONFIG_LAN
# -> SYSGEN/AUTOGEN -- never hand-injected bytes), matching the identity
# frozen in the demo's single source of truth:
#
#   tools/cluster-web-demo/mk_democonfig.py DEMO_NODE_C
#   SCSNODE=VAXC, SCSSYSTEMID=1989, group=257, ALLOCLASS=0,
#   VOTES=1, EXPECTED_VOTES=1 (Node C is cluster GENESIS -- it forms the
#   1-node cluster alone; A and B raise CN as they join over the shared L2).
#
# DECISION: rd vms-d24 (Baron 2026-09-23) -- Node C = real VAX/VMS V7.3
# (option A), superseding the V5.5 path (vms-882's V5.5 SCS-revision campaign
# is no longer on the demo critical path; build_nodeC_vms55_cluster.sh stays
# as the V5.5 recipe for anyone who needs it).
#
# PROVENANCE (do not re-litigate; see docs/design/cluster-web-demo.md and the
# rd vms-d24 decision record for the operator authorization):
#   - Base media: SYS$MANAGER-personalizable OpenVMS VAX V7.3 media as staged
#     in the ovmx-lab cluster-interop lab (k3s namespace ovmx-lab, PVC
#     vax-lab-pvc, paths /lab/media/openvms073.iso + a brand-new post-restore
#     system-disk image /lab/cluster/data/d0-newinst073.dsk -- the SAME base
#     image tests/lab/*/clean-cluster/FORMATION-NOTES.md used to build the
#     lab's own from-scratch 2-node V7.3 reference cluster, VAXCLUSTER-license-
#     free per that document's SS5 finding).
#   - This script builds a SEPARATE demo copy (group 257) from a fresh clone
#     of the base disk -- it NEVER touches the lab's own group-1 reference
#     cluster (data/d0.dsk, the live vaxlab StatefulSet volumes).
#   - Public-ship authorization: Baron, 2026-09-23, rd vms-d24. This script
#     does not itself publish anywhere -- see "PUBLISHING" below.
#
# PREREQUISITES (this script is meant to run INSIDE an ovmx-lab vaxlab pod,
# which already carries the SIMH binary + nodedrv.py + base media; see
# "RUNNING IN THE LAB" below for the exact invocation):
#   - A MicroVAX 3900 SIMH binary with console-ROM + (optionally) tap
#     Ethernet support, e.g. the lab's /usr/local/bin/vax (same binary the
#     vaxlab StatefulSet's own reference cluster runs).
#   - data/training/vax/cluster/nodedrv.py-equivalent console driver (the
#     lab image ships it at /usr/local/bin/nodedrv.py) -- pty + FIFO
#     injection, echo-verified boot-command typing, ENTER DATE AND TIME
#     auto-answer.
#   - The base disk BASE_DSK (a fresh, never-personalized V7.3 RA92 image,
#     ~1.5GB / 2,940,951 blocks) and the distribution CD image ISO (needed
#     for the "OpenVMS library" optional-component restore during
#     personalization).
#
# RUNNING IN THE LAB
#   kubectl -n ovmx-lab exec <an idle vaxlab-N pod> -- sh -c '...'
#   (the vaxlab StatefulSet's PVC is shared across all replicas -- write your
#   working copy to a NEW subdirectory under /lab, e.g.
#   /lab/cluster-demo-nodeC-v73/, never into /lab/cluster/data/d0.dsk or
#   /lab/k8s-labs/<pod>/ -- those are the LIVE group-1 reference cluster).
#   Because this script drives dozens of first-boot-personalization prompts
#   (volume label, distribution media, optional components, passwords,
#   SCSNODE/SCSSYSTEMID, PAKs, timezone) it is intentionally NOT a single
#   unattended `expect`-free pipe the way build_nodeC_vms55_cluster.sh's
#   CLUSTER_CONFIG.COM-only flow is -- see the numbered PHASE comments below,
#   which mirror the exact prompt sequence measured 2026-09-23 (rd vms-2570)
#   driving this same base image inside vaxlab-4.
#
# USAGE
#   BASE_DSK=/lab/cluster/data/d0-newinst073.dsk \
#   ISO=/lab/media/openvms073.iso \
#   SIMH_BIN=/usr/local/bin/vax \
#   NODEDRV=/usr/local/bin/nodedrv.py \
#   WORKDIR=/lab/cluster-demo-nodeC-v73 \
#   OUT=/lab/cluster-demo-nodeC-v73/vms73-nodeC-cluster.dsk.gz \
#   CLUSTER_PW=<the demo's real cluster password -- an operator fact, never committed> \
#     ./build_nodeC_vms73_cluster.sh
#
# OUTPUT: $OUT -- gzip -9 of the cluster-configured system disk, ready to
# drop in next to vms55-nodeC-cluster.dsk.gz for the pcjs cluster-node page
# (browser/ovmx-cluster.html ?diskgz=<url>).
#
# PUBLISHING: this script never pushes anywhere. Publishing the built disk to
# vax.3dl.network (baron-3dl/pcjs) is Baron-manual (the 76MB-class binary
# push hits the auto-mode guardrail) -- serve $OUT locally for any proof run.
set -euo pipefail

BASE_DSK="${BASE_DSK:-/lab/cluster/data/d0-newinst073.dsk}"
ISO="${ISO:-/lab/media/openvms073.iso}"
SIMH_BIN="${SIMH_BIN:-/usr/local/bin/vax}"
NODEDRV="${NODEDRV:-/usr/local/bin/nodedrv.py}"
WORKDIR="${WORKDIR:-/lab/cluster-demo-nodeC-v73}"
OUT="${OUT:-/lab/cluster-demo-nodeC-v73/vms73-nodeC-cluster.dsk.gz}"
TAP_DEV="${TAP_DEV:-}"          # optional: e.g. tap5 (an ALREADY-created tap on the
                                 # pod's own isolated br0) to prove live genesis mid-build;
                                 # empty = no network attach, config-only build (matches
                                 # build_nodeC_vms55_cluster.sh's no-tap default).

SYSTEM_PW="${SYSTEM_PW:-OVMXCLUSTER1}"        # SYSTEM/SYSTEST/FIELD passwords (>=8 chars, checked)
VOL_LABEL="${VOL_LABEL:-VAXCSYS}"
SCSNODE="VAXC"
SCSSYSTEMID=1989
CLUSTER_GROUP=257
CLUSTER_PW="${CLUSTER_PW:?set CLUSTER_PW -- the demo real cluster password is an operator fact, never committed}"
BOOT_DATE="${BOOT_DATE:-$(date '+%d-%b-%Y %H:%M' | tr a-z A-Z)}"

[ -f "$BASE_DSK" ] || { echo "FATAL: BASE_DSK not found: $BASE_DSK" >&2; exit 1; }
[ -f "$ISO" ]      || { echo "FATAL: ISO not found: $ISO" >&2; exit 1; }
[ -x "$SIMH_BIN" ] || { echo "FATAL: SIMH_BIN not executable: $SIMH_BIN" >&2; exit 1; }
[ -f "$NODEDRV" ]  || { echo "FATAL: NODEDRV not found: $NODEDRV" >&2; exit 1; }

mkdir -p "$WORKDIR/data"
if [ ! -f "$WORKDIR/data/d0.dsk" ]; then
    cp "$BASE_DSK" "$WORKDIR/data/d0.dsk"
fi
touch "$WORKDIR/data/nvram.bin"
ln -sf "$SIMH_BIN" "$WORKDIR/vax"

{
cat <<INI
attach nvr data/nvram.bin
set cpu conhalt
set cpu 64m
set idle=vms

set rl disable
set rq enable
set rq0 ra92
attach rq0 data/d0.dsk
set rq1 disable
set rq2 cdrom
attach -r rq2 $ISO
set rq3 disable

set xq enable
INI
[ -n "$TAP_DEV" ] && echo "at xq tap:$TAP_DEV"
cat <<INI
set tto 8b

b
INI
} > "$WORKDIR/vax.ini"

LOG="$WORKDIR/console.log"
FIFO="$LOG.in"
rm -f "$LOG" "$FIFO" "$LOG.pid" "$LOG.bootfail"

wait_for() {  # wait_for <regex> <timeout-seconds>
    local pat="$1" timeout="${2:-60}" waited=0
    while ! grep -qaE "$pat" "$LOG" 2>/dev/null; do
        sleep 2; waited=$((waited + 2))
        if [ "$waited" -ge "$timeout" ]; then
            echo "TIMEOUT waiting for /$pat/ after ${timeout}s -- tail:" >&2
            tail -c 800 "$LOG" >&2
            return 1
        fi
    done
}
send() { printf '%s\r' "$1" > "$FIFO"; sleep "${2:-2}"; }  # send <text> [settle-seconds]

wait_for_nudge() {  # like wait_for, but nudges a bare CR every ~15s (console-wake quirk,
                     # measured identically on the V5.5 build -- see build_nodeC_vms55_
                     # cluster.sh's wait_for_nudge comment).
    local pat="$1" timeout="${2:-60}" waited=0
    while ! grep -qaE "$pat" "$LOG" 2>/dev/null; do
        sleep 3; waited=$((waited + 3))
        if [ $((waited % 15)) -eq 0 ]; then printf '\r' > "$FIFO" 2>/dev/null || true; fi
        if [ "$waited" -ge "$timeout" ]; then
            echo "TIMEOUT waiting for /$pat/ after ${timeout}s -- tail:" >&2
            tail -c 800 "$LOG" >&2
            return 1
        fi
    done
}

# --- boot ------------------------------------------------------------------
( cd "$WORKDIR" && python3 "$NODEDRV" "$WORKDIR" "$LOG" \
      --date "$BOOT_DATE" --boot "B DUA0" --no-detach ) &
DRVPID=$!
trap 'kill "$DRVPID" 2>/dev/null || true' EXIT

# PHASE 1: OpenVMS VAX V7.3 Installation Procedure (first-boot personalization
# of a truly fresh disk -- measured 2026-09-23, rd vms-2570).
wait_for 'Please enter the date and time' 90
send "$BOOT_DATE" 3
wait_for 'volume label for this system disk' 15
send "$VOL_LABEL" 3
wait_for 'drive holding the OpenVMS distribution media' 10
send "DUA2:" 2
wait_for 'ready to be mounted' 10
send "Y" 3
# Optional-software menu: decline everything except the OpenVMS library
# (matches this script's minimal footprint; clustering itself needs neither
# DECwindows nor DECnet -- see tests/lab/*/clean-cluster/FORMATION-NOTES.md
# SS4(a), "cluster configuration does not need DECnet running"). We install
# DECnet NOTHING here and instead bring the LAN up via LAN$STARTUP in Phase 2
# -- simpler and license-free (DECnet Phase IV needs a PAK to actually START
# its circuit; LAN$STARTUP does not).
wait_for 'install the OpenVMS library files' 15
send "Y" 2
wait_for 'install the OpenVMS optional files' 15
send "N" 2
wait_for 'install the MSGHLP database' 15
send "N" 2
wait_for 'optional OpenVMS Management Station files' 15
send "N" 2
wait_for 'DECwindows base support' 15
send "N" 2
wait_for 'install DECnet-Plus' 15
send "N" 2
wait_for 'install DECnet Phase IV' 15
send "N" 3
wait_for 'Is this correct' 10
send "Y" 4
wait_for 'restoring OpenVMS library save set' 30 || true
# SYSTEM / SYSTEST / FIELD passwords (each prompts twice for verification).
wait_for 'Enter password for SYSTEM' 60
send "$SYSTEM_PW" 2
wait_for 'Re-enter for verification' 10
send "$SYSTEM_PW" 3
wait_for 'Enter password for SYSTEST' 15
send "$SYSTEM_PW" 2
wait_for 'Re-enter for verification' 10
send "$SYSTEM_PW" 3
wait_for 'Enter password for FIELD' 15
send "$SYSTEM_PW" 2
wait_for 'Re-enter for verification' 10
send "$SYSTEM_PW" 3
wait_for 'Please enter the SCSNODE name' 20
send "$SCSNODE" 2
wait_for 'Please enter the SCSSYSTEMID' 10
send "$SCSSYSTEMID" 3
wait_for 'register any Product Authorization Keys' 60
send "N" 2
wait_for 'best describes your location' 15
send "33" 3          # "US" (see the disk's own Zone Menu; not VMS-specific --
                      # matches FORMATION-NOTES' choice; adjust if your
                      # deployment target needs a different TZ)
wait_for 'Is this correct' 10
send "" 3             # accept [YES]
wait_for 'best describes your location' 15
send "6" 3           # "Eastern"
send "" 3             # accept [YES]
wait_for 'Daylight Savings time in effect' 15
send "Y" 3
wait_for 'Enter the Time Differential Factor' 10
send "" 3             # accept default -4:00
wait_for 'Is this correct' 10
send "Y" 6

# AUTOGEN runs + reboots automatically into the personalized system.
wait_for 'REBOOT phase is beginning' 120
wait_for 'Device\? \[' 180 || true    # fresh disk has no nvram boot default the FIRST time
if grep -qa 'Device? \[' "$LOG" 2>/dev/null; then send "DUA0" 4; fi
wait_for_nudge 'Username:' 180
send "SYSTEM" 2
send "$SYSTEM_PW" 4
wait_for '\$ $' 30

# PHASE 2: bring the LAN up (no DECnet installed) + configure clustering.
send '@SYS$STARTUP:LAN$STARTUP' 3   # harmless "already running" if LANACP auto-started
wait_for '\$ $' 15
send '@SYS$MANAGER:CLUSTER_CONFIG_LAN' 3
wait_for 'Enter choice \[1\]' 15
send "1" 2
wait_for 'LAN be used for cluster communications' 10
send "Y" 2
wait_for "cluster's group number" 10
send "$CLUSTER_GROUP" 2
wait_for "cluster's password:" 10
send "$CLUSTER_PW" 2
wait_for "verification:" 10
send "$CLUSTER_PW" 3
wait_for 'be a boot server' 10
send "N" 2
wait_for 'be a disk server' 10
send "N" 2
wait_for 'contain a quorum disk' 10
send "N" 3
wait_for 'run AUTOGEN now' 15
send "Y" 3
wait_for 'REBOOT phase is beginning' 60
# This reboot has an nvram default now (set during Phase 1's first AUTOGEN
# reboot) -- no Device? prompt expected, but tolerate one if the emulator
# ever needs it.
wait_for 'Device\? \[' 30 || true
if grep -qa 'Device? \[' "$LOG" 2>/dev/null; then send "DUA0" 4; fi
wait_for_nudge 'Username:' 180
send "SYSTEM" 2
send "$SYSTEM_PW" 4
wait_for '\$ $' 30

# --- vms-902-pattern hard assertion: never silently ship a non-clustered
#     volume. Unlike the V5.5 build's SIMH (no network support at all), this
#     lab's SIMH binary DOES support real Ethernet (the vaxlab StatefulSet's
#     own reference cluster runs on it) -- so this asserts via a LIVE
#     genesis, not just a SYSBOOT parameter probe: F$GETSYI + SDA's
#     SHOW CLUSTER, the same non-screen-mode decoder ring
#     tests/lab/*/README-lab.md documents (plain SHOW CLUSTER needs a
#     terminal-table entry this minimal-tailored disk does not carry --
#     SDA's SHOW CLUSTER needs no screen management).
send 'SET TERMINAL/WIDTH=132/NOBROADCAST' 2
send 'WRITE SYS$OUTPUT F$GETSYI("EXPECTED_VOTES"),F$GETSYI("VOTES"),F$GETSYI("NODENAME"),F$GETSYI("CLUSTER_NODES")' 4
send 'ANALYZE/SYSTEM' 4
send 'SHOW CLUSTER' 6
send 'EXIT' 3
wait_for '\$ $' 15

echo "== console evidence (identity + genesis) ==" >&2
grep -aE 'CNXMAN|CSID|VAXC|1989' "$LOG" | tail -30 >&2

CLUSTER_OK=0
grep -qaE 'now a VAXcluster member -- system VAXC' "$LOG" && CLUSTER_OK=1
if [ "$CLUSTER_OK" -ne 1 ]; then
    echo "FATAL (vms-2570 assertion): never saw '%CNXMAN, now a VAXcluster member -- system VAXC'" \
         "in the console log -- refusing to package a non-clustered volume." >&2
    exit 1
fi
echo "OK (vms-2570 assertion): VAXC formed its own 1-node VAXcluster (real genesis)," \
     "group $CLUSTER_GROUP, SCSSYSTEMID $SCSSYSTEMID." >&2

# --- clean shutdown (filesystem-consistent disk) ---------------------------
send '@SYS$SYSTEM:SHUTDOWN' 3
wait_for 'How many minutes' 15
send "" 2
wait_for 'Reason for shutdown' 10
send "vms-2570 demo build" 2
wait_for 'spin down the disk volumes' 10
send "NO" 2
wait_for 'site-specific shutdown procedure' 10
send "YES" 2
wait_for 'automatic system reboot be performed' 10
send "NO" 2
wait_for 'When will the system be rebooted' 10
send "" 2
wait_for 'Shutdown options' 10
send "" 6
wait_for 'SHUTDOWN COMPLETE' 60
kill "$DRVPID" 2>/dev/null || true
wait "$DRVPID" 2>/dev/null || true
trap - EXIT
sleep 1

# --- package -----------------------------------------------------------------
gzip -9 -c "$WORKDIR/data/d0.dsk" > "$OUT"
echo "Wrote $OUT"
gzip -t "$OUT" && echo "gzip integrity OK"
sha256sum "$OUT"
