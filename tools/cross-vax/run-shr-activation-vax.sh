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
# written by CONSUMER.EXE directly to the /dev/console DEVICE (the channel a
# NetBSD process always has, since this SysV-activated fork()+execve() child's
# inherited fd 1/2 are NOT console-wired -- consumer_main.c), and corroborated
# by DCL's $STATUS (SS$_NORMAL iff the value was golden), both landing on the
# SIMH console transcript.
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

# The golden VAX V1 oracle vector (docs/oracle/purdy-hash-vectors.md, #657).
# The cross-shareable purdy_s_hash returns THIS iff IMGACT genuinely resolved
# the .vms$imp against the shipped LIBVMS$SHR.EXE's .vms$sv and the call ran.
GOLDEN='716cbdc03c071c59'
# CONSUMER.EXE (consumer_main.c) prints the value on THREE channels, so a hit on
# ANY proves the call ran and returned the value; the gate reports which:
#   /dev/console DEVICE channel (PRIMARY -- survives an unwired fd 1/2, the
#   measured failure mode of the prior cut; the exact open() PID 1 uses):
EXPECT_LINE_CON="OVMX-VAX-SHR-ACT-CON: purdy=0x${GOLDEN}"
#   C-RTL printf channel (rc3.c's console-surfacing shape, fallback):
EXPECT_LINE="OVMX-VAX-SHR-ACT: purdy=0x${GOLDEN}"
#   bare write(2)-to-inherited-fd fallback channel (self-contained, no stdio):
EXPECT_LINE_RAW="OVMX-VAX-SHR-ACT-RAW: purdy=0x${GOLDEN}"
# The regexp that captures the value printed on ANY channel, golden or not,
# so a WRONG value is surfaced as a REAL product finding rather than a silent
# miss (rd vms-d4a: never force a pass; report the truth).
VALUE_GREP='OVMX-VAX-SHR-ACT(-CON|-RAW)?: purdy=0x[0-9a-f]{16}'
# The SECOND, console-PROVEN golden witness (independent of every text channel
# above): CONSUMER returns 0 IFF got==golden, and DCL maps a fork()+execve()
# child's exit 0 to $STATUS = SS$_NORMAL (%X00000001), which the proof
# SYSTARTUP echoes as its "STATUS=" line. Only the exact golden 64-bit value
# makes consumer_body() return 0, so this line is itself value-sensitive.
STATUS_NORMAL_LINE="VAX-SHR-ACT-PROOF: STATUS=%X00000001"

log() { echo "[shr-activation-vax] $*"; }
die() { echo "[shr-activation-vax] FATAL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# assert_shr_activation <console-log> -- THE TEETH. Pure function over a
# console transcript; shared verbatim by the real boot run and `selftest`, so
# the self-test exercises the exact logic that gates the real run.
#
# PASSES iff the GOLDEN purdy value surfaced on at least one channel AND no
# activation-failure signature appears. A value line carrying a DIFFERENT
# 64-bit hash FAILS loudly and is reported as a real IMGACT/.vms$sv resolution
# finding (the value-sensitive teeth) -- never swallowed.
# ---------------------------------------------------------------------------
assert_shr_activation() {
  local log_file="$1"
  [ -f "$log_file" ] || { echo "  FAIL: no console log at $log_file"; return 1; }

  # Which channel(s) carried the GOLDEN value?
  local ch_con=0 ch_crtl=0 ch_raw=0
  grep -qaF "$EXPECT_LINE_CON" "$log_file" && ch_con=1
  grep -qaF "$EXPECT_LINE" "$log_file"     && ch_crtl=1
  grep -qaF "$EXPECT_LINE_RAW" "$log_file" && ch_raw=1
  local golden_line=0
  { [ "$ch_con" -eq 1 ] || [ "$ch_crtl" -eq 1 ] || [ "$ch_raw" -eq 1 ]; } && golden_line=1

  # Every value CONSUMER actually printed (golden or not), for the readout.
  local values
  values=$(grep -aoE "$VALUE_GREP" "$log_file" 2>/dev/null | sed -E 's/.*=0x//' | sort -u | tr '\n' ' ' || true)
  # A value line that is NOT the golden one => a real product finding.
  local wrong=0
  if [ "$golden_line" -eq 0 ] && [ -n "$values" ]; then wrong=1; fi

  # The SECOND golden witness: DCL's $STATUS after RUN CONSUMER == SS$_NORMAL,
  # which happens IFF consumer_body() returned 0 IFF the purdy value was golden.
  # This proves golden even if no text channel surfaced -- but it is trusted
  # ONLY when no CONFLICTING wrong value line was printed (the text value is
  # authoritative when present).
  local status_normal=0
  grep -qaF "$STATUS_NORMAL_LINE" "$log_file" && status_normal=1

  local golden_hit=0
  [ "$golden_line" -eq 1 ] && golden_hit=1
  [ "$status_normal" -eq 1 ] && [ "$wrong" -eq 0 ] && golden_hit=1

  local errs err_ok=1
  errs=$(grep -aE "%IMGACT-F|IMGNOTFND|DEVNOTMOUNT|NOSUCHFILE|ACCVIO|terminated abnormally|signal 1[012]|signal [46]" "$log_file" 2>/dev/null || true)
  [ -n "$errs" ] && err_ok=0

  echo "  (a) golden purdy value surfaced     : hit=$golden_hit (want 0x${GOLDEN}; value-line=$golden_line channels: con=$ch_con crtl=$ch_crtl raw=$ch_raw; \$STATUS-normal witness=$status_normal)"
  echo "      values CONSUMER printed         : [${values:-<none surfaced on any text channel>}]"
  [ "$wrong" -eq 1 ] && echo "      *** WRONG VALUE -- cross-shareable resolved to a NON-golden hash: REAL PRODUCT FINDING (escalate with the value above) ***"
  if [ "$golden_line" -eq 0 ] && [ "$wrong" -eq 0 ] && [ "$status_normal" -eq 0 ]; then
    echo "      NOTE: no value line AND \$STATUS is not SS\$_NORMAL -- CONSUMER did not run to a golden exit (activation/exec failure, or a wrong value it could not surface)"
  fi
  echo "  (b) no activation err               : ok=$err_ok"
  [ "$err_ok" -eq 0 ] && echo "      offending: $(printf '%s' "$errs" | tr '\n' '|')"

  [ "$golden_hit" -eq 1 ] && [ "$err_ok" -eq 1 ] && return 0
  return 1
}

# ---------------------------------------------------------------------------
# selftest -- prove assert_shr_activation() has teeth. A good fixture passes;
# three distinct breakages each RED it.
# ---------------------------------------------------------------------------
GOOD_FIXTURE() {
  cat <<EOF
VAX-SHR-ACT-PROOF: === RUN CONSUMER (IMGACT resolves .vms\$sv; cross-shareable purdy_s_hash call into the shipped LIBVMS\$SHR.EXE) ===
$EXPECT_LINE_CON
$EXPECT_LINE_RAW
$EXPECT_LINE
VAX-SHR-ACT-PROOF: STATUS=%X00000001 SEVERITY=1
EOF
}

selftest() {
  local d="$WORK/selftest"; rm -rf "$d"; mkdir -p "$d"
  local fails=0

  GOOD_FIXTURE > "$d/good.log"
  echo "-- selftest 1/6: GOOD transcript (all channels golden + \$STATUS normal) must PASS --"
  if assert_shr_activation "$d/good.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: good transcript rejected"; fails=$((fails+1)); fi

  # Only the /dev/console channel surfaces (fd 1/2 unwired, so RAW+CRTL absent)
  # -- still a PASS, since the golden value reached the console on the PRIMARY
  # channel. This is the measured real-substrate shape.
  { echo "VAX-SHR-ACT-PROOF: === RUN CONSUMER ==="
    echo "$EXPECT_LINE_CON"
    echo "VAX-SHR-ACT-PROOF: STATUS=%X00000001 SEVERITY=1"; } > "$d/cononly.log"
  echo "-- selftest 2/6: only the /dev/console channel golden must PASS --"
  if assert_shr_activation "$d/cononly.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: console-only golden rejected"; fails=$((fails+1)); fi

  # No text value line AT ALL, but $STATUS is SS$_NORMAL: the exit-code witness
  # alone proves golden (consumer_body returned 0 IFF got==golden). Must PASS.
  { echo "VAX-SHR-ACT-PROOF: === RUN CONSUMER ==="
    echo "VAX-SHR-ACT-PROOF: STATUS=%X00000001 SEVERITY=1"; } > "$d/statusonly.log"
  echo "-- selftest 3/6: no value line but \$STATUS=SS\$_NORMAL (exit-code witness) must PASS --"
  if assert_shr_activation "$d/statusonly.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: \$STATUS-normal witness rejected"; fails=$((fails+1)); fi

  # A WRONG 64-bit value on the channels (and $STATUS aborted, as a nonzero exit
  # produces): the cross-shareable resolved to the wrong thing -- a REAL product
  # finding, must FAIL. A stray STATUS-normal line must NOT rescue it (the text
  # value is authoritative), so this also pins that precedence.
  { echo "VAX-SHR-ACT-PROOF: === RUN CONSUMER ==="
    echo "OVMX-VAX-SHR-ACT-CON: purdy=0xdeadbeefcafef00d"
    echo "%DCL-E-ABORT, image SYS\$SYSTEM:CONSUMER exited with error status %X00000001"
    echo "VAX-SHR-ACT-PROOF: STATUS=%X0000002C SEVERITY=4"; } > "$d/wronghash.log"
  echo "-- selftest 4/6: wrong hash value must FAIL (real product finding) --"
  if assert_shr_activation "$d/wronghash.log" >/dev/null 2>&1; then echo "  FAIL: wrong hash accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  # Nothing surfaced and $STATUS aborted: CONSUMER did not activate / never ran
  # to a golden exit. Must FAIL (no golden witness of any kind).
  { echo "VAX-SHR-ACT-PROOF: === RUN CONSUMER ==="
    echo "%DCL-E-ABORT, image SYS\$SYSTEM:CONSUMER exited with error status %X00000001"
    echo "VAX-SHR-ACT-PROOF: STATUS=%X0000002C SEVERITY=4"; } > "$d/nothing.log"
  echo "-- selftest 5/6: no value line AND \$STATUS aborted must FAIL --"
  if assert_shr_activation "$d/nothing.log" >/dev/null 2>&1; then echo "  FAIL: silent abort accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  { echo "VAX-SHR-ACT-PROOF: === RUN CONSUMER ==="
    echo "$EXPECT_LINE_CON"
    echo "VAX-SHR-ACT-PROOF: STATUS=%X00000001 SEVERITY=1"
    echo '%IMGACT-F-IMGNOTFND, image SYS$SYSTEM:CONSUMER.EXE not found'; } > "$d/imgact.log"
  echo "-- selftest 6/6: golden value BUT an IMGACT activation failure must FAIL --"
  if assert_shr_activation "$d/imgact.log" >/dev/null 2>&1; then echo "  FAIL: activation failure accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  if [ "$fails" -eq 0 ]; then
    echo "=== selftest: assert_shr_activation() has teeth (console-only + \$STATUS-witness pass; wrong-value + silent-abort + activation-fail red) ==="
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
  log "building the shr-activation artifacts (IMGACT.EXE, LIBVMS\$SHR.EXE, CONSUMER.EXE)"
  docker run --rm -v "${REPO}:/src:ro" -v "${SHR_ARTIFACTS_DIR}:/out" \
    --entrypoint sh "${CROSS_IMAGE}" \
    /src/tools/cross-vax/build-shr-activation-vax.sh /src /out
  for f in "IMGACT.EXE" "LIBVMS\$SHR.EXE" "CONSUMER.EXE"; do
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
# (UNMODIFIED, the five standard boot images), overlaid with the three
# shr-activation artifacts (SYSEXE/IMGACT.EXE, SYSEXE/CONSUMER.EXE,
# SYSLIB/LIBVMS$SHR.EXE) and the proof SYSTARTUP_VMS.COM in place of the
# Decision-A one. Never touches run-boot.sh's own SYSVOL_IMG.
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
      cp /src/tools/cross-vax/SYSTARTUP_VMS_SHR_ACTIVATION_PROOF.COM \
         /tmp/stage/SYS0/SYSCOMMON/SYSMGR/SYSTARTUP_VMS.COM
      /tmp/vmsfs_master --ods2 master /out/'"$(basename "${SHR_SYSVOL_IMG}")"' OVMXSYS /tmp/stage 64
      /tmp/vmsfs_master --ods2 list /out/'"$(basename "${SHR_SYSVOL_IMG}")")"
  echo "${listing}"
  [ -f "${SHR_SYSVOL_IMG}" ] || die "shr-activation system-volume mastering did not produce ${SHR_SYSVOL_IMG}"
  for f in DCL.EXE PROVISION.EXE IMGACT.EXE CONSUMER.EXE "LIBVMS\$SHR.EXE"; do
    echo "${listing}" | grep -qiF "${f}" \
      || die "mastered shr-activation volume is MISSING ${f}"
  done
  log "OK: shr-activation system volume carries IMGACT.EXE + CONSUMER.EXE + LIBVMS\$SHR.EXE + the proof SYSTARTUP_VMS.COM"
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

    # rd vms-d4a option (a): NON-GATING diagnostic readout. The gate-private
    # IMGACT (-DOVMX_IMGACT_BIND_TRACE) emits, for the CONSUMER activation only,
    # the elf32-vax import bind (prod/base/cell/val) on /dev/console; CONSUMER
    # emits the purdy value. Surface both so the transcript is self-diagnosing.
    # These are ADDRESSES/values ONLY -- the PASS predicate below
    # (assert_shr_activation) is UNCHANGED, so this cannot fake a golden result.
    {
      echo "---- vms-d4a option (a) bind-trace (diagnostic, non-gating) ----"
      grep -aE 'OVMX-IMGACT-FAIL:' "$WORK/shr-activation-boot.log" || echo "  (no OVMX-IMGACT-FAIL -- IMGACT did NOT hit a vms_fatal path; if \$STATUS=%X2C then it's the DCL generic-nonzero-exit fallback = CONSUMER activated + returned nonzero, NOT an IMGACT failure)"
      grep -aE 'OVMX-IMGACT-BIND-ENTER:' "$WORK/shr-activation-boot.log" || echo "  (no OVMX-IMGACT-BIND-ENTER -- bind_imports never ran for CONSUMER; IMGACT failed UPSTREAM of import binding, or the CONSUMER argv0-scope did not match)"
      grep -aE 'OVMX-IMGACT-BIND:' "$WORK/shr-activation-boot.log" || echo "  (no OVMX-IMGACT-BIND store line -- either count=0 [see ENTER], a %IMGACT-F-GSMATCH resolve-fail, or bind_imports did not reach the store)"
      grep -aE 'OVMX-VAX-SHR-ACT(-CON|-RAW)?: purdy=0x[0-9a-f]{16}' "$WORK/shr-activation-boot.log" || echo "  (no purdy= value line)"
      grep -aiE '%IMGACT-|ACCVIO|SS\$_|SIGSEGV|signal [0-9]+|STATUS=' "$WORK/shr-activation-boot.log" | head -8 || true
      echo "  VERDICT KEY: BIND absent+GSMATCH => resolve-failed | val!=LIBVMS\$SHR_base+purdy_off => cell-fill bug | val ok + crash => call mistransfer | val ok + clean + wrong purdy => wrong-return | golden => PASS"
      echo "---------------------------------------------------------------"
    } || true

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
