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
# DECnet variant (rd vms-05d) -- vms73-nodeC-decnet-cluster.dsk.gz: the SAME
# volume plus DECnet-VAX Phase IV installed + configured, NO license PAK
# (bring-your-own-license; see PHASE 3 and PROVENANCE-vms73-nodeC.md):
#   DECNET=1 \
#   FROM_GZ=/lab/cluster-demo-nodeC-v73/vms73-nodeC-cluster.dsk.gz \
#   FROM_SHA256=45355fd2631c59666b0fe2839d17ed743e7d045646d2c179a8b3402915b4fe15 \
#   ISO=/lab/media/openvms073.iso WORKDIR=<fresh dir> \
#   OUT=<dir>/vms73-nodeC-decnet-cluster.dsk.gz \
#     ./build_nodeC_vms73_cluster.sh
#   (no CLUSTER_PW needed: the input volume already carries it.)
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
BOOT_DATE="${BOOT_DATE:-$(date '+%d-%b-%Y %H:%M' | tr a-z A-Z)}"

# DECnet Phase IV (rd vms-05d, Baron 2026-10-21): DECNET=1 installs DECnet-VAX
# from the V7.3 distribution, configures it with NETCONFIG + NCP, and has the
# site startup start it -- but registers NO license PAK, so at every boot
# STARTNET fails with VMS's own license message (bring-your-own-license).
# See PHASE 3 below.
DECNET="${DECNET:-0}"
# FROM_GZ: instead of personalizing BASE_DSK from scratch (PHASES 1-2), start
# from an ALREADY-BUILT Node C volume -- the published vms73-nodeC-cluster.dsk.gz
# -- and apply only PHASE 3 to it. Its cluster identity, cluster password and
# SYSTEM password are then carried over bit-for-bit (the operator's cluster
# password is never needed, never typed). FROM_SHA256 pins the input.
FROM_GZ="${FROM_GZ:-}"
FROM_SHA256="${FROM_SHA256:-}"
if [ -z "$FROM_GZ" ]; then
    CLUSTER_PW="${CLUSTER_PW:?set CLUSTER_PW -- the demo real cluster password is an operator fact, never committed}"
    [ -f "$BASE_DSK" ] || { echo "FATAL: BASE_DSK not found: $BASE_DSK" >&2; exit 1; }
fi
[ -f "$ISO" ]      || { echo "FATAL: ISO not found: $ISO" >&2; exit 1; }
[ -x "$SIMH_BIN" ] || { echo "FATAL: SIMH_BIN not executable: $SIMH_BIN" >&2; exit 1; }
[ -f "$NODEDRV" ]  || { echo "FATAL: NODEDRV not found: $NODEDRV" >&2; exit 1; }

mkdir -p "$WORKDIR/data"
if [ -n "$FROM_GZ" ]; then
    [ "$DECNET" = 1 ] || { echo "FATAL: FROM_GZ only makes sense with DECNET=1" >&2; exit 1; }
    [ -f "$FROM_GZ" ] || { echo "FATAL: FROM_GZ not found: $FROM_GZ" >&2; exit 1; }
    if [ -n "$FROM_SHA256" ]; then
        echo "$FROM_SHA256  $FROM_GZ" | sha256sum -c - >&2 \
            || { echo "FATAL: FROM_GZ sha256 mismatch" >&2; exit 1; }
    fi
    [ -f "$WORKDIR/data/d0.dsk" ] && { echo "FATAL: $WORKDIR/data/d0.dsk exists -- use a fresh WORKDIR" >&2; exit 1; }
    # cp does read /dev/stdin here; --sparse keeps the 1.5 GB RA92 image sparse.
    # shellcheck disable=SC2216
    gzip -dc "$FROM_GZ" | cp --sparse=always /dev/stdin "$WORKDIR/data/d0.dsk"
elif [ ! -f "$WORKDIR/data/d0.dsk" ]; then
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

# MARK: wait_for/wait_for_nudge only look at console output after byte MARK
# (0 = the whole log). PHASE 3 reboots and re-sees prompts that already appear
# earlier in the log, so it calls mark() before each wait that must be fresh.
MARK=0
mark() { MARK=$(stat -c %s "$LOG" 2>/dev/null || echo 0); }
since_mark() { tail -c +$((MARK + 1)) "$LOG" 2>/dev/null | tr -d '\000\r'; }

wait_for() {  # wait_for <regex> <timeout-seconds>
    local pat="$1" timeout="${2:-60}" waited=0
    while ! since_mark | grep -aE "$pat" >/dev/null; do
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
    while ! since_mark | grep -aE "$pat" >/dev/null; do
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

if [ -n "$FROM_GZ" ]; then
# FROM_GZ: the volume is already personalized + cluster-configured -- just log in.
wait_for_nudge 'Username: ?$' 400   # the prompt, not the audit-alarm 'Username:' field
send "SYSTEM" 2
wait_for 'Password: ?$' 15
send "$SYSTEM_PW" 4
wait_for '\$ $' 60
else
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
fi   # PHASES 1-2 (skipped with FROM_GZ)

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

# --- PHASE 3 (DECNET=1, rd vms-05d): DECnet-VAX Phase IV installed + configured,
#     UNLICENSED (bring-your-own-license). Prompt sequence measured 2026-10-21
#     driving the published V7.3 Node C volume inside an isolated ovmx-lab pod.
#
#   - The kit: the V7.3 CD's own PCSI kit,
#     DUA2:[DECNET_PHASE_IV_VAX073.KIT]DEC-VAXVMS-DECNET_PHASE_IV-V0703--1.PCSI.
#   - The DECnet address is NOT the operator's to pick: STARTNET.COM on a
#     cluster member refuses to start DECnet unless executor address ==
#     SCSSYSTEMID ("DECnet address of local node must match VAXcluster system
#     ID." / "DECnet not started."), and DECdtm refuses to start too
#     (%DECdtm-W-ADDRNEQ). SCSSYSTEMID 1989 = 1*1024 + 965, so VAXC is 1.965 --
#     NETCONFIG offers exactly that as its default and we accept it.
#   - Peers: the OVMX nodes of tools/cluster-web-demo/mk_democonfig.py's
#     DEMO_ROSTER (OVMXA 1.1, OVMXB 1.2), defined with NCP DEFINE NODE.
#   - Startup: SYSTARTUP_VMS.COM's own "$!$ START/NETWORK DECNET" line is
#     uncommented (VMS's documented choice when there are no batch queues), so
#     STARTNET runs at boot on the console.
#   - NO PAK is registered. At boot STARTNET's NCP SET KNOWN CIRCUITS fails with
#     VMS's own %SYSTEM-F-NOLICENSE -- asserted below, never hidden.
if [ "$DECNET" = 1 ]; then
DN_AREA=$((SCSSYSTEMID / 1024))
DN_NODE=$((SCSSYSTEMID % 1024))
DN_ADDR="$DN_AREA.$DN_NODE"
# Peers = the OVMX nodes of the demo roster (name address), kept in step with
# tools/cluster-web-demo/mk_democonfig.py DEMO_NODE_A/DEMO_NODE_B "decnet".
DN_PEERS="${DN_PEERS:-OVMXA:1.1 OVMXB:1.2}"

mark
send 'MOUNT/OVERRIDE=IDENTIFICATION DUA2:' 3
wait_for 'mounted on' 30
mark
send 'PRODUCT INSTALL DECNET_PHASE_IV /SOURCE=DUA2:[DECNET_PHASE_IV_VAX073.KIT]' 3
wait_for 'Do you want to continue\? \[YES\]' 60
mark
send "YES" 3
wait_for 'Support addendum.*|Do you want to continue\? \[YES\]' 120
wait_for 'Do you want to continue\? \[YES\]' 60
mark
send "YES" 3
wait_for 'The following product has been installed' 600
wait_for '\$ $' 30

# NETPROXY.DAT: without it every STARTNET logs "Error opening permanent proxy
# database" -- the same AUTHORIZE step any VMS manager does after installing DECnet.
mark
send 'SET DEFAULT SYS$SYSTEM' 2
send 'MCR AUTHORIZE CREATE/PROXY' 4
wait_for '\$ $' 30
send 'SET DEFAULT SYS$LOGIN' 2

mark
send '@SYS$MANAGER:NETCONFIG' 3
wait_for "DECnet node name to be\\?.*\\[$SCSNODE\\]: \$" 60
send "" 3
wait_for "DECnet address to be\\?.*\\[$DN_ADDR\\]: \$" 15
send "" 3
wait_for 'operate as a router\?' 15
send "NO" 3
for obj in 'default DECnet account' 'MAIL object' 'FAL object' 'PHONE object' \
           'NML object' 'MIRROR object' 'VPM object'; do
    wait_for "$obj" 15
    mark
    send "NO" 3
done
wait_for 'Press RETURN to continue' 15
send "" 3
wait_for 'Do you want these commands to be executed\?' 15
mark
send "YES" 6
wait_for 'Do you want DECnet started\?' 60
send "NO" 3
wait_for '\$ $' 15
for p in $DN_PEERS; do
    send "MCR NCP DEFINE NODE ${p#*:} NAME ${p%%:*}" 3
done

# Uncomment SYSTARTUP_VMS.COM's own START/NETWORK DECNET line (EDT line mode).
mark
send 'CREATE SYS$LOGIN:STARTNET_ON.EDT' 2
send 'S"$!$ START/NETWORK DECNET"$ START/NETWORK DECNET" WHOLE' 1
send 'EXIT' 1
printf '\032' > "$FIFO"; sleep 2
send 'EDIT/EDT/COMMAND=SYS$LOGIN:STARTNET_ON.EDT SYS$MANAGER:SYSTARTUP_VMS.COM' 6
wait_for '\$ START/NETWORK DECNET' 30
send 'DELETE SYS$LOGIN:STARTNET_ON.EDT;*' 2
send 'PURGE SYS$MANAGER:SYSTARTUP_VMS.COM' 2

# Reboot, and prove at boot: cluster genesis still happens, and STARTNET fails
# with VMS's own license message (no PAK on this volume).
mark
send '@SYS$SYSTEM:SHUTDOWN' 3
wait_for 'How many minutes' 15
send "" 2
wait_for 'Reason for shutdown' 10
send "vms-05d DECnet configured" 2
wait_for 'spin down the disk volumes' 10
send "NO" 2
wait_for 'site-specific shutdown procedure' 10
send "YES" 2
wait_for 'automatic system reboot be performed' 10
send "YES" 2
wait_for 'When will the system be rebooted' 10
send "" 2
wait_for 'Shutdown options' 10
mark
send "" 6
# FROM_GZ starts with a blank nvram, so the KA655 asks for the boot device on
# this first reboot (as PHASE 1's first AUTOGEN reboot does) and would fall
# back to XQA0 if left unanswered.
wait_for 'Device\? \[' 180 || true
if since_mark | grep -aE 'Device\? \[' >/dev/null; then send "DUA0" 4; fi
wait_for 'now a VAXcluster member -- system VAXC' 600
wait_for_nudge 'Username: ?$' 400
BOOTLOG="$WORKDIR/decnet-boot-console.log"
since_mark > "$BOOTLOG"
echo "== DECnet boot evidence (rd vms-05d) ==" >&2
grep -aE 'DECnet|NCP-|NOLICENSE|LICENSE|CNXMAN|DECdtm' "$BOOTLOG" >&2 || true
grep -qa '%SYSTEM-F-NOLICENSE, operation requires software license' "$BOOTLOG" || {
    echo "FATAL (vms-05d): STARTNET did not report %SYSTEM-F-NOLICENSE at boot" >&2; exit 1; }
if grep -qaE 'must match VAXcluster|DECnet not started|ADDRNEQ' "$BOOTLOG"; then
    echo "FATAL (vms-05d): DECnet address/name does not match the cluster identity" >&2; exit 1
fi
send "SYSTEM" 2
wait_for 'Password: ?$' 15
send "$SYSTEM_PW" 4
wait_for '\$ $' 60
send 'SET TERMINAL/WIDTH=132/NOBROADCAST' 2
mark
send 'MCR NCP LIST KNOWN NODES' 4
send 'MCR NCP LIST EXECUTOR CHARACTERISTICS' 4
send 'SHOW LICENSE' 6
wait_for '\$ $' 15
since_mark > "$WORKDIR/decnet-ncp-list.log"
cat "$WORKDIR/decnet-ncp-list.log" >&2
grep -qaE "Executor node = $DN_ADDR \\($SCSNODE\\)" "$WORKDIR/decnet-ncp-list.log" || {
    echo "FATAL (vms-05d): executor is not $DN_ADDR ($SCSNODE)" >&2; exit 1; }
for p in $DN_PEERS; do
    grep -qaE "Remote node = +${p#*:} \\(${p%%:*}\\)" "$WORKDIR/decnet-ncp-list.log" || {
        echo "FATAL (vms-05d): node ${p%%:*} ${p#*:} missing from the permanent database" >&2; exit 1; }
done
if grep -qaE 'DVNET|NET-APP-SUP' "$WORKDIR/decnet-ncp-list.log"; then
    echo "FATAL (vms-05d): a DECnet license is active -- this volume must ship UNLICENSED" >&2; exit 1
fi
echo "OK (vms-05d): DECnet Phase IV installed + configured as $DN_ADDR ($SCSNODE), no PAK;" \
     "STARTNET fails honestly at boot with %SYSTEM-F-NOLICENSE." >&2
fi   # PHASE 3

# --- clean shutdown (filesystem-consistent disk) ---------------------------
mark
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
