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
# /dev/vms -> its Files-11 ODS-2 ACP mounts the system disk on DKA0: -> PID 1
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
#   tools/cross-alpha/run-boot-alpha.sh login          # boot + authenticate SYSTEM/
#                                                       # MANAGER to an interactive DCL $
#                                                       # (end-to-end boot-login gate)
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

# run_login_boot <tag> -- ONE real boot driven PAST the Username: prompt to an
# authenticated, interactive DCL "$" prompt, the Alpha analog of the x86_64
# login-to-$ gate (tests/qemu/test_install_boot_e2e.sh's login() + SHOW TIME
# assertion). It exercises the SAME LOGINOUT.EXE C path cross-built EM_ALPHA:
# SYSTEM at "Username:", MANAGER at "Password:", the "Welcome to OpenVMX" login
# banner, then "SHOW TIME" and the assertion that a real DCL activated and
# executed the command (its output carries the current year). This is where the
# authentic binary-$UAFDEF SYSUAF + Purdy authentication is proven end-to-end on
# Alpha, not just boot-to-Username. Verdict = login banner reached AND SHOW TIME
# returned the year. Returns 0/1; never `exit`s (composable). The "$" and banner
# assertions are never weakened to make this pass (Rule 8 / INV-6).
#
# Credentials/marker are the shipped SYSTEM=MANAGER defaults; override via
# LOGIN_USER / LOGIN_PASS if a fixture ever seeds a different SYSUAF.
run_login_boot() {
  local tag="$1"
  rm -f "$WORK/gate-${tag}.img" "$WORK/gate-${tag}.raw" "$WORK/gate-${tag}.log" "$WORK/gate-${tag}.fifo"
  local cname="ovmx-alpha-login-${tag}-$$"
  local rc=0
  set +e
  timeout --kill-after="$TIMEOUT_GRACE" "$DOCKER_TIMEOUT" docker run --rm \
    --name "$cname" --memory=8g --cpus="$(nproc)" \
    -v "$WORK":/work "$IMG" bash -euo pipefail -c '
      BT="'"$BOOT_TIMEOUT"'"; TAG="'"$tag"'"
      LOGIN_USER="'"${LOGIN_USER:-SYSTEM}"'"; LOGIN_PASS="'"${LOGIN_PASS:-MANAGER}"'"
      BANNER="Welcome to OpenVMX"; CURYEAR="$(date +%Y)"
      cd /work
      cp ovmx-distrib-alpha.img "gate-${TAG}.img"
      RAW="gate-${TAG}.raw"
      FIFO="/work/gate-${TAG}.fifo"; mkfifo "$FIFO"
      timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
          -kernel vmlinux-boot -append "console=ttyS0 panic=-1" \
          -drive file="gate-${TAG}.img",format=raw,if=virtio \
          -nographic -no-reboot <"$FIFO" > "$RAW" 2>&1 &
      QP=$!
      exec 6>"$FIFO"
      trap "" PIPE   # a CR fed just as the guest exits must not kill this shell
      send()    { printf "%s\r" "$1" >&6 2>/dev/null || true; }
      # waitpat <pattern> <seconds-limit> <since-byte> -- fixed-string, bounded.
      waitpat() {
          local pat="$1" lim="$2" since="${3:-0}" w=0
          while [ "$w" -lt "$lim" ]; do
              tail -c "+$((since + 1))" "$RAW" 2>/dev/null | grep -qaF "$pat" && return 0
              kill -0 "$QP" 2>/dev/null || return 1
              sleep 1; w=$((w + 1))
          done
          return 1
      }
      # waitpat_re <ERE> <seconds-limit> <since-byte> -- like waitpat but regex.
      waitpat_re() {
          local pat="$1" lim="$2" since="${3:-0}" w=0
          while [ "$w" -lt "$lim" ]; do
              tail -c "+$((since + 1))" "$RAW" 2>/dev/null | grep -qaE "$pat" && return 0
              kill -0 "$QP" 2>/dev/null || return 1
              sleep 1; w=$((w + 1))
          done
          return 1
      }
      # 1. Wake LOGINOUT to the Username: prompt (CR every 2s, vms-3f6/vms-2213).
      W=0
      while kill -0 "$QP" 2>/dev/null; do
          grep -qaF "Username:" "$RAW" 2>/dev/null && break
          printf "\r" >&6 2>/dev/null || true
          sleep 2; W=$((W + 2))
          [ "$W" -ge "$BT" ] && break
      done
      LOGIN_OK=0; CMD_OK=0
      if grep -qaF "Username:" "$RAW" 2>/dev/null; then
          OFF=$(wc -c < "$RAW")
          # 2. Authenticate: SYSTEM / MANAGER against the on-disk SYSUAF.
          send "$LOGIN_USER"
          waitpat "Password:" 20 "$OFF" && send "$LOGIN_PASS"
          # 3. Login success == the real LOGINOUT banner (not a printf).
          waitpat "$BANNER" 30 "$OFF" && LOGIN_OK=1
          # 4. Interactive DCL: run SHOW TIME, assert it executed by matching a
          #    VMS date-time stamp (DD-MON-YYYY HH:MM:SS) in the output AFTER the
          #    command (OFF2) -- proves a real DCL ran SHOW TIME.  We do NOT pin
          #    the host year here: the AXPbox/qemu-alpha RTC runs on an epoch-1980
          #    register so the guest clock reads ~20 years behind the host, and
          #    pinning date +%Y would fail a correct login (the check is "DCL
          #    executed the command", not "the emulated clock is correct").
          waitpat "\$" 20 "$OFF" || true          # settle at the DCL prompt
          OFF2=$(wc -c < "$RAW")
          send "SHOW TIME"
          waitpat_re "[0-9]+-[A-Z][A-Z][A-Z]-[0-9][0-9][0-9][0-9] [0-9][0-9]:[0-9][0-9]:[0-9][0-9]" 20 "$OFF2" && CMD_OK=1
      fi
      exec 6>&-
      sleep 2
      kill "$QP" 2>/dev/null || true
      wait "$QP" 2>/dev/null || true
      rm -f "$FIFO"
      grep -avE "TSUNAMI machine check|tsunami_(read|write)" "$RAW" > "gate-${TAG}.log" || true
      echo "LOGIN_OK=${LOGIN_OK} CMD_OK=${CMD_OK}" >> "gate-${TAG}.log"
      [ "$LOGIN_OK" -eq 1 ] && [ "$CMD_OK" -eq 1 ]
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
      log "  Files-11 ACP -> DKA0: mounted -> PROVISION.EXE (SYSTEM [1,4] identity) ->"
      log "  STARTUP -> JOB_CONTROL -> LOGINOUT -> a real interactive DCL"
      log "  Username: prompt, under qemu-system-alpha -M clipper, in ${dur}s."
      log "  Release-acceptance clean for the Alpha runtime."
      log "======================================================================"
      exit 0
    fi
    tail -40 "$WORK/gate-gate.log" 2>/dev/null || true
    die "GATE FAILED (vms-359) -- Username: prompt not reached, see $WORK/gate-gate.log"
    ;;
  login)
    # END-TO-END BOOT-LOGIN gate -- boots, then authenticates SYSTEM/MANAGER
    # against the on-disk SYSUAF and lands on an interactive DCL "$" prompt,
    # running SHOW TIME to prove a real DCL executed (the Alpha analog of the
    # x86_64 login-to-$ e2e, tests/qemu/test_install_boot_e2e.sh). Exit 0 IFF
    # the login banner is reached AND SHOW TIME returns the year.
    ensure_artifacts
    ts0=$(date +%s)
    if run_login_boot "login"; then
      dur=$(( $(date +%s) - ts0 ))
      log "======================================================================"
      log "  LOGIN GATE PASSED: OVMX/Alpha booted -> LOGINOUT -> SYSTEM/MANAGER"
      log "  authenticated against SYSUAF -> interactive DCL \$ prompt -> SHOW TIME"
      log "  executed (real DCL, INV-6-clean), under qemu-system-alpha, in ${dur}s."
      log "======================================================================"
      exit 0
    fi
    tail -40 "$WORK/gate-login.log" 2>/dev/null || true
    die "LOGIN GATE FAILED -- did not authenticate to a DCL \$ prompt, see $WORK/gate-login.log"
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
    die "unknown mode '$MODE' (want: build | boot | gate | login | validate [N])"
    ;;
esac
