#!/bin/bash
# entrypoint.sh — bring up one complete, isolated VMScluster reference lab
# inside this pod's own network namespace (vms-a5c).
#
# ONE POD == ONE LAB. The bridge and taps are created in the pod netns, so the
# 0x6007 LAVC/SCA HELLO multicast never reaches the cluster LAN and replicas
# cannot see each other. That is why every replica can reuse the SAME
# SCSSYSTEMIDs (1025/1026/1027) and the same node MACs without colliding.
#
# Requires: CAP_NET_ADMIN and /dev/net/tun. Not privileged.
set -euo pipefail

LAB_ROOT="${LAB_ROOT:-/lab}"                  # the tank vax volume, read-write
LAB_NAME="${POD_NAME:-vaxlab-local}"
LAB_DIR="${LAB_ROOT}/k8s-labs/${LAB_NAME}"
GOLDEN="${GOLDEN_SUFFIX:-.3node-golden.bak}"  # restore point to clone from
NODES="${NODES:-vax1 vax2}"                   # add vax3 for the diskless satellite
VMS_DATE="${VMS_DATE:-$(date '+%d-%b-%Y %H:%M' | tr '[:lower:]' '[:upper:]')}"
SRC="${LAB_ROOT}/cluster"

log() { echo "[lab:${LAB_NAME}] $*"; }

# --- per-node parameters, mirroring lab-1's cluster/vax{1,2,3}/local.ini -------
# node   tap    dz port  boot command
node_tap()  { case "$1" in vax1) echo tap1;; vax2) echo tap2;; vax3) echo tap3;; esac; }
node_dz()   { case "$1" in vax1) echo 2001;; vax2) echo 2002;; vax3) echo 2003;; esac; }
node_boot() {
  case "$1" in
    vax1) echo "B DUA0" ;;                  # SYS0
    vax2) echo "B/R5:10000000 DUA0" ;;      # SYS1 — R5 selects the system root
    vax3) echo "B XQA0" ;;                  # diskless satellite, boots over the wire
  esac
}

# --- 1. provision this replica's own disks ------------------------------------
# Sparse copies: d0/d1 are 1.5G apparent but ~108M/11M real, and the dataset is
# zstd on top of that. Never point two labs at one d0.dsk.
mkdir -p "${LAB_DIR}/data"
for disk in d0 d1; do
  dst="${LAB_DIR}/data/${disk}.dsk"
  if [ ! -f "${dst}" ]; then
    src="${SRC}/data/${disk}.dsk${GOLDEN}"
    [ -f "${src}" ] || { echo "FATAL: golden image missing: ${src}" >&2; exit 1; }
    log "cloning ${src##*/} -> ${dst##*/}"
    cp --sparse=always "${src}" "${dst}"
  fi
done

# --- 2. private L2 segment, entirely inside this pod's netns ------------------
log "bringing up br0 (10.2.1.254/24) + taps"
ip link add br0 type bridge 2>/dev/null || true
ip addr add 10.2.1.254/24 dev br0 2>/dev/null || true
ip link set br0 up
for n in ${NODES}; do
  tap="$(node_tap "${n}")"
  ip tuntap add dev "${tap}" mode tap 2>/dev/null || true
  ip link set "${tap}" master br0
  ip link set "${tap}" up
done
ip -brief link show

# --- 3. per-node SIMH working directory ---------------------------------------
for n in ${NODES}; do
  nd="${LAB_DIR}/${n}"
  mkdir -p "${nd}/data"
  # Each node needs its OWN nvram. Seed from lab-1's copy of that node's nvram
  # when it exists, else the shared one.
  if [ ! -f "${nd}/data/nvram.bin" ]; then
    if [ -f "${SRC}/${n}/data/nvram.bin" ]; then cp "${SRC}/${n}/data/nvram.bin" "${nd}/data/nvram.bin"
    else cp "${SRC}/data/nvram.bin" "${nd}/data/nvram.bin"; fi
  fi
  ln -sf /usr/local/bin/vax "${nd}/vax"

  # vax.ini with ABSOLUTE disk paths — lab-1's uses relative ../data paths that
  # only resolve inside its own tree.
  #   set idle=vms is load-bearing: without it SIMH spins a core forever on the
  #   VMS idle loop and the pod pegs its CPU limit doing nothing.
  cat > "${nd}/vax.ini" <<EOF
attach nvr ${nd}/data/nvram.bin
set cpu conhalt
set cpu 128m
set idle=vms

set rl disable

set rq enable
set rqb disable
set rqc disable
set rqd disable

set rq0 ra92
set rq1 ra92
set rq2 cdrom
set rq3 cdrom

attach rq0 ${LAB_DIR}/data/d0.dsk
attach rq1 ${LAB_DIR}/data/d1.dsk
attach -r rq2 ${LAB_ROOT}/media/openvms073.iso
attach -r rq3 ${LAB_ROOT}/media/openvms-internet-product-suite-v11.iso

do local.ini

b
EOF

  # local.ini: NIC + console mux. 'dep bdr 0' stays COMMENTED on every node —
  # depositing 0 into the KA655 Boot/Diagnostic Register forces unattended
  # autoboot, the node never drops to '>>>', and it boots whatever root nvram
  # defaults to. That is the 2026-07-28 duplicate-VAX1 incident (vms-d0f); the
  # console driver below is what picks each node's root instead.
  cat > "${nd}/local.ini" <<EOF
set xq enable
at xq tap:$(node_tap "${n}")

att dz $(node_dz "${n}"),speed=*32
; autoboot
;dep bdr 0
EOF
done

# --- 4. boot ------------------------------------------------------------------
mkdir -p "${LAB_DIR}/logs"
for n in ${NODES}; do
  logf="${LAB_DIR}/logs/${n}.log"
  log "booting ${n} (console FIFO ${logf}.in, DZ mux :$(node_dz "${n}"))"
  python3 /usr/local/bin/nodedrv.py "${LAB_DIR}/${n}" "${logf}" \
      --date "${VMS_DATE}" --boot "$(node_boot "${n}")" &
done

cat <<EOF
[lab:${LAB_NAME}] up. SIMH: $(cat /etc/simh-version.txt 2>/dev/null | head -1)
  lab dir     ${LAB_DIR}
  console in  printf 'SHOW CLUSTER\\r' > ${LAB_DIR}/logs/<node>.log.in
  console out tail -f ${LAB_DIR}/logs/<node>.log
  capture     tcpdump -i br0 -w ${LAB_DIR}/logs/<name>.pcap -U -s 0
  NOTE login is prompt-synchronised: wait for 'Username:', send SYSTEM, wait
       for 'Password:', send system. Batched sends race the boot chatter and
       fail as %LOGIN-F-INVPWD, which is never actually a bad password.
EOF

# Tail the consoles to stdout so 'kubectl logs' shows boot progress, and keep
# PID 1 alive for as long as the emulators run.
tail -n +1 -F "${LAB_DIR}"/logs/*.log &
wait -n
log "an emulator exited — see the logs above"
wait
