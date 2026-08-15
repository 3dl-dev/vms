#!/bin/bash
# run-rawdisk.sh - vms-d5d de-risk proof: can NetBSD/vax USERSPACE open(2) +
# pread(2) raw ODS-2 blocks off an MSCP disk, and off which device node
# (block "/dev/ra1c" vs raw-char "/dev/rra1c")? De-risks the vms-5eb atomic
# flip's A1 assumption (userspace ods2_bdev reads SYS$DISK the same way on
# vax as it does over Linux's /dev/vda).
#
# REUSES tests/lab-vax's proven substrate WITHOUT rebuilding it: this wrapper
# does NOT install NetBSD, does NOT build the MODULAR kernel, and does NOT
# cross-build vmsfs.kmod/vmsfs_mount -- run-vmsfs.sh already produces all of
# that (cached under .boot-cache/lab-vax/{anita-work,vmsfs-artifacts,
# ovmx-ods2-vax.img}), and this script's job is ONLY to cross-build the new
# vms_rawpread probe (tests/netbsd/guest/vms_rawpread.c) and boot the already-
# proven disk once more to run it.
#
#   tests/lab-vax/run-rawdisk.sh          # cross-build probe + boot + PROVE
#
# Prerequisite (one-time, or copied from another cache -- see the vms-d5d rd
# item / handoff notes for how this worktree's proof copied a conductor cache
# without touching the live one): CACHE_DIR must already contain
#   anita-work/wd0.img              (installed NetBSD/vax, MODULAR kernel in)
#   vmsfs-artifacts/{vmsfs.kmod,vmsfs_mount,netbsd-OVMX}
#   ovmx-ods2-vax.img                (mastered ODS-2 test volume)
# If any are missing, run tests/lab-vax/run-vmsfs.sh first (it produces all
# three) -- this script deliberately does not re-derive them.
#
# HARD timeout on the one SIMH invocation; container force-killed on timeout
# (a prior unbounded SIMH proof ran 1h43m -- CLAUDE.md memory note).
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"

# shellcheck source=tests/lab-vax/negctl_gate.sh
source "${HERE}/negctl_gate.sh"

NETBSD_VERSION="${NETBSD_VERSION:-10.1}"
SETS="${SETS:-kern-GENERIC,base,etc}"

CACHE_DIR="${CACHE_DIR:-${REPO}/.boot-cache/lab-vax}"
ARTIFACTS_SRC="${CACHE_DIR}/vmsfs-artifacts"       # vmsfs.kmod + vmsfs_mount (from run-vmsfs.sh)
RAWDISK_ARTIFACTS="${CACHE_DIR}/rawdisk-artifacts"  # + vms_rawpread (built here)
ODS2_IMG="${ODS2_IMG:-${CACHE_DIR}/ovmx-ods2-vax.img}"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

SESSION_TIMEOUT="${SESSION_TIMEOUT:-2700}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

log() { echo "[run-rawdisk] $*"; }
die() { echo "[run-rawdisk] FATAL: $*" >&2; exit 1; }

ensure_images() {
  docker image inspect "${CROSS_IMAGE}" >/dev/null 2>&1 || die "${CROSS_IMAGE} not built -- run tests/lab-vax/run-vmsfs.sh once first"
  docker image inspect "${LAB_IMAGE}" >/dev/null 2>&1 || die "${LAB_IMAGE} not built -- run tests/lab-vax/run-vmsfs.sh once first"
}

ensure_prereqs() {
  [ -f "${CACHE_DIR}/anita-work/wd0.img" ] || die "no cached disk at ${CACHE_DIR}/anita-work/wd0.img -- run tests/lab-vax/run-vmsfs.sh first (or copy its cache) to produce the installed+MODULAR-kernel disk"
  if [ ! -f "${ARTIFACTS_SRC}/vmsfs.kmod" ] || [ ! -f "${ARTIFACTS_SRC}/vmsfs_mount" ]; then
    die "vmsfs.kmod/vmsfs_mount missing under ${ARTIFACTS_SRC} -- run tests/lab-vax/run-vmsfs.sh first"
  fi
  [ -f "${ODS2_IMG}" ] || die "mastered ODS-2 volume missing at ${ODS2_IMG} -- run tests/lab-vax/run-vmsfs.sh first"
}

# cross-build ONLY the new probe; reuse vmsfs.kmod/vmsfs_mount unmodified.
build_probe() {
  mkdir -p "${RAWDISK_ARTIFACTS}"
  cp -f "${ARTIFACTS_SRC}/vmsfs.kmod" "${RAWDISK_ARTIFACTS}/vmsfs.kmod"
  cp -f "${ARTIFACTS_SRC}/vmsfs_mount" "${RAWDISK_ARTIFACTS}/vmsfs_mount"
  chmod +x "${RAWDISK_ARTIFACTS}/vmsfs_mount"
  log "cross-building vms_rawpread (static elf32-vax)"
  docker run --rm -v "${REPO}:/src:ro" -v "${RAWDISK_ARTIFACTS}:/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c \
    'vax--netbsdelf-gcc -O -Wall -Wextra -static -o /out/vms_rawpread /src/tests/netbsd/guest/vms_rawpread.c && \
     vax--netbsdelf-objdump -f /out/vms_rawpread | grep -iF "file format elf32-vax"'
  [ -x "${RAWDISK_ARTIFACTS}/vms_rawpread" ] || die "vms_rawpread cross-build did not produce a binary"
}

run_session() {
  local cid="ovmx-rawdisk-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_ARTIFACTS=/artifacts -e OVMX_NETBSD_DIR=/netbsd \
      -e OVMX_ODS2_IMG=/cache/"$(basename "${ODS2_IMG}")" \
      -v "${CACHE_DIR}:/cache" -v "${RAWDISK_ARTIFACTS}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_rawdisk_vax.py
  rc=$?; set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  return "${rc}"
}

ensure_images
ensure_prereqs
build_probe

rc=0
run_session || rc=$?
if vaxharness_negctl_gate "${rc}" 0; then
  log "======================================================================"
  log "  RAWDISK-VAX PASSED: raw open()+pread() reached a validated ODS-2"
  log "  home block from NetBSD/vax userspace (see the FINDING lines in the"
  log "  console log above for WHICH device node / mount state)"
  log "======================================================================"
  exit 0
fi
die "RAWDISK-VAX did not validate a raw pread on either device node (driver exit=${rc}); see console output above"
