#!/usr/bin/env bash
# clusterctl.sh -- deterministic matched-pair cluster-config for lab-Alpha (vms-0d1 harness).
#
# ============================== STATUS: LICENSE-GATED, UNVALIDATED ==============================
# This script AUTHORS the reproducible "turn a standalone golden clone into a cluster member"
# step so it is ready to run THE MOMENT an Alpha VMScluster licence lands. It has NOT been run:
# forming an Alpha VMScluster needs the VMScluster licensed facility, which is an OPERATOR-RESERVED
# decision (tests/lab-alpha/README.md "Still open": "do not work around it"; clean source = VSI
# Community Licence Programme Alpha PAKs). DO NOT run this to actually form a cluster until the
# operator rules the licence in. The parameter values below are transcribed from the README's
# "How the cluster was configured" section (already proven once, on the hand-built alpha2 disk).
# ================================================================================================
#
# WHY THIS EXISTS (the finding that motivated it, vms-0d1, 2026-08-22):
#   The lab-Alpha golden (alpha1-sys.golden.img) is STANDALONE -- SYSGEN on a fresh clone shows
#   VAXCLUSTER=0, NISCS_LOAD_PEA0=0 (it carries the ALPHA1/2049 identity but ZERO cluster
#   participation). A fresh golden clone therefore NEVER clusters. The only cluster-capable Alpha
#   disk that exists is a hand-built survivor (alpha2 = ALPHA2/2050), which made every prior
#   2-node observation depend on whatever disks survived the last run. This script removes that
#   dependency: golden clone + per-node config -> a deterministic cluster member, for BOTH nodes.
#
# WHAT IT DOES (per node, over the AXPbox serial console via the srmdrv FIFO):
#   1. Log in on OPA0: (SYSTEM / ovmxlab2026).
#   2. Append the cluster SYSGEN params + this node's identity to SYS$SYSTEM:MODPARAMS.DAT
#      (DCL OPEN/APPEND -- scriptable, no editor needed).
#   3. Establish cluster authorisation (group number + password) -> SYS$SYSTEM:CLUSTER_AUTHORIZE.DAT.
#   4. AUTOGEN (GETDATA SETPARAMS) then reboot -> the node comes up as a cluster member.
#
# USAGE (once licensed):
#   tests/lab-alpha/tools/clusterctl.sh <node> <scssystemid> [pod] [namespace]
#   e.g.  clusterctl.sh alpha1 2049
#         clusterctl.sh alpha2 2050
#   Convention (README): ALPHA1/2/3 -> DECnet 2.1/2.2/2.3 -> SCSSYSTEMID 2049/2050/2051.
#
# NOTE on step 3 (the ONE step to VERIFY against the oracle on the first licensed run): the README
# configured cluster auth via the interactive @SYS$MANAGER:CLUSTER_CONFIG_LAN.COM. This script uses
# the non-interactive SYSMAN equivalent (CONFIGURATION SET CLUSTER_AUTHORIZATION). If SYSMAN refuses
# on a not-yet-cluster node, fall back to CLUSTER_CONFIG_LAN.COM (form/join, LAN comms yes, IP no,
# boot server yes, ALLOCLASS 0, no quorum disk, EXPECTED_VOTES 2). Everything else here is the
# scriptable path and is not in doubt.
set -euo pipefail

NODE="${1:?usage: clusterctl.sh <node> <scssystemid> [pod] [namespace]}"
SYSID="${2:?need SCSSYSTEMID (e.g. 2049 for alpha1, 2050 for alpha2)}"
POD="${3:-alphalab-0}"
NS="${4:-ovmx-lab}"

# --- cluster parameters (README: "How the cluster was configured") ---------------------------
GROUP_NUMBER=2026
GROUP_PASSWORD=OVMXALPHACLU
EXPECTED_VOTES=2
NODE_VOTES=1
ALLOCLASS=0

FIFO="/lab/k8s-labs/${POD}/${NODE}/logs/${NODE}.log.in"
CLOG="/lab/k8s-labs/${POD}/${NODE}/logs/${NODE}.log"
NODE_UC="$(printf '%s' "${NODE}" | tr '[:lower:]' '[:upper:]')"

kx() { kubectl -n "${NS}" exec "${POD}" -- sh -c "$1"; }
# send one console line via the FIFO (srmdrv drains it; '\n' is the correct ending, not '\r')
send() { kx "printf '%s\n' \"$1\" > ${FIFO}"; sleep 1.5; }
# wait until a marker appears near the tail of the console log
waitfor() {
  local marker="$1" tries="${2:-30}" i
  for i in $(seq 1 "${tries}"); do
    if kx "tail -c 400 ${CLOG} 2>/dev/null | grep -aq '${marker}'"; then return 0; fi
    sleep 2
  done
  echo "clusterctl: TIMEOUT waiting for '${marker}' on ${NODE}" >&2; return 1
}

echo "clusterctl: configuring ${NODE_UC} (SCSSYSTEMID ${SYSID}) as a cluster member on ${POD}"

# --- 1. log in on OPA0: (async OPCOM scrolls the prompt away -> nudge with a bare newline) -----
send ""                                  # re-elicit Username: if it scrolled off
waitfor 'Username:' || { echo "clusterctl: no login prompt; is the node booted to login?" >&2; exit 1; }
send "SYSTEM"
waitfor 'Password:'
send "ovmxlab2026"
waitfor 'Welcome to OpenVMS'              # LOGIN-S-LOGOPRCON banner
send ""                                  # settle to the '$' DCL prompt

# --- 2. append cluster params + this node's identity to MODPARAMS.DAT (AUTOGEN-persistent) -----
send "OPEN/APPEND OVMXPF SYS\$SYSTEM:MODPARAMS.DAT"
send "WRITE OVMXPF \"! OVMX lab cluster config (vms-0d1 clusterctl.sh) -- ${NODE_UC}\""
send "WRITE OVMXPF \"SCSNODE = \"\"${NODE_UC}\"\"\""
send "WRITE OVMXPF \"SCSSYSTEMID = ${SYSID}\""
send "WRITE OVMXPF \"VAXCLUSTER = 2\""
send "WRITE OVMXPF \"NISCS_LOAD_PEA0 = 1\""
send "WRITE OVMXPF \"EXPECTED_VOTES = ${EXPECTED_VOTES}\""
send "WRITE OVMXPF \"VOTES = ${NODE_VOTES}\""
send "WRITE OVMXPF \"ALLOCLASS = ${ALLOCLASS}\""
send "CLOSE OVMXPF"

# --- 3. cluster authorisation (group + password) -> CLUSTER_AUTHORIZE.DAT ----------------------
#     VERIFY THIS STEP against the oracle on the first licensed run (see header NOTE).
send "MCR SYSMAN CONFIGURATION SET CLUSTER_AUTHORIZATION/GROUP_NUMBER=${GROUP_NUMBER}/PASSWORD=${GROUP_PASSWORD}"

# --- 4. AUTOGEN to fold the new params in, then reboot as a cluster member ----------------------
send "@SYS\$UPDATE:AUTOGEN GETDATA SETPARAMS"
waitfor 'AUTOGEN' 120 || true            # AUTOGEN is chatty; SETPARAMS finishes without rebooting
send "@SYS\$SYSTEM:SHUTDOWN"             # answer prompts below; REBOOT=YES so it re-forms
# SHUTDOWN.COM Q&A (defaults are usually fine; reboot on the way out):
send "0"                                  # minutes to shutdown
send ""                                   # reason
send "n"                                  # spin down disks? (no)
send "n"                                  # remove node from cluster permanently? (NO -- transient)
send "y"                                  # perform an automatic system reboot? (YES)

echo "clusterctl: ${NODE_UC} config submitted; it should reboot and come up as a cluster member."
echo "clusterctl: watch: kubectl -n ${NS} exec ${POD} -- grep -a 'Now a VMScluster member' ${CLOG}"
