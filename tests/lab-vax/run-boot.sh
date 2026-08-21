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
#
# vms-d9c (parent vms-d59) adds two more modes that reuse the SAME assembled
# boot-work disk but attach a MASTERED OVMX SYSTEM volume (the five cross-built
# boot images + reused data/COM files + the Decision-A SYSTARTUP_VMS.COM,
# mastered by tools/vmsfs_master.c) instead of the small test ODS-2 volume:
#   tests/lab-vax/run-boot.sh sysboot         # boot PAST the installed-system
#                                             # gate to the PROVISION.EXE exec
#   tests/lab-vax/run-boot.sh sysboot-negctl  # teeth: flat volume (no DCL.EXE)
#                                             # must halt the installed-system gate
#
# vms-065 (parent vms-f10) adds ONE MORE mode: the gate-invokable, clean
# pass/fail check a release-acceptance/frozen-verify caller invokes directly
# -- runs sysboot THEN sysboot-negctl (both must hold) under ONE exit code,
# proven stable across N consecutive runs (see the vms-065 PR description):
#   tests/lab-vax/run-boot.sh gate            # 0 = VAX boots to DCL,
#                                             # nonzero = not release-clean
#
# vms-d0e5 (parent vms-4834) adds ONE MORE mode -- the two-disk INSTALL proof,
# the vax mirror of the x86_64 install e2e (tests/qemu/test_install_menu.sh +
# test_install_boot_e2e.sh + test_product_install_e2e.sh). It builds the real
# vax image set + OVMX-OS-VAX.KIT, masters the DISTRIBUTION volume (which boots
# into OVMX$INSTALL.COM), formats a BLANK ODS-2 target, then boots the
# distribution volume (rq1 -> DKA0:) with the blank target (rq2 -> DKA100:) and
# drives the menu's PRESERVE path to install OVMX onto the target, asserting a
# rooted, genuinely-activatable system tree AND a real on-disk write (sha256
# before/after, INV-6 teeth), via tests/lab-vax/drive_install_vax.py:
#   tests/lab-vax/run-boot.sh install         # 0 = install onto a blank target
#                                             # succeeded and really wrote bytes
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
# vms-d9c: the MASTERED OVMX system volume + the flat dir of the five boot .EXE
# images it carries (built by build-boot-images-vax.sh, staged by
# stage_sysvol.sh, mastered by tools/vmsfs_master.c). Used only by the sysboot
# modes; the prove/negctl modes use the small test ODS2_IMG above unchanged.
SYSVOL_IMG="${SYSVOL_IMG:-${CACHE_DIR}/ovmx-sysvol-vax.img}"
SYSVOL_IMAGES_DIR="${SYSVOL_IMAGES_DIR:-${CACHE_DIR}/sysvol-images}"
KBUILD_DIR="${KBUILD_DIR:-${CACHE_DIR}/kbuild}"
NBSRC_DIR="${NBSRC_DIR:-${KBUILD_DIR}/nbsrc}"
KERNEL_MARKER="${CACHE_DIR}/.ovmx-modular-kernel-installed"   # SHARED with siblings
BOOT_WORKDIR="${CACHE_DIR}/boot-work"
BOOT_COPY_MARKER="${CACHE_DIR}/.boot-disk-copied"
BOOT_INSTALL_MARKER="${CACHE_DIR}/.ovmx-init-installed"

# vms-d0e5 rung G: the two-disk INSTALL proof's artifacts (the `install' mode
# only). VAX_IMAGES_DIR holds the FULL shipped vax image set (real elf32-vax
# executables, built by the ovmx-images CMake target) that BOTH the OS kit and
# the distribution volume draw from; the distribution volume boots into
# OVMX$INSTALL.COM and installs onto the freshly-formatted blank target.
VAX_IMAGES_DIR="${VAX_IMAGES_DIR:-${CACHE_DIR}/vax-images}"
OS_KIT="${OS_KIT:-${CACHE_DIR}/OVMX-OS-VAX.KIT}"
DISTRIB_IMG="${DISTRIB_IMG:-${CACHE_DIR}/ovmx-distrib-vax.img}"
BLANK_TARGET_IMG="${BLANK_TARGET_IMG:-${CACHE_DIR}/dka100-target.img}"

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

# 3b (sysboot). Cross-build the FULL set of five boot images (DCL.EXE,
#    PROVISION.EXE, LOGINOUT.EXE, JOB_CONTROL.EXE, STARTUP.EXE) for elf32-vax
#    and collect them by name into SYSVOL_IMAGES_DIR. build-boot-images-vax.sh
#    already builds + Decision-A-activation-asserts all five; here we just copy
#    the delivered images up. Cached.
build_boot_image_set() {
  if [ "${FORCE_SYSVOL_BUILD:-0}" != "1" ] \
     && [ -f "${SYSVOL_IMAGES_DIR}/DCL.EXE" ] \
     && [ -f "${SYSVOL_IMAGES_DIR}/PROVISION.EXE" ] \
     && [ -f "${SYSVOL_IMAGES_DIR}/LOGINOUT.EXE" ] \
     && [ -f "${SYSVOL_IMAGES_DIR}/JOB_CONTROL.EXE" ] \
     && [ -f "${SYSVOL_IMAGES_DIR}/STARTUP.EXE" ]; then
    log "boot image set present -- NOT rebuilding (set FORCE_SYSVOL_BUILD=1 to force)"; return 0; fi
  mkdir -p "${SYSVOL_IMAGES_DIR}"
  log "cross-building the full boot image set (STARTUP/PROVISION/DCL/JOB_CONTROL/LOGINOUT) for elf32-vax"
  docker run --rm -v "${REPO}:/src" -w /src -v "${SYSVOL_IMAGES_DIR}:/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c '
      set -e
      OUT_ROOT=/tmp/vax-boot-images sh tools/cross-vax/build-boot-images-vax.sh
      cp /tmp/vax-boot-images/ovmx-init/STARTUP.EXE      /out/STARTUP.EXE
      cp /tmp/vax-boot-images/provision/PROVISION.EXE    /out/PROVISION.EXE
      cp /tmp/vax-boot-images/vmsdcl/DCL.EXE             /out/DCL.EXE
      cp /tmp/vax-boot-images/job-control/JOB_CONTROL.EXE /out/JOB_CONTROL.EXE
      cp /tmp/vax-boot-images/loginout/LOGINOUT.EXE      /out/LOGINOUT.EXE'
  for img in STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE; do
    [ -f "${SYSVOL_IMAGES_DIR}/${img}" ] || die "boot image set missing ${img}"
  done
}

# 3c (sysboot). Master the OVMX SYSTEM volume: build the host vmsfs_master, stage
#    the rooted [SYS0.SYSCOMMON] tree (stage_sysvol.sh: the five vax images +
#    reused data/COM files + the Decision-A SYSTARTUP_VMS.COM), and master a
#    64 MB vmsfs volume. All inside CROSS_IMAGE's native cc (same "cc a host tool
#    in the container" pattern as master_volume above -- nothing on the host).
#    vmsfs is little-endian on disk, so a host-built vmsfs_master masters a
#    vax-bootable volume directly.
master_system_volume() {
  # ALWAYS re-master (rd vms-72da). The mastered volume must reflect the CURRENT
  # staged boot images (stage_sysvol.sh + SYSVOL_IMAGES_DIR). Mastering is cheap
  # -- build vmsfs_master + stage + master a 64-block volume, seconds -- while the
  # EXPENSIVE artifacts (the MODULAR kernel, the NetBSD disk, the cross-built
  # EXEs) are cached separately (build_boot_images self-heals on any missing EXE).
  # A `-f SYSVOL_IMG' reuse guard here booted a STALE cached volume that predated
  # PROVISION.EXE joining the staging: the boot passed the DCL.EXE installed-gate
  # but PROVISION.EXE + OVMXVMSSYS.PAR were "missing", so PID 1 halted at
  # %OVMX-F-EXECINIT and the LNM/pager code was NEVER reached -- a false green.
  log "mastering the OVMX/NetBSD-vax SYSTEM volume (vmsfs_master + stage_sysvol.sh)"
  rm -f "${SYSVOL_IMG}"
  listing="$(docker run --rm -v "${REPO}:/src:ro" -v "${SYSVOL_IMAGES_DIR}:/images:ro" \
    -v "$(dirname "${SYSVOL_IMG}"):/out" --entrypoint sh "${CROSS_IMAGE}" -c '
      set -e
      cc -O2 -Wall -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
         -I /src/src/kernel/vmsfs -I /src/src/vmsfs/include \
         -o /tmp/vmsfs_master /src/tools/vmsfs_master.c \
         /src/src/vmsfs/ods2/ods2_reader.c /src/src/vmsfs/ods2/ods2_writer.c \
         /src/src/vmsfs/ods2/ods2_edit.c /src/src/vmsfs/ods2/ods2_bdev.c \
         /src/src/vmsfs/ods2/ods2_path.c /src/src/vmsfs/ods2/ods2_block_posix.c
      bash /src/tests/lab-vax/stage_sysvol.sh /images /src /tmp/stage
      /tmp/vmsfs_master master /out/'"$(basename "${SYSVOL_IMG}")"' OVMXSYS /tmp/stage 64
      /tmp/vmsfs_master list /out/'"$(basename "${SYSVOL_IMG}")")"
  echo "${listing}"
  [ -f "${SYSVOL_IMG}" ] || die "system-volume mastering did not produce ${SYSVOL_IMG}"
  # Hard content gate: the mastered volume MUST carry the images PID 1 execs and
  # the SYSGEN params it reads, so a staging/caching regression fails HERE (red),
  # never as a lenient boot that halts before PROVISION runs (rd vms-72da).
  for f in DCL.EXE PROVISION.EXE OVMXVMSSYS.PAR; do
    echo "${listing}" | grep -qiF "${f}" \
      || die "mastered system volume is MISSING ${f} -- staging/caching regression (vms-72da)"
  done
  log "mastered system volume carries DCL.EXE + PROVISION.EXE + OVMXVMSSYS.PAR"
}

# 3d (install). Cross-build the FULL shipped vax image set via the top-level
#    CMake `ovmx-images' target (tools/cross-vax/build-ovmx-images-vax-cmake.sh)
#    and collect the boot + utility images (PRODUCT/AUTHORIZE/INITIALIZE/SYSGEN)
#    into VAX_IMAGES_DIR. These are REAL elf32-vax executables (the script asserts
#    the Decision-A activation contract on each), NOT stand-ins -- both the OS kit
#    and the distribution volume below draw from here (rd vms-d0e5 rung G).
build_vax_images() {
  local need="STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE PRODUCT.EXE AUTHORIZE.EXE INITIALIZE.EXE SYSGEN.EXE"
  if [ "${FORCE_VAX_IMAGES:-0}" != "1" ]; then
    local have=1
    for img in $need; do [ -f "${VAX_IMAGES_DIR}/${img}" ] || have=0; done
    [ "${have}" -eq 1 ] && { log "vax image set present -- NOT rebuilding (set FORCE_VAX_IMAGES=1)"; return 0; }
  fi
  mkdir -p "${VAX_IMAGES_DIR}"
  log "cross-building the full shipped vax image set ('cmake --build --target ovmx-images'; hard cap ${KBUILD_TIMEOUT}s)"
  local cid="ovmx-install-images-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${KBUILD_TIMEOUT}" \
    docker run --rm --name "${cid}" -v "${REPO}:/src" -w /src -v "${VAX_IMAGES_DIR}:/out" \
      --entrypoint sh "${CROSS_IMAGE}" -c '
        set -e
        BUILD_DIR=/tmp/build-vax-images-cmake sh tools/cross-vax/build-ovmx-images-vax-cmake.sh
        for img in STARTUP.EXE PROVISION.EXE DCL.EXE JOB_CONTROL.EXE LOGINOUT.EXE \
                   PRODUCT.EXE AUTHORIZE.EXE INITIALIZE.EXE SYSGEN.EXE; do
          cp /tmp/build-vax-images-cmake/bin/$img /out/$img
        done'
  rc=$?; set -e
  [ "${rc}" -eq 0 ] || { docker kill "${cid}" >/dev/null 2>&1 || true; die "ovmx-images vax build failed/timed out (rc=${rc})"; }
  for img in $need; do [ -f "${VAX_IMAGES_DIR}/${img}" ] || die "ovmx-images build finished but ${img} missing"; done
  log "vax image set built: $(echo $need | wc -w) real elf32-vax images in ${VAX_IMAGES_DIR}"
}

# 3e (install). Package OVMX-OS-VAX.KIT from those REAL images via
#    tools/cross-vax/build-os-kit-vax.sh. That script needs a HOST ovmx_kit_pack
#    (it links vmsfs/vmslnm/ovmx_kit_reader, so it is not a single-file compile);
#    build it natively into /tmp inside CROSS_IMAGE -- NEVER into the repo tree
#    (shared-host hygiene) -- and pass it as the script's 4th arg.
build_os_kit() {
  if [ "${FORCE_OS_KIT:-0}" != "1" ] && [ -f "${OS_KIT}" ]; then
    log "OVMX-OS-VAX.KIT present -- NOT repacking (set FORCE_OS_KIT=1)"; return 0; fi
  log "packaging OVMX-OS-VAX.KIT from the real vax images (build-os-kit-vax.sh)"
  docker run --rm -v "${REPO}:/src" -w /src \
    -v "${VAX_IMAGES_DIR}:/images:ro" -v "$(dirname "${OS_KIT}"):/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c '
      set -e
      cmake -S /src -B /tmp/hostbuild -DBUILD_TOOLS=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
      cmake --build /tmp/hostbuild --target ovmx_kit_pack -- -j"$(nproc)" >/dev/null
      sh tools/cross-vax/build-os-kit-vax.sh /images /src \
         /out/'"$(basename "${OS_KIT}")"' /tmp/hostbuild/bin/ovmx_kit_pack'
  [ -f "${OS_KIT}" ] || die "OS kit packaging did not produce ${OS_KIT}"
  log "packaged ${OS_KIT}"
}

# 3f (install). Master the DISTRIBUTION volume: stage the installer-media shape
#    (stage_sysvol.sh --distribution --kit -- the system tree PLUS the OS kit at
#    SYS$UPDATE: and the distribution SYSTARTUP that @'s OVMX$INSTALL.COM) and
#    master it with vmsfs_master -> ovmx-distrib-vax.img. Mirrors
#    master_system_volume, in --distribution mode. Content-gates that the mastered
#    volume actually carries OVMX-OS-VAX.KIT.
master_distribution_volume() {
  [ -f "${OS_KIT}" ] || die "OS kit missing (build_os_kit first): ${OS_KIT}"
  log "mastering the OVMX/NetBSD-vax DISTRIBUTION volume (stage_sysvol.sh --distribution + vmsfs_master)"
  rm -f "${DISTRIB_IMG}"
  local listing
  listing="$(docker run --rm -v "${REPO}:/src:ro" -v "${VAX_IMAGES_DIR}:/images:ro" \
    -v "${OS_KIT}:/kit/OVMX-OS-VAX.KIT:ro" \
    -v "$(dirname "${DISTRIB_IMG}"):/out" --entrypoint sh "${CROSS_IMAGE}" -c '
      set -e
      cc -O2 -Wall -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
         -I /src/src/kernel/vmsfs -I /src/src/vmsfs/include \
         -o /tmp/vmsfs_master /src/tools/vmsfs_master.c \
         /src/src/vmsfs/ods2/ods2_reader.c /src/src/vmsfs/ods2/ods2_writer.c \
         /src/src/vmsfs/ods2/ods2_edit.c /src/src/vmsfs/ods2/ods2_bdev.c \
         /src/src/vmsfs/ods2/ods2_path.c /src/src/vmsfs/ods2/ods2_block_posix.c
      bash /src/tests/lab-vax/stage_sysvol.sh --distribution --kit /kit/OVMX-OS-VAX.KIT \
           /images /src /tmp/stage
      /tmp/vmsfs_master master /out/'"$(basename "${DISTRIB_IMG}")"' OVMXSYS /tmp/stage 64
      /tmp/vmsfs_master list /out/'"$(basename "${DISTRIB_IMG}")")"
  echo "${listing}"
  [ -f "${DISTRIB_IMG}" ] || die "distribution-volume mastering did not produce ${DISTRIB_IMG}"
  echo "${listing}" | grep -qiF "OVMX-OS-VAX.KIT" \
    || die "mastered distribution volume is MISSING OVMX-OS-VAX.KIT (staging regression, rd vms-d0e5)"
  log "mastered distribution volume carries OVMX-OS-VAX.KIT (boots into OVMX\$INSTALL.COM)"
}

# 3g (install). Format a BLANK ODS-2 install target (label WORK) -- the mirror of
#    the x86_64 install e2e's host-side `INITIALIZE.EXE --ods2 dka100.img WORK 16'.
#    The cross-built INITIALIZE.EXE is elf32-vax (cannot run on the host), so the
#    blank volume is formatted with the SAME host ODS-2 codec master_volume /
#    master_system_volume use -- vmsfs_master mastering an EMPTY tree -- which
#    produces a genuine, mountable ODS-2 volume with label WORK and free space for
#    the install. vmsfs is little-endian on disk (arch-neutral), so a host-mastered
#    volume mounts on the vax.
make_blank_target() {
  log "formatting a BLANK ODS-2 install target (label WORK) -> ${BLANK_TARGET_IMG}"
  rm -f "${BLANK_TARGET_IMG}"
  docker run --rm -v "${REPO}:/src:ro" -v "$(dirname "${BLANK_TARGET_IMG}"):/out" \
    --entrypoint sh "${CROSS_IMAGE}" -c '
      set -e
      cc -O2 -Wall -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
         -I /src/src/kernel/vmsfs -I /src/src/vmsfs/include \
         -o /tmp/vmsfs_master /src/tools/vmsfs_master.c \
         /src/src/vmsfs/ods2/ods2_reader.c /src/src/vmsfs/ods2/ods2_writer.c \
         /src/src/vmsfs/ods2/ods2_edit.c /src/src/vmsfs/ods2/ods2_bdev.c \
         /src/src/vmsfs/ods2/ods2_path.c /src/src/vmsfs/ods2/ods2_block_posix.c
      mkdir -p /tmp/blank
      /tmp/vmsfs_master master /out/'"$(basename "${BLANK_TARGET_IMG}")"' WORK /tmp/blank 16'
  [ -f "${BLANK_TARGET_IMG}" ] || die "blank target formatting did not produce ${BLANK_TARGET_IMG}"
  log "blank ODS-2 target formatted (label WORK)"
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
  local ovmx_mode="$1"
  local workdir="$2"
  shift 2
  local cid="ovmx-boot-${ovmx_mode}-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_MODE="${ovmx_mode}" -e OVMX_ARTIFACTS=/artifacts -e OVMX_NETBSD_DIR=/netbsd \
      -e NETBSD_WORKDIR="${workdir}" -e OVMX_ODS2_IMG=/cache/"$(basename "${ODS2_IMG}")" \
      -e OVMX_SYSVOL_IMG=/cache/"$(basename "${SYSVOL_IMG}")" \
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

# THE NEGCTL CONTRACT, BOOT'S OWN SHAPE (rd vms-cf5): unlike the facility
# drivers (access/eflag/proctab/mbx/devvms/vmsfs), drive_boot_vax.py's main()
# already computes the FINAL pass/fail verdict for EVERY mode itself --
# including negctl/sysboot-negctl, where "the negative control had teeth" IS
# the 0 exit (see drive_boot_vax.py's own mode dispatch). This wrapper is
# therefore deliberately NOT routed through negctl_gate.sh: there is no
# inversion to apply here (0 always means "this mode's gate is satisfied",
# nonzero always means it is not), so sourcing vaxharness_negctl_gate() would
# be a no-op at best and a real inversion bug at worst. drive_boot_vax.py's
# raw exit code is still exactly one of {0, PROOF_FAILED, HARNESS_ERROR}
# (rd vms-cf5's canonical exit-code contract) -- only the WRAPPER-side
# inversion step differs from its sibling drivers, by design.

# soft_die: die()'s message shape (the "[run-boot] FATAL: ..." line to
# stderr every failure path below already used) WITHOUT the `exit 1` --
# used only inside run_sysboot_positive/run_sysboot_negctl (vms-065) so
# those two can be called as ordinary functions returning 0/1 and composed
# by the `gate` mode below, instead of each unconditionally terminating the
# whole script the moment `sysboot`/`sysboot-negctl` runs standalone.
soft_die() { echo "[run-boot] FATAL: $*" >&2; return 1; }

# vms-065: the sysboot positive proof and its negative control, factored out
# of the case arms below into plain functions (0 = held, 1 = failed; never
# `exit` themselves) so `gate` mode can chain BOTH under one pass/fail exit
# code -- the single command a release-acceptance/frozen-verify caller
# invokes. Standalone `sysboot`/`sysboot-negctl` mode is unchanged: same log
# lines, same pass/fail conditions, just reached through a function call.
run_sysboot_positive() {
  # vms-d9c: master the OVMX system volume and boot vms-7b1's disk with it,
  # proving the boot passes the installed-system gate and reaches the
  # SYS$SYSTEM:PROVISION.EXE exec.
  build_boot_image_set
  master_system_volume
  # rd vms-e7a TEETH: drive_boot_vax.py's console check (no %OVMX-W-OWNER
  # after SYSTEM identity) only proves PROVISION reported no ERROR -- a
  # write VOP that silently no-ops (INV-6's exact failure class) would ALSO
  # produce zero warnings, since lchown(2) would report success either way.
  # A HASH of the raw sysvol image, before this boot vs. after, is the
  # positive proof: rq1 is attached WITHOUT `-r' (see do_sysboot's vmm_args),
  # so the guest's real block writes land in THIS host file. provision_
  # ownership() unconditionally lchown()s ~28 objects in the system tree
  # every boot (rd vms-e7a's own comment: "WHY IT RUNS ON EVERY BOOT"), so a
  # working write path MUST change these bytes; a no-op VOP_SETATTR cannot.
  local sysvol_hash_before sysvol_hash_after
  sysvol_hash_before="$(sha256sum "${SYSVOL_IMG}" | awk '{print $1}')"
  log "sysvol pre-boot sha256:  ${sysvol_hash_before}"
  if run_session sysboot /cache/boot-work; then
    sysvol_hash_after="$(sha256sum "${SYSVOL_IMG}" | awk '{print $1}')"
    log "sysvol post-boot sha256: ${sysvol_hash_after}"
    if [ "${sysvol_hash_before}" = "${sysvol_hash_after}" ]; then
      soft_die "SYSBOOT: the mounted ODS-2 SYSTEM volume's raw on-disk bytes did NOT change during the boot (rd vms-e7a). provision_ownership() unconditionally lchown()s every object in the system tree on every boot -- identical before/after bytes means either it never ran, or (the INV-6 failure class) VOP_SETATTR silently no-op'd instead of really writing. A green console (no %OVMX-W-OWNER) is NOT sufficient by itself; this hash diff is the positive proof of a real write."
      return 1
    fi
    log "OK (rd vms-e7a): the mounted ODS-2 SYSTEM volume's on-disk bytes CHANGED"
    log "  during this boot -- REAL block writes hit the volume (not a"
    log "  silently-faked/no-op VOP_SETATTR; INV-6 honest-failure teeth)."
    log "======================================================================"
    log "  SYSBOOT PASSED: ovmx_init on NetBSD/vax mounted the MASTERED OVMX"
    log "  ODS-2 SYSTEM volume READ-WRITE, passed the installed-system gate,"
    log "  reached PID 1's exec of SYS\$SYSTEM:PROVISION.EXE (%STDRV-I-STARTUP),"
    log "  and PROVISION's UIC-ownership writes really hit the on-disk volume."
    log "======================================================================"
    return 0
  fi
  soft_die "SYSBOOT FAILED (see console output above)"
}

run_sysboot_negctl() {
  # Teeth: boot the SAME disk with the FLAT test volume (no SYS$SYSTEM:DCL.EXE)
  # -- the installed-system gate must halt and %STDRV-I-STARTUP must NOT appear.
  if run_session sysboot-negctl /cache/boot-work; then
    log "PASS: sysboot negative control -- the flat volume (no DCL.EXE)"
    log "      halted the installed-system gate and never reached STDRV"
    log "      (the installed-system gate has teeth)"
    return 0
  fi
  soft_die "SYSBOOT NEGATIVE CONTROL FAILED unexpectedly (see console output above)"
}

# vms-d0e5 rung G: the two-disk INSTALL proof. Build the real vax image set + OS
# kit, master the distribution volume, format a blank ODS-2 target, then boot the
# distribution volume (rq1 -> DKA0:) with the blank target (rq2 -> DKA100:) and
# drive OVMX$INSTALL.COM to install onto it (drive_install_vax.py). sha256 the
# target before/after so the install's real block writes are PROVEN to land (the
# same INV-6 hash-diff teeth run_sysboot_positive applies to the RW system volume:
# a green console alone cannot rule out a silently-faked/no-op write).
run_install() {
  build_vax_images
  build_os_kit
  master_distribution_volume
  make_blank_target
  local before after
  before="$(sha256sum "${BLANK_TARGET_IMG}" | awk '{print $1}')"
  log "blank target pre-install sha256:  ${before}"
  local cid="ovmx-install-drive-$$"; local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e NETBSD_VERSION="${NETBSD_VERSION}" -e "SETS=${SETS}" \
      -e OVMX_NETBSD_DIR=/netbsd -e NETBSD_WORKDIR=/cache/boot-work \
      -e OVMX_DISTRIB_IMG=/cache/"$(basename "${DISTRIB_IMG}")" \
      -e OVMX_BLANK_IMG=/cache/"$(basename "${BLANK_TARGET_IMG}")" \
      -v "${CACHE_DIR}:/cache" -v "${ARTIFACTS_DIR}:/artifacts:ro" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_install_vax.py
  rc=$?; set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  after="$(sha256sum "${BLANK_TARGET_IMG}" | awk '{print $1}')"
  log "blank target post-install sha256: ${after}"
  if [ "${rc}" -ne 0 ]; then
    soft_die "INSTALL FAILED (drive_install_vax.py exit ${rc}; see console output above)"
    return 1
  fi
  if [ "${before}" = "${after}" ]; then
    soft_die "INSTALL: the blank target's raw on-disk bytes did NOT change during the install (rd vms-d0e5). The menu reported success but PRODUCT INSTALL's real block writes never hit the target image -- a silent no-op (INV-6 failure class). A green console is NOT sufficient by itself; this hash diff is the positive proof of a real write."
    return 1
  fi
  log "OK (rd vms-d0e5): the install target's on-disk bytes CHANGED -- REAL block"
  log "  writes hit DKA100: (not a silently-faked/no-op write; INV-6 teeth)."
  log "======================================================================"
  log "  INSTALL-VAX PASSED: the OVMX/NetBSD-vax DISTRIBUTION volume booted into"
  log "  OVMX\$INSTALL.COM, the PRESERVE path installed OVMX onto a BLANK ODS-2"
  log "  target over the executive (MOUNT + PRODUCT INSTALL + AUTHORIZE + SYSGEN),"
  log "  the installed target carries a ROOTED, genuinely-activatable system tree,"
  log "  and the target's raw bytes really changed on disk."
  log "======================================================================"
  return 0
}

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
  sysboot)
    run_sysboot_positive && exit 0
    exit 1
    ;;
  sysboot-negctl)
    run_sysboot_negctl && exit 0
    exit 1
    ;;
  gate)
    # vms-065: THE gate-invokable check -- ONE command, ONE clean exit code
    # (0 = VAX boots to a real DCL Username: prompt AND the negative control
    # has teeth; nonzero = not release-acceptance clean), bounded by
    # run_session's existing hard `timeout --kill-after` per SIMH stage
    # (SESSION_TIMEOUT, default 2700s each -- this mode runs at most two).
    # Runs the DCL-capstone positive proof (mastered SYSTEM volume boots PAST
    # the installed-system gate, through PROVISION.EXE demand-paging +
    # SYSTEM-identity + the logical-name layer + STARTUP.COM, to a real
    # interactive Username: prompt) THEN its negative control (the flat
    # volume, no DCL.EXE, MUST halt the gate) -- both must hold. This is the
    # single command a release-acceptance / frozen-verify caller invokes:
    #   tests/lab-vax/run-boot.sh gate
    if run_sysboot_positive && run_sysboot_negctl; then
      log "======================================================================"
      log "  GATE PASSED (vms-065): the VAX boot-to-DCL proof and its negative"
      log "  control both held -- SIMH/NetBSD-vax boots ovmx_init as PID 1 to a"
      log "  real interactive DCL Username: prompt on a mastered OVMX system"
      log "  volume, and the installed-system gate correctly refuses a volume"
      log "  with no DCL.EXE. Release-acceptance clean for the VAX runtime."
      log "======================================================================"
      exit 0
    fi
    die "GATE FAILED (vms-065) -- see console output above"
    ;;
  install)
    # vms-d0e5 rung G: the two-disk INSTALL proof -- boot the distribution
    # volume with a blank target attached, drive OVMX$INSTALL.COM to install
    # OVMX onto the target, and assert it worked (rooted, activatable, real
    # block writes). The vax mirror of the x86_64 install e2e.
    #   tests/lab-vax/run-boot.sh install
    run_install && exit 0
    exit 1
    ;;
  *) die "unknown mode '${MODE}' (want: prove | negctl | sysboot | sysboot-negctl | gate | install)" ;;
esac
