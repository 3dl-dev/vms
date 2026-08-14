#!/bin/bash
# run-boot.sh - orchestrate the vms-7b1 proof: ASSEMBLE a bootable OVMX/NetBSD-vax
# system disk from the OVMX build and boot it UNATTENDED under SIMH with ovmx_init
# (STARTUP.EXE) as PID 1 (init), reaching the boot milestone
#   kernel up -> vms.kmod loaded (/dev/vms live) -> OpenVMX banner -> vmsfs.kmod
#   loaded -> OVMX ODS-2 system disk (DKA0:) mounted.
# Reaching a full DCL prompt is the SEPARATE capstone (vms-d59); this stops at the
# banner+mount milestone (epic vms-8e8, parent vms-c99).
#
# The capstone-minus-one of the vax lab: it wires together what its two peers prove
# in isolation -- run-devvms.sh (/dev/vms PING) and run-vmsfs.sh (ODS-2 mount+read)
# -- but UNDER ovmx_init running as real PID 1. It reuses the SAME vax runtime
# substrate: the custom GENERIC+MODULAR /netbsd kernel (vax GENERIC omits
# `options MODULAR'), the loadable elf32-vax vms.kmod + vmsfs.kmod, and a mastered
# OVMX ODS-2 volume; and it links STARTUP.EXE (ovmx_init) for elf32-vax.
#
# ISOLATION FROM THE SHARED DISK. The devvms/vmsfs proofs boot the SHARED cached
# wd0.img and expect NetBSD's own init (a single-user shell). This proof REPLACES
# /sbin/init with ovmx_init, which would break them -- so it works on a private
# COPY of the cached disk (boot-work/wd0.img), never the shared one. The shared
# disk is only ever READ (copied) and, on a cold cache, kernel-swapped the same
# way the siblings swap it (shared KERNEL_MARKER).
#
# STAGES (each SIMH invocation wrapped in a HARD `timeout'; container force-killed
# on timeout -- a prior emulator proof in this repo once ran unbounded 1h43m):
#   1. CROSS-BUILD (ovmx-cross-vax): STARTUP.EXE (ovmx_init, dynamic elf32-vax,
#      ld.elf_so activation, Decision A) + loadable vms.kmod + vmsfs.kmod.
#   2. BUILD-KERNEL (ovmx-cross-vax): the GENERIC+MODULAR vax kernel (cached).
#   3. MASTER (ovmx-cross-vax): a small OVMX ODS-2 volume (tests/qemu/
#      mkimage_vmsfs.c; host cc; arch-independent). Cached.
#   4. INSTALL (ovmx-vax-lab): install NetBSD/vax once if the shared cache is cold.
#   5. INSTALL-KERNEL (ovmx-vax-lab): swap the MODULAR kernel onto the SHARED disk
#      once (shared marker; the siblings may have done it already).
#   6. COPY (host): clone the shared (MODULAR-kernel) disk to boot-work/wd0.img.
#   7. INSTALL-BOOT (ovmx-vax-lab): boot the COPY single-user, install STARTUP.EXE
#      as /sbin/init, place the modules in the module_path, pre-create /dev/vms +
#      the ra1 (DKA0:) node + the boot mount points. Once (marker).
#   8. PROVE (ovmx-vax-lab): boot the assembled COPY; ovmx_init runs as PID 1 with
#      the ODS-2 volume on rq1 -> ra1 -> DKA0:; assert the milestone lines; plus
#      the negctl (no ODS-2 volume -> the MOUNTED milestone must NOT appear).
#
# Nothing installed on the host (Rule 9). Reuses the cached disk; never reinstalls.
#
#   tests/lab-vax/run-boot.sh              # build all, assemble, PROVE
#   tests/lab-vax/run-boot.sh negctl       # teeth: boot without the ODS-2 volume
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
MODE="${1:-prove}"

NETBSD_VERSION="${NETBSD_VERSION:-10.1}"
ISO_NAME="NetBSD-${NETBSD_VERSION}-vax.iso"
ISO_SHA512="${ISO_SHA512:-aa763aa2240e4623adf09dd1a1ed2da0e3b96959d33544d52026a0c7c7448c6f0da8517bf059b9c53a9786782c0373b2e3da84de4b36cc5aeb669d219ac0f225}"
SETS="${SETS:-kern-GENERIC,base,etc}"
SRC_BASE="${SRC_BASE:-https://cdn.netbsd.org/pub/NetBSD/NetBSD-${NETBSD_VERSION}/source/sets}"
SRC_SHA512="${SRC_SHA512:-6ae2053b4b75821238c0757d4f7258daece425de72524c616e07d3adee7c48d87422dd47d852a137918cec3dd3c0d339e372f4504dfe9f1bc5520011775bdb86}"
GNUSRC_SHA512="${GNUSRC_SHA512:-8a1c42030ba44eb2a0c7a5111187bc02e8f4d0860d8491b7863579e612333665c478625c37b01f08732e3cfd29ec31335f1db1274fd7dcfdc048b09d1b4bbb83}"
SHARESRC_SHA512="${SHARESRC_SHA512:-703eeb306fc0328cad7e6f0e100d2e7af194f82e613338f4611a7bcd5f6d773d8789e7ce03ec25268ec2b95ccdb97c3b4289a838a629716498b4d7c3184cb1ef}"
SYSSRC_SHA512="${SYSSRC_SHA512:-766ac21f33cfe0e701dfedb894fa07f36d811da1a12e979181e8fca7af4e627852680ce42a7b29e97dd3e2e402ddf9ae7bfba60c8d7dc6b8a3354d8ce8c06926}"

CACHE_DIR="${CACHE_DIR:-${REPO}/.boot-cache/lab-vax}"
ARTIFACTS_DIR="${CACHE_DIR}/boot-artifacts"      # STARTUP.EXE + vms.kmod + vmsfs.kmod + netbsd-OVMX
ODS2_IMG="${ODS2_IMG:-${CACHE_DIR}/ovmx-ods2-vax.img}"
KBUILD_DIR="${KBUILD_DIR:-${CACHE_DIR}/kbuild}"
NBSRC_DIR="${NBSRC_DIR:-${KBUILD_DIR}/nbsrc}"
KERNEL_MARKER="${CACHE_DIR}/.ovmx-modular-kernel-installed"   # SHARED with siblings
BOOT_WORKDIR="${CACHE_DIR}/boot-work"
BOOT_COPY_MARKER="${CACHE_DIR}/.boot-disk-copied"
BOOT_INSTALL_MARKER="${CACHE_DIR}/.ovmx-init-installed"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-5400}"
KBUILD_TIMEOUT="${KBUILD_TIMEOUT:-5400}"
SESSION_TIMEOUT="${SESSION_TIMEOUT:-2700}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

log() { echo "[run-boot] $*"; }
die() { echo "[run-boot] FATAL: $*" >&2; exit 1; }

ensure_images() {
  docker image inspect "${CROSS_IMAGE}" >/dev/null 2>&1 || {
    log "building ${CROSS_IMAGE}"
    docker build -f "${REPO}/tools/cross-vax/Dockerfile" -t "${CROSS_IMAGE}" "${REPO}/tools/cross-vax"; }
  docker image inspect "${LAB_IMAGE}" >/dev/null 2>&1 || {
    log "building ${LAB_IMAGE}"
    docker build -f "${HERE}/Dockerfile" -t "${LAB_IMAGE}" "${HERE}"; }
}

fetch_set() {
  local name="$1" sha="$2" tgz="${KBUILD_DIR}/${name}.tgz"
  if [ ! -f "${tgz}" ] || ! echo "${sha}  ${tgz}" | sha512sum -c --status -; then
    log "downloading ${name}.tgz"
    curl -fSL --retry 3 -o "${tgz}.part" "${SRC_BASE}/${name}.tgz"
    echo "${sha}  ${tgz}.part" | sha512sum -c --status - || { rm -f "${tgz}.part"; die "${name}.tgz checksum mismatch"; }
    mv "${tgz}.part" "${tgz}"
  fi
  tar -C "${NBSRC_DIR}" -xzf "${tgz}"
}

ensure_src() {
  if [ -f "${NBSRC_DIR}/usr/src/build.sh" ] && [ -f "${NBSRC_DIR}/usr/src/sys/sys/param.h" ]; then
    log "full NetBSD src tree present"; return 0; fi
  mkdir -p "${NBSRC_DIR}"
  log "fetching + extracting the full NetBSD ${NETBSD_VERSION} src tree"
  fetch_set src "${SRC_SHA512}"
  fetch_set gnusrc "${GNUSRC_SHA512}"
  fetch_set sharesrc "${SHARESRC_SHA512}"
  fetch_set syssrc "${SYSSRC_SHA512}"
  [ -f "${NBSRC_DIR}/usr/src/build.sh" ] || die "src tree extract incomplete"
}

# 1. cross-build STARTUP.EXE + vms.kmod + vmsfs.kmod into ARTIFACTS_DIR (each into
#    its own scratch subdir; only the delivered artifact is copied up, so the CD
#    carries just the four boot deliverables).
cross_build() {
  if [ "${FORCE_CROSS_BUILD:-0}" != "1" ] \
     && [ -f "${ARTIFACTS_DIR}/STARTUP.EXE" ] \
     && [ -f "${ARTIFACTS_DIR}/vms.kmod" ] \
     && [ -f "${ARTIFACTS_DIR}/vmsfs.kmod" ]; then
    log "boot artifacts present -- NOT rebuilding (set FORCE_CROSS_BUILD=1 to force)"; return 0; fi
  ensure_src
  mkdir -p "${ARTIFACTS_DIR}"
  # Each cross-build runs into the container's OWN /tmp (never the mounted
  # cache), so no root-owned scratch dirs land in ARTIFACTS_DIR; only the final
  # deliverable is copied to /out.
  log "cross-building STARTUP.EXE (ovmx_init, PID 1) for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src -v "${ARTIFACTS_DIR}:/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c \
    'OUT=/tmp/build-startup sh tools/cross-vax/build-ovmx-init-vax.sh && cp /tmp/build-startup/STARTUP.EXE /out/STARTUP.EXE'
  log "cross-building the loadable vms.kmod for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src -v "${NBSRC_DIR}:/nbsrc:ro" \
    -v "${ARTIFACTS_DIR}:/out" --entrypoint sh "${CROSS_IMAGE}" -c \
    'OUT=/tmp/build-devvms sh tools/cross-vax/build-devvms-vax.sh && cp /tmp/build-devvms/vms.kmod /out/vms.kmod'
  log "cross-building the loadable vmsfs.kmod for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src -v "${NBSRC_DIR}:/nbsrc:ro" \
    -v "${ARTIFACTS_DIR}:/out" --entrypoint sh "${CROSS_IMAGE}" -c \
    'OUT=/tmp/build-vmsfs sh tools/cross-vax/build-vmsfs-mount-vax.sh && cp /tmp/build-vmsfs/vmsfs.kmod /out/vmsfs.kmod'
  [ -f "${ARTIFACTS_DIR}/STARTUP.EXE" ] && [ -f "${ARTIFACTS_DIR}/vms.kmod" ] \
    && [ -f "${ARTIFACTS_DIR}/vmsfs.kmod" ] || die "cross-build missing artifacts"
}

# 2. build (or reuse) the custom MODULAR kernel, cached as ARTIFACTS_DIR/netbsd-OVMX.
build_kernel() {
  if [ -f "${ARTIFACTS_DIR}/netbsd-OVMX" ]; then
    log "MODULAR kernel artifact present -- NOT rebuilding"; return 0; fi
  # Reuse a sibling's already-built kernel if present (same build script/pin).
  for sib in "${CACHE_DIR}/vmsfs-artifacts/netbsd-OVMX" "${CACHE_DIR}/devvms-artifacts/netbsd-OVMX"; do
    if [ -f "${sib}" ]; then
      log "reusing sibling MODULAR kernel: ${sib}"
      mkdir -p "${ARTIFACTS_DIR}"; cp "${sib}" "${ARTIFACTS_DIR}/netbsd-OVMX"; return 0; fi
  done
  ensure_src
  mkdir -p "${KBUILD_DIR}/obj" "${KBUILD_DIR}/tools" "${KBUILD_DIR}/dest" "${ARTIFACTS_DIR}"
  log "building the GENERIC+MODULAR NetBSD/vax kernel (build.sh; hard cap ${KBUILD_TIMEOUT}s)"
  local cid="ovmx-boot-kbuild-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${KBUILD_TIMEOUT}" \
    docker run --rm --name "${cid}" \
      -v "${REPO}:/src" -w /src \
      -v "${NBSRC_DIR}:/nbsrc" -v "${KBUILD_DIR}/obj:/obj" -v "${KBUILD_DIR}/tools:/tools" \
      -v "${KBUILD_DIR}/dest:/dest" -v "${ARTIFACTS_DIR}:/out" \
      --entrypoint bash "${CROSS_IMAGE}" tools/cross-vax/build-vax-modular-kernel.sh
  rc=$?; set -e
  [ "${rc}" -eq 0 ] || { docker kill "${cid}" >/dev/null 2>&1 || true; die "kernel build failed/timed out (rc=${rc})"; }
  [ -f "${ARTIFACTS_DIR}/netbsd-OVMX" ] || die "kernel build finished but netbsd-OVMX missing"
}

# 3. master a small OVMX ODS-2 volume (host cc; arch-independent). Cached/shared.
master_volume() {
  if [ -f "${ODS2_IMG}" ]; then
    log "mastered ODS-2 volume present -- NOT re-mastering"; return 0; fi
  log "mastering a small OVMX ODS-2 volume (tests/qemu/mkimage_vmsfs.c)"
  docker run --rm -v "${REPO}:/src:ro" -v "$(dirname "${ODS2_IMG}"):/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c \
    "cc -O2 -Wall -I /src/src/kernel/vmsfs -o /tmp/mkimage_vmsfs /src/tests/qemu/mkimage_vmsfs.c && /tmp/mkimage_vmsfs /out/$(basename "${ODS2_IMG}")"
  [ -f "${ODS2_IMG}" ] || die "ODS-2 mastering did not produce ${ODS2_IMG}"
}

# 4. ensure the shared NetBSD/vax disk (install once).
ensure_disk() {
  if [ -f "${CACHE_DIR}/anita-work/wd0.img" ]; then
    log "cached NetBSD/vax disk present -- NOT reinstalling"; return 0; fi
  log "cached disk absent -- one-time lab-vax install (SLOW, hard cap ${INSTALL_TIMEOUT}s)"
  local cid="ovmx-boot-install-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${INSTALL_TIMEOUT}" \
    docker run --rm --name "${cid}" \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e ISO_SHA512="${ISO_SHA512}" \
      -e "SETS=${SETS}" -e INSTALL_TIMEOUT="${INSTALL_TIMEOUT}" \
      -v "${CACHE_DIR}:/cache" "${LAB_IMAGE}" install
  rc=$?; set -e
  [ "${rc}" -eq 0 ] || { docker kill "${cid}" >/dev/null 2>&1 || true; die "install failed/timed out (rc=${rc})"; }
  [ -f "${CACHE_DIR}/anita-work/wd0.img" ] || die "install finished but wd0.img missing"
}

# run drive_boot_vax.py in one SIMH session; args: <ovmx_mode> <workdir> [extra -e ...]
run_session() {
  local ovmx_mode="$1" workdir="$2"; shift 2
  local cid="ovmx-boot-${ovmx_mode}-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_MODE="${ovmx_mode}" -e OVMX_ARTIFACTS=/artifacts -e OVMX_NETBSD_DIR=/netbsd \
      -e NETBSD_WORKDIR="${workdir}" -e OVMX_ODS2_IMG=/cache/"$(basename "${ODS2_IMG}")" \
      "$@" \
      -v "${CACHE_DIR}:/cache" -v "${ARTIFACTS_DIR}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_boot_vax.py
  rc=$?; set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  return "${rc}"
}

# 5. swap the MODULAR kernel onto the SHARED disk (shared marker).
ensure_modular_kernel() {
  if [ -f "${KERNEL_MARKER}" ]; then
    log "MODULAR kernel already installed on the shared disk -- skipping"; return 0; fi
  log "installing the MODULAR kernel onto the shared disk (boot GENERIC single-user)"
  run_session install-kernel /cache/anita-work || die "MODULAR-kernel install session failed"
  touch "${KERNEL_MARKER}"
}

# 6. clone the shared (MODULAR-kernel) disk to an ISOLATED boot-work copy.
make_boot_copy() {
  if [ -f "${BOOT_COPY_MARKER}" ] && [ -f "${BOOT_WORKDIR}/wd0.img" ]; then
    log "isolated boot-disk copy present -- NOT re-copying"; return 0; fi
  [ -f "${CACHE_DIR}/anita-work/wd0.img" ] || die "shared disk missing; cannot copy"
  mkdir -p "${BOOT_WORKDIR}"
  log "cloning the shared MODULAR-kernel disk -> ${BOOT_WORKDIR}/wd0.img"
  cp "${CACHE_DIR}/anita-work/wd0.img" "${BOOT_WORKDIR}/wd0.img.part"
  mv "${BOOT_WORKDIR}/wd0.img.part" "${BOOT_WORKDIR}/wd0.img"
  touch "${BOOT_COPY_MARKER}"
}

# 7. assemble ovmx_init + modules + nodes onto the boot-work copy (once).
ensure_boot_install() {
  if [ -f "${BOOT_INSTALL_MARKER}" ]; then
    log "boot disk already assembled (ovmx_init installed) -- skipping"; return 0; fi
  log "assembling the bootable disk (install STARTUP.EXE as /sbin/init, modules, nodes)"
  run_session install-boot /cache/boot-work || die "boot-disk assembly session failed"
  touch "${BOOT_INSTALL_MARKER}"
}

ensure_images
cross_build
build_kernel
master_volume
ensure_disk
ensure_modular_kernel
make_boot_copy
ensure_boot_install

case "${MODE}" in
  prove)
    if run_session prove /cache/boot-work; then
      log "======================================================================"
      log "  BOOT-VAX PASSED: ovmx_init boots as PID 1 on NetBSD/vax under SIMH"
      log "  (executive on /dev/vms, OpenVMX banner, ODS-2 DKA0: mounted)"
      log "======================================================================"
      exit 0
    fi
    die "BOOT-VAX FAILED (see console output above)"
    ;;
  negctl)
    if run_session negctl /cache/boot-work; then
      log "PASS: negative control -- the MOUNTED milestone correctly did not "
      log "      appear without an ODS-2 system disk (the mount gate has teeth)"
      exit 0
    fi
    die "NEGATIVE CONTROL FAILED unexpectedly (see console output above)"
    ;;
  *) die "unknown mode '${MODE}' (want: prove | negctl)" ;;
esac
