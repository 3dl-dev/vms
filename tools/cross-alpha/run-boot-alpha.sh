#!/usr/bin/env bash
#
# run-boot-alpha.sh -- rd vms-359 (Alpha[A6]), the gate-invokable, clean
# pass/fail check a release-acceptance / frozen-verify caller invokes to
# confirm the Alpha runtime still boots to a real interactive DCL
# `Username:` prompt under qemu-system-alpha -- the exact analog of what
# tests/lab-vax/run-boot.sh's `gate` mode does for VAX (rd vms-065).
#
# WHAT IT PROVES (BOOT A only, from boot-alpha-image.sh -- see that script's
# header for the full milestone chain): kernel up -> vms.ko attaches
# /dev/vms -> vmsfs.ko mounts the VMSFS system disk on DKA0: -> PID 1
# (STARTUP.EXE) execs PROVISION.EXE (SYSTEM [1,4] identity established) ->
# STARTUP.COM -> JOB_CONTROL -> LOGINOUT -> a real interactive DCL
# `Username:` prompt (INV-6-clean -- not a printf). BOOT B (the IMGACT
# capability proof) is a SEPARATE rung-A2 check, not part of the
# "boots to DCL" claim this gate exists to protect, so it is deliberately
# NOT run here -- fewer moving parts is fewer flake sources (Rule 8).
#
# EXIT CONTRACT: 0 IFF the Alpha runtime reaches a real DCL `Username:`
# prompt within the hard per-boot timeout; nonzero otherwise (build failure,
# boot hang/timeout, kernel panic, or the prompt simply never appearing).
# The `Username:` assertion is never weakened to make this pass.
#
# ISOLATION (Rule 9 / shared-host hygiene): this script NEVER reads or
# writes boot-alpha-image.sh's / build-alpha-bootimage.sh's own AMBIENT
# default caches (/tmp/ovmx-alpha-boot, /tmp/ovmx-vmsko-alpha) -- a
# developer or another agent may be using those for manual debugging on a
# shared host at any time. It uses its OWN cache root (GATE_ROOT, default
# ${REPO}/.boot-cache/alpha-gate), mirroring tests/lab-vax/run-boot.sh's own
# ${REPO}/.boot-cache/lab-vax convention, so a concurrent session can
# neither poison nor be poisoned by a gate run.
#
# CACHING (mirrors the VAX gate's "cached kernel/disk" pattern, rd vms-065):
# the FULL Alpha boot-image assembly (Linux/Alpha kernel cross-build, OVMX
# Alpha userland cross-build, VMSFS system-disk mastering, initramfs bake --
# collectively ~20-30 minutes) runs ONCE and is cached under GATE_ROOT/{vmsko,
# userland,boot}. This is safe to reuse across repeated gate invocations
# because none of it is what the gate is testing -- the gate's OWN subject is
# the BOOT (does PID 1 reach a real DCL prompt on THIS kernel+disk), which is
# re-run in full, unmodified, from a fresh disk copy, every single time (see
# run_one_boot). A cached kernel/disk is exactly as suspect to a boot-chain
# regression as a freshly-built one; only the (expensive, unrelated) act of
# cross-compiling them is skipped. Set FORCE_BUILD=1 to force a rebuild.
#
# HARNESS GOTCHA (harden-hardening note): any kernel re-bake needs
# `export ARCH=alpha` -- build-alpha-bootimage.sh (invoked unmodified by
# ensure_artifacts below) already does this internally; nothing in this
# script re-invokes the kernel build directly, so there is nothing here to
# get that wrong, but it stays load-bearing if this script ever grows one.
#
# NOT ON THE PER-PR PATH. This gate is release-acceptance / frozen-verify
# tooling, invoked by the conductor at cut time on the frozen SHA -- exactly
# like tests/lab-vax/run-boot.sh gate. It is deliberately not wired into
# .github/workflows/ci.yml (keeps the slow emulator boot off every PR).
#
# USAGE:
#   tools/cross-alpha/run-boot-alpha.sh              # gate mode (default)
#   tools/cross-alpha/run-boot-alpha.sh gate          # same, explicit
#   tools/cross-alpha/run-boot-alpha.sh build          # just (re)build the cache
#   tools/cross-alpha/run-boot-alpha.sh boot           # one real boot, verbose verdict
#   tools/cross-alpha/run-boot-alpha.sh validate [N]   # N consecutive real boots
#                                                       # (reliability validation; default N=5)
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
MODE="${1:-gate}"

KV="${KV:-6.6.52}"
GATE_ROOT="${GATE_ROOT:-$REPO/.boot-cache/alpha-gate}"
export VMSKO_WORK="${VMSKO_WORK:-$GATE_ROOT/vmsko}"
export USERLAND="${USERLAND:-$GATE_ROOT/userland}"
export WORK="${WORK:-$GATE_ROOT/boot}"
export KV

# Hard bound on a SINGLE qemu-system-alpha boot (Rule 9: a hung
# qemu-system-alpha can run for hours). Matches boot-alpha-image.sh's own
# BOOT_TIMEOUT default; the observed real boot-to-Username: time is well
# under a minute, so 200s carries generous headroom without risking a false
# red on a merely-slow host.
BOOT_TIMEOUT="${BOOT_TIMEOUT:-200}"
# Outer wrapper around the whole `docker run` (image-pull/start overhead +
# BOOT_TIMEOUT + teardown); a second, independent bound so a wedged docker
# daemon can't hang this gate even if the in-container `timeout` somehow
# didn't fire.
DOCKER_TIMEOUT="${DOCKER_TIMEOUT:-$((BOOT_TIMEOUT + 150))}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"
IMG="ovmx-cross-alpha"

log() { echo "[run-boot-alpha] $*"; }
die() { echo "[run-boot-alpha] FATAL: $*" >&2; exit 1; }

ensure_artifacts() {
  if [ "${FORCE_BUILD:-0}" != "1" ] \
     && [ -f "$WORK/vmlinux-boot" ] && [ -f "$WORK/ovmx-distrib-alpha.img" ]; then
    log "boot artifacts cached in $WORK -- NOT rebuilding (set FORCE_BUILD=1 to force)"
    return 0
  fi
  log "assembling boot artifacts (VMSKO_WORK=$VMSKO_WORK USERLAND=$USERLAND WORK=$WORK)"
  log "this is the slow one-time step (~20-30 min: kernel cross-build + userland + disk master)"
  "$HERE/build-alpha-bootimage.sh"
  [ -f "$WORK/vmlinux-boot" ] && [ -f "$WORK/ovmx-distrib-alpha.img" ] \
    || die "build-alpha-bootimage.sh finished but artifacts missing from $WORK"
}

# run_one_boot <tag> -- ONE real, independent boot: a fresh private disk copy,
# a hard-timeout qemu-system-alpha run of BOOT A (console-CR driven exactly as
# boot-alpha-image.sh's BOOT A -- LOGINOUT waits for an operator RETURN on
# OPA0:, vms-3f6/vms-2213), verdict = did the filtered console log contain a
# real "Username:" prompt. Returns 0/1; never `exit`s (composable, and used in
# a loop by `validate`).
run_one_boot() {
  local tag="$1"
  rm -f "$WORK/gate-${tag}.img" "$WORK/gate-${tag}.raw" "$WORK/gate-${tag}.log" "$WORK/gate-${tag}.fifo"
  local cname="ovmx-alpha-gate-${tag}-$$"
  local rc=0
  set +e
  timeout --kill-after="$TIMEOUT_GRACE" "$DOCKER_TIMEOUT" docker run --rm \
    --name "$cname" --memory=8g --cpus="$(nproc)" \
    -v "$WORK":/work "$IMG" bash -euo pipefail -c '
      BT="'"$BOOT_TIMEOUT"'"; TAG="'"$tag"'"
      cd /work
      cp ovmx-distrib-alpha.img "gate-${TAG}.img"
      FIFO="/work/gate-${TAG}.fifo"; mkfifo "$FIFO"
      timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
          -kernel vmlinux-boot -append "console=ttyS0 panic=-1" \
          -drive file="gate-${TAG}.img",format=raw,if=virtio \
          -nographic -no-reboot <"$FIFO" > "gate-${TAG}.raw" 2>&1 &
      QP=$!
      exec 6>"$FIFO"
      trap "" PIPE   # a CR fed just as the guest exits must not kill this shell
      W=0
      while kill -0 "$QP" 2>/dev/null; do
          grep -qaF "Username:" "gate-${TAG}.raw" 2>/dev/null && break
          printf "\r" >&6 2>/dev/null || true
          sleep 2; W=$((W + 2))
          [ "$W" -ge "$BT" ] && break
      done
      exec 6>&-
      sleep 2                # let LOGINOUT flush the prompt after the wake CR
      kill "$QP" 2>/dev/null || true
      wait "$QP" 2>/dev/null || true
      rm -f "$FIFO"
      grep -avE "TSUNAMI machine check|tsunami_(read|write)" "gate-${TAG}.raw" > "gate-${TAG}.log" || true
      grep -qaF "Username:" "gate-${TAG}.log"
    '
  rc=$?
  set -e
  docker rm -f "$cname" >/dev/null 2>&1 || true
  rm -f "$WORK/gate-${tag}.img"
  return "$rc"
}

case "$MODE" in
  build)
    ensure_artifacts
    log "artifacts ready: $WORK/vmlinux-boot, $WORK/ovmx-distrib-alpha.img"
    ;;
  boot)
    ensure_artifacts
    ts0=$(date +%s)
    if run_one_boot "single"; then
      log "PASS: Alpha booted to a real DCL Username: prompt in $(( $(date +%s) - ts0 ))s"
      exit 0
    fi
    tail -40 "$WORK/gate-single.log" 2>/dev/null || true
    die "BOOT FAILED -- Username: prompt not reached, see $WORK/gate-single.log"
    ;;
  gate)
    # THE gate-invokable check -- ONE command, ONE clean exit code (0 = Alpha
    # boots to a real DCL Username: prompt; nonzero = not release-acceptance
    # clean), bounded by a hard per-boot timeout. This is the single command
    # a release-acceptance / frozen-verify caller invokes:
    #   tools/cross-alpha/run-boot-alpha.sh gate
    ensure_artifacts
    ts0=$(date +%s)
    if run_one_boot "gate"; then
      dur=$(( $(date +%s) - ts0 ))
      log "======================================================================"
      log "  GATE PASSED (vms-359): OVMX/Alpha booted -- executive on /dev/vms ->"
      log "  vmsfs.ko -> DKA0: mounted -> PROVISION.EXE (SYSTEM [1,4] identity) ->"
      log "  STARTUP -> JOB_CONTROL -> LOGINOUT -> a real interactive DCL"
      log "  Username: prompt, under qemu-system-alpha -M clipper, in ${dur}s."
      log "  Release-acceptance clean for the Alpha runtime."
      log "======================================================================"
      exit 0
    fi
    tail -40 "$WORK/gate-gate.log" 2>/dev/null || true
    die "GATE FAILED (vms-359) -- Username: prompt not reached, see $WORK/gate-gate.log"
    ;;
  validate)
    N="${2:-5}"
    ensure_artifacts
    log "validating $N consecutive real Alpha boots (GATE_ROOT=$GATE_ROOT)"
    pass=0
    for i in $(seq 1 "$N"); do
      ts0=$(date +%s)
      if run_one_boot "validate-$i"; then
        log "run $i/$N: PASS ($(( $(date +%s) - ts0 ))s)"
        pass=$((pass + 1))
      else
        log "run $i/$N: FAIL ($(( $(date +%s) - ts0 ))s) -- see $WORK/gate-validate-$i.log"
      fi
    done
    fail=$((N - pass))
    log "validation: $pass/$N passed, $fail/$N failed"
    [ "$fail" -eq 0 ] || die "validation found $fail flake(s) out of $N runs -- NOT gate-grade"
    ;;
  *)
    die "unknown mode '$MODE' (want: build | boot | gate | validate [N])"
    ;;
esac
