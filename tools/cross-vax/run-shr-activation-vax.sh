#!/bin/bash
# run-shr-activation-vax.sh - vms-d4a (vms-404 P4), the ANTI-LARP RUNTIME
# proof: a shipped OVMX VAX shareable ACTIVATES on the SIMH VAX rail.
#
# WHAT THIS PROVES (a REAL activation, not a build-only readelf-shape check,
# which P3a/vms-099 already covers): boot the assembled OVMX/NetBSD-vax
# runtime under SIMH, and have IMGACT.EXE activate CONSUMER.EXE (elf32-vax,
# LINKVAX.EXE `--executable`, PT_INTERP=IMGACT.EXE) which imports
# `purdy_s_hash` from the SHIPPED LIBVMS$SHR.EXE (rd vms-c7f7) -- a REAL
# cross-shareable universal-symbol call whose result is VALUE-SENSITIVE: the
# real OpenVMS "VAX V1" oracle vector (docs/oracle/purdy-hash-vectors.md),
# printed over a REAL write(2) syscall (P4BOOT$SHR.EXE's p4boot_puts, this
# gate's tiny exit/write runtime shim -- LIBVMS$SHR.EXE rightly exports
# neither), landing on the SIMH console transcript.
#
# WHY NOT $STATUS/OVMX-SEAM (the Alpha shipped-shareable gate's discipline,
# rd vms-410/#1075, tools/cross-alpha/run-module-gp-activation-alpha.sh
# `shipped-gate`): VAX consumer images are activated SysV-flavor, never
# VMS-standard (src/imgact/arch/vax/start.S: "mirror x86_64/aarch64's SysV
# tail-jump, NOT Alpha's PDSC trampoline"; src/imgact/imgact.c's
# imgact_vms_standard_activate is explicitly `#else ... "VMS-standard image
# activation is Alpha-only" -- fail honest, never mis-transfer` on every
# non-Alpha arch). A SysV image's $STATUS is executive-recorded only at
# in-process rundown (src/libvms/syssvc/sys_imgact.c) as a COLLAPSED verdict
# -- SS$_ACCVIO / SS$_NORMAL / SS$_ABORT -- never a value-sensitive encoding.
# So this gate mirrors the Alpha discipline's INTENT (a value that only
# appears if the real cross-shareable call really ran) over the channel VAX
# actually has: the consumer's own console print.
#
# SUBSTRATE (rd vms-d59/vms-065's "boots to DCL" proof, tests/lab-vax/
# run-boot.sh `sysboot`): this script calls that UNMODIFIED, fully-gated
# proof first (it assembles the ovmx_init+modules boot disk, the MODULAR
# kernel, and the standard mastered ODS-2 system volume, all cached) --
# proving the ordinary substrate is sound BEFORE layering this gate's own
# volume/SYSTARTUP on top. It then masters ITS OWN system-volume file
# (never overwriting run-boot.sh's own cached SYSVOL_IMG) carrying the four
# shr-activation artifacts (build-shr-activation-vax.sh) plus a proof
# SYSTARTUP_VMS.COM (SYSTARTUP_VMS_SHR_ACTIVATION_PROOF.COM) that RUNs
# CONSUMER.EXE during STDRV, mirroring tools/cross-alpha/
# SYSTARTUP_VMS_JOINT_PROOF.COM's shape -- then boots THAT volume with the
# SAME drive_boot_vax.py `sysboot` driver (UNMODIFIED; it already asserts the
# boot reaches a real interactive Username: prompt, the DCL capstone).
#
# PROVE-CAN-FAIL (Rule 7): `selftest` drives the SAME assert_shr_activation()
# the real gate uses against crafted console-log fixtures -- a good
# transcript passes; a wrong hash, a missing line, an IMGACT activation
# failure, and an activated-but-crashed image each RED it.
#
# Rule 9: BUILD/TEST tooling only, fully containerized (ovmx-cross-vax /
# ovmx-vax-lab); nothing here is a runtime. Nothing installed on the host.
#
# USAGE:
#   tools/cross-vax/run-shr-activation-vax.sh selftest   # can-fail proof, no boot
#   tools/cross-vax/run-shr-activation-vax.sh build       # artifacts + readelf-shape only, no boot
#   tools/cross-vax/run-shr-activation-vax.sh gate        # the full runtime proof (default)
#
# EXIT: 0 iff the shipped LIBVMS$SHR.EXE cross-shareable call really ran on
# the real SIMH VAX rail and the boot reached its DCL capstone; nonzero
# otherwise. Never weakened to pass; a genuine runtime blocker is reported
# with the exact console log, not papered over.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"                  # tools/cross-vax
REPO="$(cd "$HERE/../.." && pwd)"
MODE="${1:-gate}"

CROSS_IMAGE="${CROSS_IMAGE:-ovmx-cross-vax}"
LAB_IMAGE="${LAB_IMAGE:-ovmx-vax-lab}"

# Dedicated cache root, isolated from run-boot.sh's OWN artifacts (SYSVOL_IMG,
# etc.) except where we DELIBERATELY read its outputs (the standard boot-work
# disk + the cross-built boot image set) -- never write into them.
GATE_ROOT="${GATE_ROOT:-$REPO/.boot-cache/lab-vax}"
CACHE_DIR="$GATE_ROOT"
SYSVOL_IMAGES_DIR="${CACHE_DIR}/sysvol-images"
BOOT_WORKDIR="${CACHE_DIR}/boot-work"
SHR_ARTIFACTS_DIR="${CACHE_DIR}/shr-activation-artifacts"
SHR_SYSVOL_IMG="${CACHE_DIR}/ovmx-shr-activation-sysvol-vax.img"
WORK="${WORK:-$GATE_ROOT/shr-activation}"
mkdir -p "$WORK"

BOOT_TIMEOUT="${BOOT_TIMEOUT:-1800}"
SESSION_TIMEOUT="${SESSION_TIMEOUT:-2700}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

EXPECT_LINE='OVMX-VAX-SHR-ACT: purdy=0x716cbdc03c071c59'

log() { echo "[shr-activation-vax] $*"; }
die() { echo "[shr-activation-vax] FATAL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# assert_shr_activation <console-log> -- THE TEETH. Pure function over a
# console transcript; shared verbatim by the real boot run and `selftest`, so
# the self-test exercises the exact logic that gates the real run.
# ---------------------------------------------------------------------------
assert_shr_activation() {
  local log_file="$1"
  [ -f "$log_file" ] || { echo "  FAIL: no console log at $log_file"; return 1; }

  local hit=0
  grep -qaF "$EXPECT_LINE" "$log_file" && hit=1

  local errs err_ok=1
  errs=$(grep -aE "%IMGACT-F|IMGNOTFND|DEVNOTMOUNT|NOSUCHFILE|ACCVIO|terminated abnormally|signal 1[012]|signal [46]" "$log_file" 2>/dev/null || true)
  [ -n "$errs" ] && err_ok=0

  echo "  (a) value-sensitive purdy hash line : hit=$hit (want '$EXPECT_LINE')"
  echo "  (b) no activation err               : ok=$err_ok"
  [ "$err_ok" -eq 0 ] && echo "      offending: $(printf '%s' "$errs" | tr '\n' '|')"

  [ "$hit" -eq 1 ] && [ "$err_ok" -eq 1 ] && return 0
  return 1
}

# ---------------------------------------------------------------------------
# selftest -- prove assert_shr_activation() has teeth. A good fixture passes;
# three distinct breakages each RED it.
# ---------------------------------------------------------------------------
GOOD_FIXTURE() {
  cat <<EOF
VAX-SHR-ACT-PROOF: === RUN CONSUMER (IMGACT resolves .vms\$sv; cross-shareable purdy_s_hash call into the shipped LIBVMS\$SHR.EXE) ===
$EXPECT_LINE
VAX-SHR-ACT-PROOF: STATUS=%X00000001 SEVERITY=1
EOF
}

selftest() {
  local d="$WORK/selftest"; rm -rf "$d"; mkdir -p "$d"
  local fails=0

  GOOD_FIXTURE > "$d/good.log"
  echo "-- selftest 1/4: GOOD transcript must PASS --"
  if assert_shr_activation "$d/good.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: good transcript rejected"; fails=$((fails+1)); fi

  GOOD_FIXTURE | sed 's/716cbdc03c071c59/deadbeefcafef00d/' > "$d/wronghash.log"
  echo "-- selftest 2/4: wrong hash value must FAIL --"
  if assert_shr_activation "$d/wronghash.log" >/dev/null 2>&1; then echo "  FAIL: wrong hash accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  GOOD_FIXTURE | grep -v "purdy=" > "$d/missing.log"
  echo "-- selftest 3/4: missing sentinel line must FAIL --"
  if assert_shr_activation "$d/missing.log" >/dev/null 2>&1; then echo "  FAIL: missing sentinel accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  { echo "VAX-SHR-ACT-PROOF: === RUN CONSUMER ==="
    echo '%IMGACT-F-IMGNOTFND, image SYS$SYSTEM:CONSUMER.EXE not found'; } > "$d/imgact.log"
  echo "-- selftest 4/4: IMGACT activation failure (IMGNOTFND) must FAIL --"
  if assert_shr_activation "$d/imgact.log" >/dev/null 2>&1; then echo "  FAIL: activation failure accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  if [ "$fails" -eq 0 ]; then
    echo "=== selftest: assert_shr_activation() has teeth (good passes, all 3 breakages red) ==="
    return 0
  fi
  echo "=== selftest FAILED: $fails case(s) wrong -- the gate cannot be trusted ==="
  return 1
}

ensure_images() {
  docker image inspect "${CROSS_IMAGE}" >/dev/null 2>&1 || {
    log "building ${CROSS_IMAGE}"
    docker build -f "${REPO}/tools/cross-vax/Dockerfile" -t "${CROSS_IMAGE}" "${REPO}/tools/cross-vax"; }
  docker image inspect "${LAB_IMAGE}" >/dev/null 2>&1 || {
    log "building ${LAB_IMAGE}"
    docker build -f "${REPO}/tests/lab-vax/Dockerfile" -t "${LAB_IMAGE}" "${REPO}/tests/lab-vax"; }
}

# Build the four shr-activation artifacts (build-shr-activation-vax.sh).
build_shr_artifacts() {
  mkdir -p "$SHR_ARTIFACTS_DIR"
  log "building the shr-activation artifacts (IMGACT.EXE, LIBVMS\$SHR.EXE, P4BOOT\$SHR.EXE, CONSUMER.EXE)"
  docker run --rm -v "${REPO}:/src:ro" -v "${SHR_ARTIFACTS_DIR}:/out" \
    --entrypoint sh "${CROSS_IMAGE}" \
    /src/tools/cross-vax/build-shr-activation-vax.sh /src /out
  for f in "IMGACT.EXE" "LIBVMS\$SHR.EXE" "P4BOOT\$SHR.EXE" "CONSUMER.EXE"; do
    [ -s "${SHR_ARTIFACTS_DIR}/${f}" ] || die "shr-activation artifact missing after build: ${f}"
  done
}

# Reuse run-boot.sh's OWN prerequisite chain: the standard `sysboot` proof
# (rd vms-065/vms-d59) fully assembles + caches the ovmx_init boot-work disk,
# the MODULAR kernel, and the standard cross-built boot image set (in the
# SAME CACHE_DIR this script uses) -- proving the ordinary substrate is sound
# BEFORE this gate layers its own volume/SYSTARTUP on top. Idempotent
# (cached) on repeat runs.
ensure_substrate() {
  log "priming the substrate: tests/lab-vax/run-boot.sh sysboot (cached; the standard vms-065 boot-to-DCL proof)"
  CACHE_DIR="${CACHE_DIR}" "${REPO}/tests/lab-vax/run-boot.sh" sysboot \
    || die "the STANDARD sysboot proof (run-boot.sh) failed -- the ordinary VAX boot substrate is not sound; this gate's own volume cannot be expected to boot either. See the run-boot.sh output above."
  for img in DCL.EXE PROVISION.EXE LOGINOUT.EXE JOB_CONTROL.EXE STARTUP.EXE; do
    [ -f "${SYSVOL_IMAGES_DIR}/${img}" ] || die "expected cached boot image missing: ${SYSVOL_IMAGES_DIR}/${img}"
  done
  [ -f "${BOOT_WORKDIR}/wd0.img" ] || die "expected assembled boot-work disk missing: ${BOOT_WORKDIR}/wd0.img"
}

# Master THIS gate's own system-volume file: stage_sysvol.sh's ordinary tree
# (UNMODIFIED, the five standard boot images), overlaid with the four
# shr-activation artifacts (SYSEXE/IMGACT.EXE, SYSEXE/CONSUMER.EXE,
# SYSLIB/LIBVMS$SHR.EXE, SYSLIB/P4BOOT$SHR.EXE) and the proof SYSTARTUP_VMS.COM
# in place of the Decision-A one. Never touches run-boot.sh's own SYSVOL_IMG.
master_shr_activation_volume() {
  log "mastering the shr-activation system volume (stage_sysvol.sh + shr-activation overlay + vmsfs_master)"
  rm -f "${SHR_SYSVOL_IMG}"
  local listing
  listing="$(docker run --rm -v "${REPO}:/src:ro" -v "${SYSVOL_IMAGES_DIR}:/images:ro" \
    -v "${SHR_ARTIFACTS_DIR}:/shr:ro" -v "$(dirname "${SHR_SYSVOL_IMG}")":/out \
    --entrypoint sh "${CROSS_IMAGE}" -c '
      set -e
      cc -O2 -Wall -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
         -I /src/src/vmsfs/include \
         -o /tmp/vmsfs_master /src/tools/vmsfs_master.c \
         /src/src/vmsfs/ods2/ods2_reader.c /src/src/vmsfs/ods2/ods2_writer.c \
         /src/src/vmsfs/ods2/ods2_edit.c /src/src/vmsfs/ods2/ods2_bdev.c \
         /src/src/vmsfs/ods2/ods2_path.c /src/src/vmsfs/ods2/ods2_block_posix.c
      bash /src/tests/lab-vax/stage_sysvol.sh /images /src /tmp/stage
      mkdir -p /tmp/stage/SYS0/SYSCOMMON/SYSLIB
      cp "/shr/IMGACT.EXE"      /tmp/stage/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE
      cp "/shr/CONSUMER.EXE"    /tmp/stage/SYS0/SYSCOMMON/SYSEXE/CONSUMER.EXE
      cp "/shr/LIBVMS\$SHR.EXE" "/tmp/stage/SYS0/SYSCOMMON/SYSLIB/LIBVMS\$SHR.EXE"
      cp "/shr/P4BOOT\$SHR.EXE" "/tmp/stage/SYS0/SYSCOMMON/SYSLIB/P4BOOT\$SHR.EXE"
      cp /src/tools/cross-vax/SYSTARTUP_VMS_SHR_ACTIVATION_PROOF.COM \
         /tmp/stage/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM
      /tmp/vmsfs_master --ods2 master /out/'"$(basename "${SHR_SYSVOL_IMG}")"' OVMXSYS /tmp/stage 64
      /tmp/vmsfs_master --ods2 list /out/'"$(basename "${SHR_SYSVOL_IMG}")")"
  echo "${listing}"
  [ -f "${SHR_SYSVOL_IMG}" ] || die "shr-activation system-volume mastering did not produce ${SHR_SYSVOL_IMG}"
  for f in DCL.EXE PROVISION.EXE IMGACT.EXE CONSUMER.EXE "LIBVMS\$SHR.EXE" "P4BOOT\$SHR.EXE"; do
    echo "${listing}" | grep -qiF "${f}" \
      || die "mastered shr-activation volume is MISSING ${f}"
  done
  log "OK: shr-activation system volume carries IMGACT.EXE + CONSUMER.EXE + LIBVMS\$SHR.EXE + P4BOOT\$SHR.EXE + the proof SYSTARTUP_VMS.COM"
}

# Boot the standard boot-work disk (rq0) with THIS gate's own mastered
# volume attached on rq1 -> ra1 -> DUA0:, via the SAME UNMODIFIED
# drive_boot_vax.py `sysboot` mode run-boot.sh's positive proof uses --
# it already asserts the boot reaches a real interactive Username: prompt
# (the DCL capstone, vms-d59), so a boot that gets there really ran STDRV
# (and therefore this gate's proof SYSTARTUP_VMS.COM) to completion.
boot_and_capture() {
  local cid="ovmx-vax-shr-activation-$$"
  local out="$WORK/shr-activation-boot.log"
  rm -f "$out"
  log "booting boot-work (ovmx_init) with the shr-activation system volume on rq1 (deadline ${SESSION_TIMEOUT}s)"
  local rc=0
  set +e
  timeout --kill-after="${TIMEOUT_GRACE}" "${SESSION_TIMEOUT}" \
    docker run --rm --name "${cid}" --entrypoint python3 \
      -e OVMX_MODE=sysboot -e OVMX_NETBSD_DIR=/netbsd \
      -e NETBSD_WORKDIR=/cache/boot-work -e NETBSD_BOOT_DEADLINE="${BOOT_TIMEOUT}" \
      -e OVMX_SYSVOL_IMG=/cache/"$(basename "${SHR_SYSVOL_IMG}")" \
      -v "${CACHE_DIR}:/cache" \
      -v "${REPO}/tests/netbsd:/netbsd:ro" -v "${REPO}/tests/lab-vax:/lab-vax:ro" \
      "${LAB_IMAGE}" /lab-vax/drive_boot_vax.py 2>&1 | tee "$out"
  rc=${PIPESTATUS[0]}
  set -e
  docker kill "${cid}" >/dev/null 2>&1 || true
  [ -f "$out" ] || die "shr-activation BOOT produced no console log (SIMH never started?)"
  return "$rc"
}

case "$MODE" in
  selftest)
    selftest
    ;;
  build)
    ensure_images
    build_shr_artifacts
    log "BUILD-ONLY PASSED: shr-activation artifacts built + readelf-shape verified (no boot)."
    ;;
  gate)
    log "verifying the gate can fail (selftest) before the real boot"
    selftest || die "selftest failed -- assert_shr_activation() cannot be trusted; aborting before the boot"

    ensure_images
    build_shr_artifacts
    ensure_substrate
    master_shr_activation_volume

    boot_rc=0
    boot_and_capture || boot_rc=$?
    log "boot driver exit code: ${boot_rc} (0 = drive_boot_vax.py's OWN sysboot verdict -- reached the real DCL Username: prompt)"

    if assert_shr_activation "$WORK/shr-activation-boot.log"; then
      if [ "$boot_rc" -eq 0 ]; then
        log "======================================================================"
        log "  VAX SHR-ACTIVATION PASSED (rd vms-d4a): IMGACT.EXE activated the"
        log "  shipped-graph CONSUMER.EXE on the real SIMH VAX rail; the cross-"
        log "  shareable purdy_s_hash call into the SHIPPED LIBVMS\$SHR.EXE ran and"
        log "  printed the real OpenVMS oracle vector's value:"
        log "    $EXPECT_LINE"
        log "  and the boot reached its DCL Username: capstone."
        log "======================================================================"
        exit 0
      fi
      die "the value-sensitive sentinel appeared but the boot's OWN sysboot verdict did not (exit ${boot_rc}) -- the boot did not reach the DCL capstone (Username:) even though STDRV ran the proof; see the console log above ($WORK/shr-activation-boot.log)"
    fi
    die "VAX SHR-ACTIVATION FAILED (rd vms-d4a): the value-sensitive sentinel '$EXPECT_LINE' did not appear (or an activation-failure signature did) -- see the console log above ($WORK/shr-activation-boot.log). This is a REAL runtime finding, not weakened to pass."
    ;;
  *) die "unknown mode '$MODE' (want: selftest | build | gate)" ;;
esac
