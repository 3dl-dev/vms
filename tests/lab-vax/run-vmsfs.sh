#!/bin/bash
# run-vmsfs.sh - orchestrate the vms-544d runtime proof: a mastered OVMX ODS-2
# volume MOUNTS + READS on real NetBSD/vax under SIMH (epic vms-8e8, tributary A
# V5; satisfies the PID-1 /vms mount, gates vms-7b1 -> capstone vms-d59).
#
# The ODS-2 sibling of run-devvms.sh (the P4-B executive /dev/vms proof), reusing
# the SAME vax runtime substrate: the custom GENERIC+MODULAR kernel
# (tools/cross-vax/build-vax-modular-kernel.sh -- vax GENERIC omits MODULAR),
# installed on the cached NetBSD/vax disk, booted single-user (securelevel 0) so
# an out-of-tree module can modload. Here the module is the ODS-2 vnode/VFS
# module (vmsfs.kmod) and the proof mounts + reads a mastered volume.
#
# STAGES (each SIMH invocation wrapped in a HARD `timeout'; container force-killed
# on timeout):
#   1. CROSS-BUILD (ovmx-cross-vax): the loadable elf32-vax vmsfs.kmod (-fno-pic,
#      all OVMX symbols resolved) + the static vmsfs_mount helper.
#   2. BUILD-KERNEL (ovmx-cross-vax): the GENERIC+MODULAR vax kernel (cached).
#   3. MASTER (ovmx-cross-vax): a small OVMX ODS-2 volume with HELLO.TXT, via
#      tests/qemu/mkimage_vmsfs.c (host cc; arch-independent). Cached.
#   4. INSTALL (ovmx-vax-lab): install NetBSD/vax once if the cache is cold.
#   5. INSTALL-KERNEL (ovmx-vax-lab): swap in the MODULAR kernel once (marked).
#   6. PROVE (ovmx-vax-lab): boot single-user, modload vmsfs, mount the ra1
#      volume read-only, ls + cat HELLO.TXT; plus the INV-6 module-absent
#      control; then (rd vms-e7a) remount READ-WRITE and prove the write VOPs
#      (VOP_CREATE/WRITE/SETATTR/MKDIR/REMOVE) via an unmount+remount
#      persistence round trip -- see drive_vmsfs_vax.py's stage 4.
#
# Nothing installed on the host (Rule 9). Reuses the cached disk; never reinstalls.
#
#   tests/lab-vax/run-vmsfs.sh              # build all, ensure disk+kernel+volume, PROVE
#   tests/lab-vax/run-vmsfs.sh negctl       # VMSFS_SKIP_MODLOAD teeth (fail-then-pass)
#
# THE NEGCTL CONTRACT (rd vms-cf5): this wrapper never hand-rolls its own
# "if driver_exit; then die ...; fi" inversion for negctl mode -- it sources
# negctl_gate.sh and applies vaxharness_negctl_gate() to the driver session's
# raw exit code, the ONE inversion rule shared with vaxharness.py's Python
# negctl_gate() (and unit-tested identically to it, test_vaxharness.py).
# drive_vmsfs_vax.py itself is NEVER negctl-mode-aware.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
MODE="${1:-prove}"

# shellcheck source=tests/lab-vax/negctl_gate.sh
source "${HERE}/negctl_gate.sh"

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
ARTIFACTS_DIR="${CACHE_DIR}/vmsfs-artifacts"     # vmsfs.kmod + vmsfs_mount + netbsd-OVMX
ODS2_IMG="${ODS2_IMG:-${CACHE_DIR}/ovmx-ods2-vax.img}"
KBUILD_DIR="${KBUILD_DIR:-${CACHE_DIR}/kbuild}"
NBSRC_DIR="${KBUILD_DIR}/nbsrc"
KERNEL_MARKER="${CACHE_DIR}/.ovmx-modular-kernel-installed"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-5400}"
KBUILD_TIMEOUT="${KBUILD_TIMEOUT:-5400}"
SESSION_TIMEOUT="${SESSION_TIMEOUT:-2700}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

log() { echo "[run-vmsfs] $*"; }
die() { echo "[run-vmsfs] FATAL: $*" >&2; exit 1; }

ensure_images() {
  docker image inspect "${CROSS_IMAGE}" >/dev/null 2>&1 || {
    log "building ${CROSS_IMAGE}"
    docker build -f "${REPO}/tools/cross-vax/Dockerfile" -t "${CROSS_IMAGE}" "${REPO}/tools/cross-vax"; }
  docker image inspect "${LAB_IMAGE}" >/dev/null 2>&1 || {
    log "building ${LAB_IMAGE}"
    docker build -f "${HERE}/Dockerfile" -t "${LAB_IMAGE}" "${HERE}"; }
}

fetch_set() {
  local name="$1"
  local sha="$2"
  local tgz="${KBUILD_DIR}/${name}.tgz"
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

# 1. cross-build the loadable vmsfs module + mount helper
cross_build() {
  ensure_src
  mkdir -p "${ARTIFACTS_DIR}"
  log "cross-building vmsfs.kmod (-fno-pic, loadable) + vmsfs_mount for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src \
    -v "${NBSRC_DIR}:/nbsrc:ro" -v "${ARTIFACTS_DIR}:/out" \
    "${CROSS_IMAGE}" sh tools/cross-vax/build-vmsfs-mount-vax.sh
  [ -f "${ARTIFACTS_DIR}/vmsfs.kmod" ] && [ -f "${ARTIFACTS_DIR}/vmsfs_mount" ] || die "cross-build missing artifacts"
}

# 2. build the custom MODULAR kernel (cached on the artifact)
build_kernel() {
  if [ -f "${ARTIFACTS_DIR}/netbsd-OVMX" ]; then
    log "MODULAR kernel artifact present -- NOT rebuilding"; return 0; fi
  ensure_src
  mkdir -p "${KBUILD_DIR}/obj" "${KBUILD_DIR}/tools" "${KBUILD_DIR}/dest" "${ARTIFACTS_DIR}"
  log "building the GENERIC+MODULAR NetBSD/vax kernel (build.sh; hard cap ${KBUILD_TIMEOUT}s)"
  local cid="ovmx-vmsfs-kbuild-$$"; local rc=0
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

# 3. master a small OVMX ODS-2 volume (host cc; arch-independent). Cached.
master_volume() {
  if [ -f "${ODS2_IMG}" ]; then
    log "mastered ODS-2 volume present -- NOT re-mastering (${ODS2_IMG})"; return 0; fi
  log "mastering a small OVMX ODS-2 volume (tests/qemu/mkimage_vmsfs.c)"
  docker run --rm -v "${REPO}:/src:ro" -v "$(dirname "${ODS2_IMG}"):/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c \
    "cc -O2 -Wall -I /src/src/kernel/vmsfs -o /tmp/mkimage_vmsfs /src/tests/qemu/mkimage_vmsfs.c && /tmp/mkimage_vmsfs /out/$(basename "${ODS2_IMG}")"
  [ -f "${ODS2_IMG}" ] || die "ODS-2 mastering did not produce ${ODS2_IMG}"
  log "mastered ODS-2 volume: $(ls -l "${ODS2_IMG}" | awk '{print $5}') bytes"
}

# 4. ensure the cached NetBSD/vax disk (install once)
ensure_disk() {
  if [ -f "${CACHE_DIR}/anita-work/wd0.img" ]; then
    log "cached NetBSD/vax disk present -- NOT reinstalling"; return 0; fi
  log "cached disk absent -- one-time lab-vax install (SLOW, hard cap ${INSTALL_TIMEOUT}s)"
  local cid="ovmx-vmsfs-install-$$"; local rc=0
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

# run the driver in one SIMH session with a given OVMX_MODE; returns the
# driver's RAW (never mode-inverted) exit code via $? -- negctl_gate.sh
# applies the inversion in the caller, never here.
#
# CALL-SITE CONTRACT (bash `set -e` scope-leak; see run-mbx.sh's/run-
# access.sh's identical comment for the full empirical repro): `errexit` is
# a GLOBAL shell option, not function-scoped, so the `set -e` this function
# runs right before its own `return "${rc}"` stays in effect in the CALLER
# too. The ONLY safe way to call this function is `||`-protected --
#     rc=0; run_session ... || rc=$?
# -- never `set +e; run_session ...; rc=$?; set -e`.
run_session() {
  local ovmx_mode="$1" skip_load="${2:-}"; local cid="ovmx-vmsfs-${ovmx_mode}-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_MODE="${ovmx_mode}" -e OVMX_ARTIFACTS=/artifacts -e OVMX_NETBSD_DIR=/netbsd \
      -e OVMX_ODS2_IMG=/cache/"$(basename "${ODS2_IMG}")" \
      ${skip_load:+-e VMSFS_SKIP_MODLOAD=1} \
      -v "${CACHE_DIR}:/cache" -v "${ARTIFACTS_DIR}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_vmsfs_vax.py
  rc=$?; set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  return "${rc}"
}

# 5. install the MODULAR kernel onto the disk (once)
ensure_modular_kernel() {
  if [ -f "${KERNEL_MARKER}" ]; then
    log "MODULAR kernel already installed on the cached disk -- skipping"; return 0; fi
  log "installing the MODULAR kernel onto the disk (boot GENERIC single-user, swap /netbsd)"
  run_session install-kernel || die "MODULAR-kernel install session failed"
  touch "${KERNEL_MARKER}"
}

ensure_images
cross_build
build_kernel
master_volume
ensure_disk
ensure_modular_kernel

case "${MODE}" in
  prove)
    rc=0
    run_session prove || rc=$?
    if vaxharness_negctl_gate "${rc}" 0; then
      log "======================================================================"
      log "  VMSFS-VAX PASSED: OVMX ODS-2 volume MOUNTS + READS on NetBSD/vax/SIMH"
      log "======================================================================"
      exit 0
    fi
    die "VMSFS-VAX FAILED (driver exit=${rc}; see console output above)"
    ;;
  negctl)
    rc=0
    run_session prove skip || rc=$?
    if vaxharness_negctl_gate "${rc}" 1; then
      log "PASS: negative control failed as required (driver exit=${rc}; the mount+read assertion has teeth)"
      exit 0
    fi
    die "NEGATIVE CONTROL DID NOT FAIL: harness passed (exit=${rc}) with the module unloaded -- no teeth"
    ;;
  *) die "unknown mode '${MODE}' (want: prove | negctl)" ;;
esac
