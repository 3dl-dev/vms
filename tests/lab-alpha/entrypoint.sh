#!/bin/bash
# entrypoint.sh -- bring up one Alpha reference lab inside this pod (vms-e2c).
#
# ONE POD == ONE COMPLETE LAB, exactly as tests/lab/entrypoint.sh does for the
# VAX side. With NODES="alpha1 alpha2" the pod contains a whole two-node
# AlphaServer cluster whose LAN is a bridge in THIS POD'S netns, so the SCS
# multicast never reaches the cluster LAN, replicas cannot see each other, and
# every replica can reuse the same SCSSYSTEMIDs by design.
#
# WHAT IS PER-NODE AND WHAT IS SHARED -- get this wrong and nodes corrupt each
# other silently, because every pod mounts the SAME ReadWriteMany volume:
#
#   SHARED, read-only   $LAB_ROOT/alpha/rom/cl67srmrom.exe   SRM firmware
#                       $LAB_ROOT/alpha/media/*.ISO          install media
#                       $LAB_ROOT/alpha/disks/*.golden.img   golden system disk
#   PER-NODE, read-write $LAB_DIR/<node>/disks/*.img
#                        $LAB_DIR/<node>/rom/{decompressed,flash,dpr}.rom
#
# The ROM files are the trap. AXPbox WRITES decompressed.rom, flash.rom and
# dpr.rom. flash and dpr are the machine's NVRAM and TOY clock -- they hold
# bootdef_dev and the time, so they are kept across restarts on purpose. Wiping
# them leaves the clock cold and VMS stops at "Please enter date and time"
# instead of booting. Only cl67srmrom.exe is a read-only input.
set -euo pipefail

LAB_ROOT="${LAB_ROOT:-/lab}"
LAB_NAME="${POD_NAME:-alphalab-local}"
LAB_DIR="${LAB_ROOT}/k8s-labs/${LAB_NAME}"
ALPHA_SRC="${LAB_ROOT}/alpha"

NODES="${NODES:-alpha1}"
AXPBOX_VERSION="${AXPBOX_VERSION:-1.2.0}"
AXPBOX="/usr/local/bin/axpbox-${AXPBOX_VERSION}"

GOLDEN="${GOLDEN:-}"                # clone this instead of installing
MEDIA="${MEDIA:-}"                  # attach this ISO as a CD (install only)
SYSDISK_SIZE="${SYSDISK_SIZE:-9G}"
MEMORY_BITS="${MEMORY_BITS:-28}"    # 2^28 = 256 MB
CPU_SPEED="${CPU_SPEED:-800M}"
CONTROLLER="${CONTROLLER:-ide}"
NETWORK="${NETWORK:-auto}"          # auto = on when more than one node
BASE_PORT="${BASE_PORT:-21264}"

log() { echo "[alpha:${LAB_NAME}] $*"; }

[ -x "${AXPBOX}" ] || { echo "FATAL: no emulator at ${AXPBOX}" >&2; exit 1; }
[ -f "${ALPHA_SRC}/rom/cl67srmrom.exe" ] || { echo "FATAL: SRM firmware missing" >&2; exit 1; }

node_count=$(echo "${NODES}" | wc -w)
if [ "${NETWORK}" = "auto" ]; then
  [ "${node_count}" -gt 1 ] && NETWORK=on || NETWORK=off
fi

# --- 1. private L2 segment, entirely inside this pod's netns ------------------
# VETH PAIRS, NOT TAPS -- and the difference is not cosmetic. lab-2 gives SIMH
# a tap because SIMH opens the tap's CHARACTER DEVICE and becomes the endpoint,
# so what it writes appears as RX on the tap netdev and the bridge forwards it.
#
# AXPbox does not do that. It drives its emulated DE500 through libpcap, and
# pcap injection goes out the interface's TRANSMIT path. On a tap, transmit
# means "hand the frame to whatever has the character device open" -- which is
# nobody. Every frame the guest sends is therefore delivered to an unread fd and
# dropped, and NOTHING reaches br0. Measured: OpenVMS reported EWA0 link UP and
# sat at "waiting to form or join an OpenVMS Cluster" while tcpdump on both br0
# and tap1 captured exactly zero packets in either direction.
#
# A veth pair has a real peer, so injecting on veth<i> is received by vbr<i>,
# which IS on the bridge and forwards normally. Capture on veth<i> then sees
# what the bridge delivers back.
if [ "${NETWORK}" = "on" ]; then
  log "bringing up br0 + veth pairs for: ${NODES}"
  ip link add br0 type bridge 2>/dev/null || true
  ip link set br0 up
  i=0
  for n in ${NODES}; do
    i=$((i + 1))
    ip link del "veth${i}" 2>/dev/null || true
    ip link add "veth${i}" type veth peer name "vbr${i}"
    ip link set "vbr${i}" master br0
    ip link set "veth${i}" up
    ip link set "vbr${i}" up
  done
  ip -brief link show
fi

# --- 2. per-node provisioning -------------------------------------------------
i=0
for n in ${NODES}; do
  i=$((i + 1))
  nd="${LAB_DIR}/${n}"
  mkdir -p "${nd}/disks" "${nd}/rom" "${nd}/logs"
  cp -f "${ALPHA_SRC}/rom/cl67srmrom.exe" "${nd}/rom/cl67srmrom.exe"

  sysdisk="${nd}/disks/${n}-sys.img"
  if [ ! -f "${sysdisk}" ]; then
    if [ -n "${GOLDEN}" ] && [ -f "${ALPHA_SRC}/disks/${GOLDEN}" ]; then
      log "${n}: cloning golden system disk ${GOLDEN}"
      cp --sparse=always "${ALPHA_SRC}/disks/${GOLDEN}" "${sysdisk}"
    else
      log "${n}: creating blank ${SYSDISK_SIZE} system disk"
      truncate -s "${SYSDISK_SIZE}" "${sysdisk}"
    fi
  fi

  port=$((BASE_PORT + i - 1))
  # Locally-administered MAC, stable per node index so a capture is readable.
  mac=$(printf '08-00-2B-00-00-%02X' "${i}")

  # CONTROLLER PLACEMENT IS LOAD-BEARING -- three layouts were measured:
  #   ide, CD as SLAVE on the system disk's channel (disk0.1 -> DQA1): the SRM
  #     reads block 0, calls it a valid boot block, then fails the very next
  #     multi-block read. Never reaches VMS.
  #   scsi, sym53c895 (DKA0 + DKA600): media reads fine and OpenVMS 8.4 loads
  #     its executive, then bugchecks INVEXCEPTN before the installer.
  #   ide, CD as MASTER on the SECOND channel (disk1.0 -> DQB0): installs.
  # `icache` is deliberately absent: v1.2.0 rejects it as unrecognised.
  cfg="${nd}/${n}.cfg"
  {
    echo "// generated by entrypoint.sh for ${LAB_NAME}/${n} -- do not edit in place"
    echo "sys0 = tsunami"
    echo "{"
    echo "    memory.bits = ${MEMORY_BITS};"
    echo "    rom.srm          = \"${nd}/rom/cl67srmrom.exe\";"
    echo "    rom.decompressed = \"${nd}/rom/decompressed.rom\";"
    echo "    rom.flash        = \"${nd}/rom/flash.rom\";"
    echo "    rom.dpr          = \"${nd}/rom/dpr.rom\";"
    echo "    cpu0 = ev68cb { speed = ${CPU_SPEED}; }"
    echo "    serial0 = serial { port = ${port}; action = \"\"; }"
    if [ "${CONTROLLER}" = "scsi" ]; then
      echo "    pci0.1 = sym53c895"
      echo "    {"
      echo "        disk0.0 = file { file = \"${sysdisk}\"; cdrom = false; read_only = false; }"
      [ -n "${MEDIA}" ] && [ -f "${ALPHA_SRC}/media/${MEDIA}" ] && \
        echo "        disk0.6 = file { file = \"${ALPHA_SRC}/media/${MEDIA}\"; cdrom = true; read_only = true; }"
      echo "    }"
    else
      echo "    pci0.15 = ali_ide"
      echo "    {"
      echo "        disk0.0 = file { file = \"${sysdisk}\"; cdrom = false; read_only = false; }"
      # SECOND channel, master. Not disk0.1 -- see the note above.
      [ -n "${MEDIA}" ] && [ -f "${ALPHA_SRC}/media/${MEDIA}" ] && \
        echo "        disk1.0 = file { file = \"${ALPHA_SRC}/media/${MEDIA}\"; cdrom = true; read_only = true; }"
      echo "    }"
    fi
    if [ "${NETWORK}" = "on" ]; then
      echo "    pci0.3 = dec21143 { adapter = \"veth${i}\"; mac = \"${mac}\"; }"
    fi
    echo "    pci0.7 = ali { mouse.enabled = false; vga_console = false; }"
    echo "    pci0.19 = ali_usb { }"
    echo "}"
  } > "${cfg}"

  log "${n}: console :${port}  disk ${sysdisk##*/}  $([ "${NETWORK}" = on ] && echo "nic veth${i}/${mac}" || echo "no nic")"
done

# --- 3. boot every node -------------------------------------------------------
AUTOBOOT="${AUTOBOOT:-}"
if [ -z "${AUTOBOOT}" ] && [ -n "${GOLDEN}" ]; then
  AUTOBOOT=$([ "${CONTROLLER}" = "scsi" ] && echo "boot dka0" || echo "boot dqa0")
fi

declare -a PIDS=()
i=0
for n in ${NODES}; do
  i=$((i + 1))
  nd="${LAB_DIR}/${n}"
  port=$((BASE_PORT + i - 1))
  clog="${nd}/logs/${n}.log"
  fifo="${nd}/logs/${n}.log.in"
  # Create the log BEFORE the tail below globs for it, as lab-2 does, or
  # 'kubectl logs' shows no console output at all.
  : > "${clog}"

  "${AXPBOX}" run "${nd}/${n}.cfg" > "${nd}/logs/emulator.log" 2>&1 &
  PIDS+=($!)

  # DO NOT ADD A TCP READINESS PROBE HERE. The obvious one -- open a connection
  # to the console port to see whether it is listening -- is itself a console
  # client connecting and disconnecting, and AXPbox EXITS on that. It powers the
  # machine off a second before the real console attaches, and the symptom is an
  # emulator that "died instantly" with an empty log. srmdrv.py retries the
  # connect internally (-C), so the first connection ever made is the pump's.
  #
  # The pump must NEVER exit: dropping it powers the machine off.
  python3 /usr/local/bin/srmdrv.py -t 0 -C 90 -p "${port}" -f "${fifo}" -l "${clog}" \
      > "${nd}/logs/pump.log" 2>&1 &
  PIDS+=($!)

  # Per-node bring-up watcher: SRM prompt -> optional autoboot -> cold-clock
  # date prompt -> login prompt.
  ( for _ in $(seq 1 180); do
      grep -aq 'P00>>>' "${clog}" 2>/dev/null || { sleep 1; continue; }
      echo "[alpha:${LAB_NAME}] ${n}: SRM console ready"
      [ -n "${AUTOBOOT}" ] || exit 0
      echo "[alpha:${LAB_NAME}] ${n}: autoboot ${AUTOBOOT}"
      printf '%s\n' "${AUTOBOOT}" > "${fifo}"
      for _ in $(seq 1 120); do
        grep -aq 'Username:' "${clog}" 2>/dev/null && break
        if tail -c 400 "${clog}" | grep -aq 'enter date and time'; then
          echo "[alpha:${LAB_NAME}] ${n}: setting VMS clock from the host"
          printf '%s\n' "$(date -u '+%d-%b-%Y %H:%M' | tr '[:lower:]' '[:upper:]')" > "${fifo}"
          break
        fi
        sleep 2
      done
      for _ in $(seq 1 300); do
        if grep -aq 'Username:' "${clog}" 2>/dev/null; then
          echo "[alpha:${LAB_NAME}] ${n}: OpenVMS is up -- login prompt reached"
          exit 0
        fi
        sleep 2
      done
      echo "[alpha:${LAB_NAME}] *** ${n}: booted SRM but OpenVMS never reached a login prompt"
      exit 0
    done
    echo "[alpha:${LAB_NAME}] *** ${n}: never reached the P00>>> prompt -- do NOT use this lab as an oracle" ) &
done

cat <<EOF
[alpha:${LAB_NAME}] up. emulator: $(head -1 /etc/axpbox-version.txt)
  version     ${AXPBOX_VERSION}
  nodes       ${NODES}
  lab dir     ${LAB_DIR}
  network     ${NETWORK}
  golden      ${GOLDEN:-<none, blank disks>}
  media       ${MEDIA:-<none>}
  console in  printf 'SHOW SYSTEM\\n' > ${LAB_DIR}/<node>/logs/<node>.log.in
  console out tail -f ${LAB_DIR}/<node>/logs/<node>.log
  capture     tcpdump -i br0 -w ${LAB_DIR}/logs/<name>.pcap -U -s 0
  NOTE login is prompt-synchronised: wait for 'Username:', send SYSTEM, wait
       for 'Password:', send it. Batched sends race the boot chatter and fail
       as %LOGIN-F-CMDINPUT, which is never actually a bad password.
EOF

tail -n +1 -F "${LAB_DIR}"/*/logs/*.log &

# Keep PID 1 alive while the emulators run. Polling rather than `wait -n <pid>`:
# if a child has already exited and been reaped, `wait -n` fails with "no such
# job" and, under `set -e`, that kills PID 1 and turns a working lab into a
# CrashLoop.
while :; do
  for p in "${PIDS[@]}"; do
    kill -0 "${p}" 2>/dev/null || { log "*** a node emulator or console pump exited (pid ${p})"; sleep 2; exit 1; }
  done
  sleep 5
done
