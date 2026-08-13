#!/bin/bash
# run-devvms.sh - orchestrate the P4-B runtime proof (rd vms-f78bb, epic
# vms-8e8): the OVMX executive `vms' pseudo-device LIVE on real NetBSD/vax under
# SIMH, with a version/ping ioctl round-tripping through /dev/vms.
#
# Sibling of tests/netbsd/run_p2b.sh (the amd64 module-load+ping proof), for the
# VAX architecture under SIMH. Because NetBSD/vax GENERIC omits `options MODULAR'
# (it is the ONLY port that does) AND the vax port's module-loader glue was never
# wired into the build, the amd64 "just modload it" path does not work on stock
# vax. So this harness takes design-p4's compile-into-kernel FALLBACK, in its
# least-invasive form: build a custom kernel that re-enables the module framework
# (GENERIC + MODULAR, patching the vax wiring gap), install it, and modload the
# UNCHANGED cross-built OVMX module. See tests/lab-vax/README.md ("P4-B") and
# tools/cross-vax/build-vax-modular-kernel.sh.
#
# STAGES (each SIMH invocation wrapped in a HARD `timeout'; container force-killed
# on timeout -- a prior emulator proof in this repo once ran unbounded 1h43m):
#   1. CROSS-BUILD (ovmx-cross-vax): the loadable elf32-vax vms.kmod (-fno-pic, so
#      its relocations are kobj-loadable) + the static vmsprobe.
#   2. BUILD-KERNEL (ovmx-cross-vax): the GENERIC+MODULAR vax kernel (netbsd-OVMX)
#      via NetBSD build.sh. Cached: skipped if the kernel artifact already exists.
#   3. INSTALL (ovmx-vax-lab): if the cached NetBSD/vax disk is absent, install it
#      ONCE (design-p4 §5: install-once, never a hot-path reinstall).
#   4. INSTALL-KERNEL (ovmx-vax-lab): if not already done, boot the disk GENERIC
#      single-user and swap in the MODULAR kernel as /netbsd (SIMH cannot inject a
#      kernel like `qemu -kernel', so it must reach the disk). Marked, done once.
#   5. PROVE (ovmx-vax-lab): boot the MODULAR kernel single-user, modload, mknod
#      /dev/vms, run the probe -> PING OK; plus the INV-6 module-absent control.
#
# Nothing is installed on the host -- SIMH, anita, the vax cross toolchain and
# NetBSD all live in containers / the mounted cache (Rule 9).
#
#   tests/lab-vax/run-devvms.sh              # build everything, ensure disk+kernel, PROVE
#   tests/lab-vax/run-devvms.sh negctl       # P4B_SKIP_LOAD teeth check (fail-then-pass)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
MODE="${1:-prove}"

# --- pinned inputs (bump deliberately, together, with fresh checksums) --------
NETBSD_VERSION="${NETBSD_VERSION:-10.1}"
ISO_NAME="NetBSD-${NETBSD_VERSION}-vax.iso"
ISO_SHA512="${ISO_SHA512:-aa763aa2240e4623adf09dd1a1ed2da0e3b96959d33544d52026a0c7c7448c6f0da8517bf059b9c53a9786782c0373b2e3da84de4b36cc5aeb669d219ac0f225}"
SETS="${SETS:-kern-GENERIC,base,etc}"
SRC_BASE="${SRC_BASE:-https://cdn.netbsd.org/pub/NetBSD/NetBSD-${NETBSD_VERSION}/source/sets}"
# NetBSD source sets needed to build a kernel with build.sh (sys + build infra +
# in-tree toolchain source). SHA512s from the release's own source/sets/SHA512.
SRC_SHA512="${SRC_SHA512:-6ae2053b4b75821238c0757d4f7258daece425de72524c616e07d3adee7c48d87422dd47d852a137918cec3dd3c0d339e372f4504dfe9f1bc5520011775bdb86}"
GNUSRC_SHA512="${GNUSRC_SHA512:-8a1c42030ba44eb2a0c7a5111187bc02e8f4d0860d8491b7863579e612333665c478625c37b01f08732e3cfd29ec31335f1db1274fd7dcfdc048b09d1b4bbb83}"
SHARESRC_SHA512="${SHARESRC_SHA512:-703eeb306fc0328cad7e6f0e100d2e7af194f82e613338f4611a7bcd5f6d773d8789e7ce03ec25268ec2b95ccdb97c3b4289a838a629716498b4d7c3184cb1ef}"
SYSSRC_SHA512="${SYSSRC_SHA512:-766ac21f33cfe0e701dfedb894fa07f36d811da1a12e979181e8fca7af4e627852680ce42a7b29e97dd3e2e402ddf9ae7bfba60c8d7dc6b8a3354d8ce8c06926}"

CACHE_DIR="${CACHE_DIR:-${REPO}/.boot-cache/lab-vax}"
ARTIFACTS_DIR="${CACHE_DIR}/devvms-artifacts"       # module+probe+kernel (small)
# Heavy, rebuildable build scratch (full src ~2.5G + build.sh obj/tools ~4G).
# Kept OUT of the disk-cache path by default so CI's actions/cache stays small;
# override KBUILD_DIR to a runner-temp path in CI.
KBUILD_DIR="${KBUILD_DIR:-${CACHE_DIR}/kbuild}"
NBSRC_DIR="${KBUILD_DIR}/nbsrc"
KERNEL_MARKER="${CACHE_DIR}/.ovmx-modular-kernel-installed"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-5400}"   # 90 min hard cap: NetBSD/vax install
KBUILD_TIMEOUT="${KBUILD_TIMEOUT:-5400}"     # 90 min hard cap: build.sh tools+kernel
SESSION_TIMEOUT="${SESSION_TIMEOUT:-2700}"   # 45 min hard cap: one SIMH boot session
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

log() { echo "[run-devvms] $*"; }
die() { echo "[run-devvms] FATAL: $*" >&2; exit 1; }

ensure_images() {
  docker image inspect "${CROSS_IMAGE}" >/dev/null 2>&1 || {
    log "building ${CROSS_IMAGE}"
    docker build -f "${REPO}/tools/cross-vax/Dockerfile" -t "${CROSS_IMAGE}" "${REPO}/tools/cross-vax"; }
  docker image inspect "${LAB_IMAGE}" >/dev/null 2>&1 || {
    log "building ${LAB_IMAGE}"
    docker build -f "${HERE}/Dockerfile" -t "${LAB_IMAGE}" "${HERE}"; }
}

# fetch+verify+extract one pinned source set into NBSRC_DIR
fetch_set() {
  # Separate `local' statements: under `set -u', referencing ${name} in the SAME
  # `local' that assigns it trips "unbound variable" (bash evaluates the RHS list
  # before the names bind). This path only runs on a COLD cache (no nbsrc yet),
  # which is why it never fired locally but reddened the first CI run.
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
    log "full NetBSD src tree present: ${NBSRC_DIR}"; return 0; fi
  mkdir -p "${NBSRC_DIR}"
  log "fetching + extracting the full NetBSD ${NETBSD_VERSION} src tree (src+gnusrc+sharesrc+syssrc)"
  fetch_set src "${SRC_SHA512}"
  fetch_set gnusrc "${GNUSRC_SHA512}"
  fetch_set sharesrc "${SHARESRC_SHA512}"
  fetch_set syssrc "${SYSSRC_SHA512}"
  [ -f "${NBSRC_DIR}/usr/src/build.sh" ] || die "src tree extract incomplete"
}

# 1. cross-build the loadable module + probe
cross_build() {
  ensure_src
  mkdir -p "${ARTIFACTS_DIR}"
  log "cross-building vms.kmod (-fno-pic, loadable) + vmsprobe for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src \
    -v "${NBSRC_DIR}:/nbsrc:ro" -v "${ARTIFACTS_DIR}:/out" \
    "${CROSS_IMAGE}" sh tools/cross-vax/build-devvms-vax.sh
  [ -f "${ARTIFACTS_DIR}/vms.kmod" ] && [ -f "${ARTIFACTS_DIR}/vmsprobe" ] || die "cross-build missing artifacts"
}

# 2. build the custom MODULAR kernel (cached on the artifact)
build_kernel() {
  if [ -f "${ARTIFACTS_DIR}/netbsd-OVMX" ]; then
    log "MODULAR kernel artifact present -- NOT rebuilding (${ARTIFACTS_DIR}/netbsd-OVMX)"; return 0; fi
  ensure_src
  mkdir -p "${KBUILD_DIR}/obj" "${KBUILD_DIR}/tools" "${KBUILD_DIR}/dest" "${ARTIFACTS_DIR}"
  log "building the GENERIC+MODULAR NetBSD/vax kernel (build.sh; hard cap ${KBUILD_TIMEOUT}s)"
  local cid="ovmx-vax-kbuild-$$"; local rc=0
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

# 3. ensure the cached NetBSD/vax disk (install once)
ensure_disk() {
  if [ -f "${CACHE_DIR}/anita-work/wd0.img" ]; then
    log "cached NetBSD/vax disk present -- NOT reinstalling"; return 0; fi
  log "cached disk absent -- one-time lab-vax install (SLOW, hard cap ${INSTALL_TIMEOUT}s)"
  local cid="ovmx-vax-devvms-install-$$"; local rc=0
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

# run the driver in one SIMH session with a given OVMX_MODE
run_session() {
  local ovmx_mode="$1" skip_load="${2:-}"; local cid="ovmx-vax-devvms-${ovmx_mode}-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_MODE="${ovmx_mode}" -e OVMX_ARTIFACTS=/artifacts -e OVMX_NETBSD_DIR=/netbsd \
      ${skip_load:+-e P4B_SKIP_LOAD=1} \
      -v "${CACHE_DIR}:/cache" -v "${ARTIFACTS_DIR}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_devvms_vax.py
  rc=$?; set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  return "${rc}"
}

# 4. install the MODULAR kernel onto the disk (once)
ensure_modular_kernel() {
  if [ -f "${KERNEL_MARKER}" ]; then
    log "MODULAR kernel already installed on the cached disk -- skipping"; return 0; fi
  log "installing the MODULAR kernel onto the disk (boot GENERIC single-user, swap /netbsd; hard cap ${SESSION_TIMEOUT}s)"
  run_session install-kernel || die "MODULAR-kernel install session failed"
  touch "${KERNEL_MARKER}"
}

ensure_images
cross_build
build_kernel
ensure_disk
ensure_modular_kernel

case "${MODE}" in
  prove)
    if run_session prove; then
      log "======================================================================"
      log "  DEVVMS-VAX PASSED: /dev/vms live on NetBSD/vax under SIMH, PING OK"
      log "======================================================================"
      exit 0
    fi
    die "DEVVMS-VAX FAILED (see console output above)"
    ;;
  negctl)
    # Teeth: with the module skipped, the PING OK assertion MUST fail.
    if run_session prove skip; then
      die "NEGATIVE CONTROL DID NOT FAIL: harness passed with the module unloaded -- no teeth"
    fi
    log "PASS: negative control failed as required (the PING assertion has teeth)"
    exit 0
    ;;
  *) die "unknown mode '${MODE}' (want: prove | negctl)" ;;
esac
