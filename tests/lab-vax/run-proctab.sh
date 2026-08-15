#!/bin/bash
# run-proctab.sh - orchestrate the process-table / $GETJPI / $PROCESS_SCAN
# runtime proof (rd vms-2e0, parent vms-945e, epic vms-8e8): the process
# table proven cross-process against a REAL /dev/vms on NetBSD/vax under SIMH
# (INV-6 honest).
#
# Sibling of run-eflag.sh (the P4-E event-flag proof) and of
# tests/netbsd/drive_netbsd_p4a.py's PROCTAB phase, for the VAX architecture
# under SIMH. Reuses P4-B/P4-E's compile-into-kernel fallback unchanged (vax
# GENERIC omits `options MODULAR'; see tests/lab-vax/README.md "P4-B"): build
# a custom GENERIC+MODULAR kernel, install it, and modload the UNCHANGED
# cross-built OVMX module (which already carries src/kernel-core/vms_proctab.c,
# the same facility source the per-PR B1 gate width-checks).
#
# THE NEGCTL CONTRACT (rd vms-cf5): this wrapper never hand-rolls its own
# "if driver_exit; then die ...; fi" inversion for negctl mode -- it sources
# negctl_gate.sh and applies vaxharness_negctl_gate() to the driver session's
# raw exit code, the ONE inversion rule shared with vaxharness.py's Python
# negctl_gate() (and unit-tested identically to it, test_vaxharness.py). This
# is the fix for the exact bug class rd vms-cf5 documents: a driver and its
# wrapper silently disagreeing about what "negative control satisfied" means.
#
# STAGES (each SIMH invocation wrapped in a HARD `timeout'; container
# force-killed on timeout):
#   1. CROSS-BUILD (ovmx-cross-vax): the loadable elf32-vax vms.kmod + the
#      static vmsproctab guest tool (tools/cross-vax/build-proctab-vax.sh).
#   2. BUILD-KERNEL (ovmx-cross-vax): the GENERIC+MODULAR vax kernel
#      (netbsd-OVMX) via NetBSD build.sh. Cached: skipped if the kernel
#      artifact already exists (own proctab-artifacts cache, mirrors how
#      P4-E's eflag-artifacts is separate from P4-B's devvms-artifacts).
#   3. INSTALL (ovmx-vax-lab): if the cached NetBSD/vax disk is absent,
#      install it ONCE (design-p4 §5: install-once, never a hot-path
#      reinstall).
#   4. INSTALL-KERNEL (ovmx-vax-lab): if not already done, boot the disk
#      GENERIC single-user and swap in the MODULAR kernel as /netbsd.
#   5. PROVE (ovmx-vax-lab): boot the MODULAR kernel single-user, modload,
#      mknod /dev/vms, run the cross-process process-table proof; plus the
#      INV-6 module-absent control and the PROCTAB_SKIP_LOAD/PROCTAB_SKIP_BG
#      teeth checks.
#
# Nothing is installed on the host -- SIMH, anita, the vax cross toolchain and
# NetBSD all live in containers / the mounted cache (Rule 9).
#
#   tests/lab-vax/run-proctab.sh              # build everything, ensure disk+kernel, PROVE
#   tests/lab-vax/run-proctab.sh negctl-load  # PROCTAB_SKIP_LOAD teeth check
#   tests/lab-vax/run-proctab.sh negctl-bg    # PROCTAB_SKIP_BG teeth check
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"
MODE="${1:-prove}"

# shellcheck source=tests/lab-vax/negctl_gate.sh
source "${HERE}/negctl_gate.sh"

# --- pinned inputs (bump deliberately, together, with fresh checksums; IDENTICAL
# to run-eflag.sh/run-devvms.sh -- same NetBSD/vax release, same shared disk) --
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
ARTIFACTS_DIR="${CACHE_DIR}/proctab-artifacts"       # module+tool+kernel (small)
KBUILD_DIR="${KBUILD_DIR:-${CACHE_DIR}/proctab-kbuild}"
NBSRC_DIR="${KBUILD_DIR}/nbsrc"
KERNEL_MARKER="${CACHE_DIR}/.ovmx-proctab-modular-kernel-installed"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-5400}"   # 90 min hard cap: NetBSD/vax install
KBUILD_TIMEOUT="${KBUILD_TIMEOUT:-5400}"     # 90 min hard cap: build.sh tools+kernel
SESSION_TIMEOUT="${SESSION_TIMEOUT:-2700}"   # 45 min hard cap: one SIMH boot session
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

log() { echo "[run-proctab] $*"; }
die() { echo "[run-proctab] FATAL: $*" >&2; exit 1; }

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
    log "full NetBSD src tree present: ${NBSRC_DIR}"; return 0; fi
  mkdir -p "${NBSRC_DIR}"
  log "fetching + extracting the full NetBSD ${NETBSD_VERSION} src tree (src+gnusrc+sharesrc+syssrc)"
  fetch_set src "${SRC_SHA512}"
  fetch_set gnusrc "${GNUSRC_SHA512}"
  fetch_set sharesrc "${SHARESRC_SHA512}"
  fetch_set syssrc "${SYSSRC_SHA512}"
  [ -f "${NBSRC_DIR}/usr/src/build.sh" ] || die "src tree extract incomplete"
}

# 1. cross-build the loadable module + process-table tool
cross_build() {
  ensure_src
  mkdir -p "${ARTIFACTS_DIR}"
  log "cross-building vms.kmod (-fno-pic, loadable) + vmsproctab for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src \
    -v "${NBSRC_DIR}:/nbsrc:ro" -v "${ARTIFACTS_DIR}:/out" \
    "${CROSS_IMAGE}" sh tools/cross-vax/build-proctab-vax.sh
  [ -f "${ARTIFACTS_DIR}/vms.kmod" ] && [ -f "${ARTIFACTS_DIR}/vmsproctab" ] || die "cross-build missing artifacts"
}

# 2. build the custom MODULAR kernel (cached on the artifact)
build_kernel() {
  if [ -f "${ARTIFACTS_DIR}/netbsd-OVMX" ]; then
    log "MODULAR kernel artifact present -- NOT rebuilding (${ARTIFACTS_DIR}/netbsd-OVMX)"; return 0; fi
  ensure_src
  mkdir -p "${KBUILD_DIR}/obj" "${KBUILD_DIR}/tools" "${KBUILD_DIR}/dest" "${ARTIFACTS_DIR}"
  log "building the GENERIC+MODULAR NetBSD/vax kernel (build.sh; hard cap ${KBUILD_TIMEOUT}s)"
  local cid="ovmx-vax-proctab-kbuild-$$"; local rc=0
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

# 3. ensure the cached NetBSD/vax disk (install once; SHARED with the other
#    lab-vax jobs via the same actions/cache key on wd0.img's location)
ensure_disk() {
  if [ -f "${CACHE_DIR}/anita-work/wd0.img" ]; then
    log "cached NetBSD/vax disk present -- NOT reinstalling"; return 0; fi
  log "cached disk absent -- one-time lab-vax install (SLOW, hard cap ${INSTALL_TIMEOUT}s)"
  local cid="ovmx-vax-proctab-install-$$"; local rc=0
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
# CALL-SITE CONTRACT (bash `set -e` scope-leak, found on vms-4b7's own
# nightly run -- and hit again here, verbatim, on this proof's first nightly
# run: a driver's clean negctl-load proof-failure, exit 16, reddened the JOB
# even though vaxharness_negctl_gate() should have inverted it). `errexit` is
# a GLOBAL shell option, not function-scoped: the `set -e` this function runs
# right before `return "${rc}"` stays in effect in the CALLER after the call
# returns, regardless of what `set -e`/`set +e` the caller had in effect when
# it MADE the call. So a caller that invokes this function as a bare
# statement --
#     set +e; run_session ...; rc=$?; set -e
# -- does NOT actually protect itself: by the time `run_session` returns a
# nonzero `rc`, `-e` has already been turned back ON (by this function, one
# line above its own `return`), and bash's errexit check fires against THAT
# state, aborting the whole script immediately -- `rc=$?` and everything
# after it (including the `vaxharness_negctl_gate` call) never runs.
#
# THE FIX, and the ONLY safe way to call this function: protect the call
# with `||`, which bash exempts from errexit unconditionally --
#     rc=0; run_session ... || rc=$?
# -- never `set +e; run_session ...; rc=$?; set -e`. Every case in the MODE
# dispatch below uses the `||` form for exactly this reason (mirrors the
# merged run-access.sh fix, rd vms-4b7).
run_session() {
  local ovmx_mode="$1" skip_load="${2:-}" skip_bg="${3:-}"; local cid="ovmx-vax-proctab-${ovmx_mode}-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_MODE="${ovmx_mode}" -e OVMX_ARTIFACTS=/artifacts -e OVMX_NETBSD_DIR=/netbsd \
      ${skip_load:+-e PROCTAB_SKIP_LOAD=1} ${skip_bg:+-e PROCTAB_SKIP_BG=1} \
      -v "${CACHE_DIR}:/cache" -v "${ARTIFACTS_DIR}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_proctab_vax.py
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
    rc=0
    run_session prove || rc=$?
    if vaxharness_negctl_gate "${rc}" 0; then
      log "======================================================================"
      log "  PROCTAB-VAX PASSED: the process table shared cross-process on"
      log "  real NetBSD/vax under SIMH, real /dev/vms (INV-6 honest)"
      log "======================================================================"
      exit 0
    fi
    die "PROCTAB-VAX FAILED (driver exit=${rc}; see console output above)"
    ;;
  negctl-load)
    # Teeth: with modload skipped, the whole proof MUST fail (driver exit != 0).
    rc=0
    run_session prove skip "" || rc=$?
    if vaxharness_negctl_gate "${rc}" 1; then
      log "PASS: PROCTAB_SKIP_LOAD negative control failed as required (driver exit=${rc})"
      exit 0
    fi
    die "NEGATIVE CONTROL DID NOT FAIL: harness passed (exit=${rc}) with the module unloaded -- no teeth"
    ;;
  negctl-bg)
    # Teeth: with process A never launched, process B's by-name $GETJPI MUST
    # find nothing, which makes the SAME positive-assertion script the prove
    # mode runs fail for real (driver_proctab_vax.py does NOT special-case
    # this into an early "negctl ok" exit -- see its skip_bg comment, and
    # rd vms-cf5's whole point). So the gate here is IDENTICAL in shape to
    # negctl-load: negctl=1, satisfied iff the driver exited nonzero.
    rc=0
    run_session prove "" skip || rc=$?
    if vaxharness_negctl_gate "${rc}" 1; then
      log "PASS: PROCTAB_SKIP_BG negative control failed as required (driver exit=${rc})"
      exit 0
    fi
    die "NEGATIVE CONTROL DID NOT FAIL: harness passed (exit=${rc}) with process A never registered -- no teeth"
    ;;
  *) die "unknown mode '${MODE}' (want: prove | negctl-load | negctl-bg)" ;;
esac
