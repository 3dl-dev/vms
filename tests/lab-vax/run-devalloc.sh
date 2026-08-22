#!/bin/bash
# run-devalloc.sh - rd vms-618 proof: the OVMX executive's DEVICE-ALLOCATION
# facility ($ALLOC / $DALLOC, VMS_IOCTL_ALLOC 0x55 / DALLOC 0x56) is GENUINE on
# NetBSD/vax -- answered by the real executive-resident device table
# (src/kernel-core/vms_devtab.c, ported into the NetBSD `vms' module by this
# item), not by a substrate-local handler that reports success.
#
# THE POINT (INV-6). A local $ALLOC that just answered SS$_NORMAL would pass
# every single-process assertion and still be a facade. The decisive property is
# cross-process: a device ONE process allocates is REFUSED to ANOTHER with
# SS$_DEVALLOC, because the row lives in the executive. drive_devalloc_vax.py
# runs each operation as a SEPARATE guest process to exercise exactly that, plus
# the teeth (a device that does not exist must be SS$_NOSUCHDEV).
#
# REUSES tests/lab-vax's proven substrate WITHOUT rebuilding it: it does NOT
# install NetBSD and does NOT build the MODULAR kernel -- run-boot.sh already
# produces both (cached under .boot-cache/lab-vax/{anita-work,boot-artifacts}).
# It cross-builds the CURRENT vms.kmod + the vmsdevalloc probe, clones the
# cached disk into its OWN workdir (so it can never disturb a boot/install run
# using anita-work or boot-work), boots once, and runs the probes.
#
#   tests/lab-vax/run-devalloc.sh
#
# Prerequisite (produced by tests/lab-vax/run-boot.sh):
#   anita-work/wd0.img         installed NetBSD/vax with the MODULAR kernel
#   boot-artifacts/netbsd-OVMX the MODULAR kernel artifact
#   ovmx-ods2-vax.img          any ODS-2 volume image (attached as ra1 -> DKA0:)
#   dka100-target.img          any second disk image (attached as ra2 -> DKA100:)
#
# HARD timeout on the one SIMH invocation; container force-killed on timeout.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "${HERE}/../.." && pwd)"

NETBSD_VERSION="${NETBSD_VERSION:-10.1}"
SETS="${SETS:-kern-GENERIC,base,etc}"

CACHE_DIR="${CACHE_DIR:-${REPO}/.boot-cache/lab-vax}"
BOOT_ARTIFACTS="${CACHE_DIR}/boot-artifacts"        # netbsd-OVMX (from run-boot.sh)
DEVALLOC_ARTIFACTS="${CACHE_DIR}/devalloc-artifacts" # vms.kmod + vmsdevalloc (built here)
DEVALLOC_WORK="${CACHE_DIR}/devalloc-work"           # this proof's PRIVATE disk copy
KBUILD_DIR="${KBUILD_DIR:-${CACHE_DIR}/kbuild}"
NBSRC_DIR="${NBSRC_DIR:-${KBUILD_DIR}/nbsrc}"

DKA0_IMG="${DKA0_IMG:-${CACHE_DIR}/ovmx-ods2-vax.img}"
DKA100_IMG="${DKA100_IMG:-${CACHE_DIR}/dka100-target.img}"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

SESSION_TIMEOUT="${SESSION_TIMEOUT:-2700}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

log() { echo "[run-devalloc] $*"; }
die() { echo "[run-devalloc] FATAL: $*" >&2; exit 1; }

ensure_prereqs() {
  docker image inspect "${CROSS_IMAGE}" >/dev/null 2>&1 || die "${CROSS_IMAGE} not built -- run tests/lab-vax/run-boot.sh once first"
  docker image inspect "${LAB_IMAGE}"   >/dev/null 2>&1 || die "${LAB_IMAGE} not built -- run tests/lab-vax/run-boot.sh once first"
  [ -f "${CACHE_DIR}/anita-work/wd0.img" ] || die "no cached disk at ${CACHE_DIR}/anita-work/wd0.img -- run tests/lab-vax/run-boot.sh first"
  [ -f "${BOOT_ARTIFACTS}/netbsd-OVMX" ]   || die "no MODULAR kernel at ${BOOT_ARTIFACTS}/netbsd-OVMX -- run tests/lab-vax/run-boot.sh first"
  [ -d "${NBSRC_DIR}/usr/src/sys" ]        || die "no NetBSD syssrc at ${NBSRC_DIR} -- run tests/lab-vax/run-boot.sh first"
  [ -f "${DKA0_IMG}" ]                     || die "missing DKA0: image ${DKA0_IMG}"
  [ -f "${DKA100_IMG}" ]                   || die "missing DKA100: image ${DKA100_IMG}"
}

# ALWAYS rebuild: this proof is about the CURRENT executive, so a cached kmod
# would be a false green waiting to happen (the run-boot.sh cross_build guard's
# lesson, applied the other way).
cross_build() {
  mkdir -p "${DEVALLOC_ARTIFACTS}"
  log "cross-building the CURRENT vms.kmod + the vmsdevalloc probe for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src -v "${NBSRC_DIR}:/nbsrc:ro" \
    -v "${DEVALLOC_ARTIFACTS}:/out" --entrypoint sh "${CROSS_IMAGE}" -c \
    'OUT=/tmp/build-devvms sh tools/cross-vax/build-devvms-vax.sh &&
     cp /tmp/build-devvms/vms.kmod /out/vms.kmod &&
     cp /tmp/build-devvms/vmsdevalloc /out/vmsdevalloc'
  [ -f "${DEVALLOC_ARTIFACTS}/vms.kmod" ] && [ -f "${DEVALLOC_ARTIFACTS}/vmsdevalloc" ] \
    || die "cross-build missing artifacts"
  # The MODULAR kernel the guest boots is the one run-boot.sh built.
  cp -f "${BOOT_ARTIFACTS}/netbsd-OVMX" "${DEVALLOC_ARTIFACTS}/netbsd-OVMX"
}

# A PRIVATE clone of the cached disk: this proof must never share a disk image
# with a concurrent run-boot.sh session (SIMH opens it read-write).
make_disk_copy() {
  mkdir -p "${DEVALLOC_WORK}"
  if [ -f "${DEVALLOC_WORK}/wd0.img" ]; then
    log "private disk copy present -- reusing ${DEVALLOC_WORK}/wd0.img"; return 0; fi
  log "cloning the cached MODULAR-kernel disk -> ${DEVALLOC_WORK}/wd0.img"
  cp "${CACHE_DIR}/anita-work/wd0.img" "${DEVALLOC_WORK}/wd0.img"
  [ -f "${CACHE_DIR}/anita-work/netbsd.ini" ] \
    && cp "${CACHE_DIR}/anita-work/netbsd.ini" "${DEVALLOC_WORK}/netbsd.ini" || true
}

prove() {
  local cid="ovmx-devalloc-drive-$$"; local rc=0
  log "booting NetBSD/vax under SIMH to prove genuine \$ALLOC/\$DALLOC"
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_NETBSD_DIR=/netbsd -e NETBSD_WORKDIR=/cache/devalloc-work \
      -e OVMX_ARTIFACTS=/artifacts \
      -e OVMX_DKA0_IMG=/cache/"$(basename "${DKA0_IMG}")" \
      -e OVMX_DKA100_IMG=/cache/"$(basename "${DKA100_IMG}")" \
      -v "${CACHE_DIR}:/cache" -v "${DEVALLOC_ARTIFACTS}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_devalloc_vax.py
  rc=$?; set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  return "${rc}"
}

ensure_prereqs
cross_build
make_disk_copy
if prove; then
  log "======================================================================"
  log "  DEVALLOC-VAX PASSED (rd vms-618): \$ALLOC/\$DALLOC on NetBSD/vax are"
  log "  answered by the REAL executive-resident device table -- a nonexistent"
  log "  unit is SS\$_NOSUCHDEV, a unit one process allocates is refused to"
  log "  another with SS\$_DEVALLOC, and \$DALLOC really releases it."
  log "======================================================================"
  exit 0
fi
die "DEVALLOC-VAX FAILED (see console output above)"
