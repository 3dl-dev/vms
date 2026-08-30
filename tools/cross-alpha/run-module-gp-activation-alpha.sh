#!/usr/bin/env bash
#
# run-module-gp-activation-alpha.sh -- vms-8208, the OVMX/Alpha single-proc N=3
# activation gate.
#
# HISTORY / WHY THIS EXISTS.  This began as the API-compat leg of the vms-5f5
# per-image module-GP program (#921/vms-095: establish a per-image module-GP in
# $15, `.ovmx_gpdisp $15', linkage loads $15-relative).  Run on the REAL
# executive it caught a REAL regression: the $15 build activates and then
# CRASHES before main (SIGSEGV, $STATUS=SS$_ABORT) because the module-GP
# establish moved the linkage base off &PDSC while the compiler's linkage
# offsets stay &PDSC-relative -- a K-shift that is fatal for every K!=0
# procedure (gdb-pinned: correct cell &PDSC-64 valid, $15-64 NULL, delta == K
# exactly).  The per-image-GP program (C1/C2/C3) was reverted as premise-wrong
# (design doc 1.3 refuted at runtime); this SAME gate now proves the RESTORED
# pre-$15 behavior -- cc1's `.base $27' (base == &PDSC), which the pre-$15
# differential already showed activates cleanly.
#
# WHAT IT PROVES (a REAL activation test, not a does-it-link check):
#
#   1. A single-proc __main / minimal image ACTIVATES on the REAL OVMX/Alpha
#      executive (qemu-system-alpha + real vms.ko / /dev/vms, image staged in
#      SYS$SYSEXE on a MOUNTED ODS-2 volume, read by IMGACT over the Files-11
#      ACP), crt0 -> decc$main -> main runs, and the EXECUTIVE-recorded
#      completion $STATUS is %X0035A019 (N=3 = C$_EXIT1 + (3-1)*8, C$_EXIT1 =
#      0x0035A009).  With `.base $27' the base IS &PDSC, so the &PDSC-relative
#      linkage loads hit their cells -- the model that works.
#
#   2. The value-sensitivity CONTROL (same image, main returns 0) reads back
#      SS$_NORMAL (%X00000001). A fixed constant, or an activation that never
#      runs main, cannot satisfy BOTH the N=3 decode AND this anchor.
#
#   3. No activation-failure appears for either image -- neither an IMGACT-side
#      error (%IMGACT-F / IMGNOTFND / DEVNOTMOUNT / NOSUCHFILE) NOR an
#      ACTIVATED-BUT-CRASHED signature (%DCL-F-ABORT "terminated abnormally
#      (signal N)", $STATUS=%X0000002C = SS$_ABORT -- the exact tell of the
#      reverted $15 K-shift).  (A clean `return 3' instead runs main, prints the
#      crt0-join line + the seam, and only THEN maps the nonzero exit to a
#      %DCL-E-ABORT -- Error severity, NOT the Fatal "terminated abnormally"
#      crash; that E-severity case is EXPECTED and is the known DCL-fidelity
#      gap. The EXECUTIVE seam value is the truth here.)
#
# THE REAL EXECUTIVE, NOT qemu-user (INV-6 / Rule 9).  The definitive assertion
# runs on qemu-system-alpha with a real /dev/vms executive and the image on a
# MOUNTED ODS-2 volume served over the ACP -- exactly the class of the alpha
# boot gates (run-boot-alpha.sh).  A qemu-user run (qemu-alpha, IMGACT as
# PT_INTERP) is NOT a substitute: it has no /dev/vms and no mounted volume, and
# has hidden a real-runtime activation fault before (rc=3 under qemu-user while
# the real executive returned rc=44 %IMGACT-F-IMGNOTFND).  The faithful fix for
# any red here is to SATISFY the runtime (put the image on DKA0:/ODS-2, serve it
# over the ACP, run it at DCL), NEVER to weaken the assertion or fall back to
# qemu-user for the verdict.
#
# PROVE-CAN-FAIL.  `selftest' drives the SAME assert_activation() the real run
# uses against crafted fixtures: a good transcript passes; a wrong sentinel, a
# missing crt0-join line, a broken control anchor, an IMGACT activation failure,
# and an activated-but-crashed image (signal 11 / SS$_ABORT) each RED it. A gate
# that cannot fail certifies nothing.
#
# Rule 9: BUILD/TEST tooling only, fully containerized; nothing here is a
# runtime. All deps in the ovmx-cross-alpha / ovmx-cross-alpha-vms images.
#
# USAGE:
#   tools/cross-alpha/run-module-gp-activation-alpha.sh            # gate (default)
#   tools/cross-alpha/run-module-gp-activation-alpha.sh gate        # same, explicit
#   tools/cross-alpha/run-module-gp-activation-alpha.sh selftest     # can-fail proof, no boot
#
# EXIT: 0 iff the merged-$15 single-proc image activates N=3 on the real
# executive, the SS$_NORMAL control anchors it, and no activation-failure
# %-error appears; nonzero otherwise. The assertions are never weakened to pass.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"                 # tools/cross-alpha
REPO="$(cd "$HERE/../.." && pwd)"
MODE="${1:-gate}"

VMS_IMG="ovmx-cross-alpha-vms"                        # the merged $15 alpha-dec-vms toolchain
BOOT_IMG="ovmx-cross-alpha"                           # kernel/userland/qemu-system-alpha

KV="${KV:-6.6.52}"
# Dedicated, isolated cache root (mirrors run-boot-alpha.sh's convention so a
# concurrent alpha session on this shared host can neither poison nor be
# poisoned by this gate's caches).
GATE_ROOT="${GATE_ROOT:-$REPO/.boot-cache/alpha-modgp-gate}"
export VMSKO_WORK="${VMSKO_WORK:-$GATE_ROOT/vmsko}"
export USERLAND="${USERLAND:-$GATE_ROOT/userland}"
export WORK="${WORK:-$GATE_ROOT/boot}"
export KV

BOOT_TIMEOUT="${BOOT_TIMEOUT:-240}"
DOCKER_TIMEOUT="${DOCKER_TIMEOUT:-$((BOOT_TIMEOUT + 150))}"
TIMEOUT_GRACE="${TIMEOUT_GRACE:-30}"

# The faithful DEC C main-return encoding LINK folds (build.log:
# "C$_EXIT1 folded to absolute 0x35a009"): return 0 -> SS$_NORMAL (0x1),
# return N>=2 -> C$_EXIT1 + (N-1)*8.  So N=3 -> 0x0035A019.
CEXIT1=$((0x35a009))
WANT_SENTINEL=3
# vms-065c: which single-proc MILESTONE image the gate builds + activates. The
# default joint_main.c returns sentinel 3 (N=3, $STATUS=0x0035A019). The
# `crtl-rms-gate' mode overrides this to crtl_rms_test.c, which exercises the
# heap (malloc -> mallocng), RMS file I/O and stdio and returns sentinel 7
# (N=7, $STATUS=0x0035A039) -- the vms-1ef crtl_rms runtime proof, now gated
# per-PR so a future LINK.EXE / toolchain / CRTL change cannot silently
# re-break the malloc->mallocng activation path with no CI tell.
MILESTONE_MAIN=joint_main.c

log() { echo "[modgp-activation] $*"; }
die() { echo "[modgp-activation] FATAL: $*" >&2; exit 1; }

# ---------------------------------------------------------------------------
# assert_activation <console-log> -- THE TEETH. Pure function over a console
# transcript; shared verbatim by the real BOOT-A run and by `selftest`, so the
# self-test exercises the exact logic that gates the real run. Returns 0 iff all
# four conditions hold; prints a per-condition verdict.
# ---------------------------------------------------------------------------
assert_activation() {
  local log="$1"
  [ -f "$log" ] || { echo "  FAIL: no console log at $log"; return 1; }

  local crt0_mile crt0_ctl seam_mile seam_ctl mile_hex ctl_hex
  crt0_mile=$(grep -qaE "OVMX crt0 join: activated, argc=1" "$log" && echo 1 || echo 0)
  crt0_ctl=$(grep -qaE "OVMX crt0 join OK-CONTROL: activated, argc=1" "$log" && echo 1 || echo 0)
  # image=JOINT_E2E\.EXE will NOT match JOINT_E2E_OK.EXE (a `_' follows, not `.').
  seam_mile=$(grep -aoE "OVMX-SEAM: image=JOINT_E2E\.EXE[^\"]*STATUS=0x[0-9A-Fa-f]+" "$log" 2>/dev/null | tail -1)
  seam_ctl=$(grep -aoE "OVMX-SEAM: image=JOINT_E2E_OK\.EXE[^\"]*STATUS=0x[0-9A-Fa-f]+" "$log" 2>/dev/null | tail -1)
  mile_hex=$(printf '%s' "$seam_mile" | grep -oiE '0x[0-9a-f]+' | tail -1)
  ctl_hex=$(printf '%s' "$seam_ctl" | grep -oiE '0x[0-9a-f]+' | tail -1)

  # (b) milestone: the executive $STATUS decodes to the sentinel 3 (== 0x0035A019).
  local sentinel="?" mile_ok=0 mile_dec
  if [ -n "$mile_hex" ]; then
    mile_dec=$(( mile_hex ))
    if [ "$mile_dec" -ge "$CEXIT1" ] && [ $(( (mile_dec - CEXIT1) % 8 )) -eq 0 ]; then
      sentinel=$(( (mile_dec - CEXIT1) / 8 + 1 ))
      [ "$sentinel" -eq "$WANT_SENTINEL" ] && mile_ok=1
    fi
  fi

  # (c) control anchor: main-returns-0 image reads SS$_NORMAL (0x1).
  local ctl_ok=0
  [ -n "$ctl_hex" ] && [ "$(( ctl_hex ))" -eq 1 ] && ctl_ok=1

  # (d) no NEW activation-failure %-error for either joint image. A CLEAN return-3
  # activation prints "OVMX crt0 join: activated" and the seam, and only THEN does
  # the fork path map the nonzero exit to a %DCL-E-ABORT (Error severity, raw value
  # 3 shown) -- that is EXPECTED and is deliberately NOT in this set. These are the
  # signatures a wild module-GP / broken $15 save-restore / never-established $15 /
  # failed IMGACT raises: a hard %DCL-F-ABORT "terminated abnormally (signal N)"
  # CRASH (the image faulted before/at main), or an IMGACT-side activation error.
  # A crash reads back an ACCVIO-class $STATUS (%X0000002C) and prints NO seam.
  local errs err_ok=1
  errs=$(grep -aE "%IMGACT-F|IMGNOTFND|DEVNOTMOUNT|NOSUCHFILE|ACCVIO|terminated abnormally|signal 1[012]|signal [46]|%X0000002C" "$log" 2>/dev/null || true)
  [ -n "$errs" ] && err_ok=0

  echo "  (a) crt0 -> main ran   : milestone=$crt0_mile control=$crt0_ctl (want 1/1)"
  echo "  (b) N=3 milestone seam : ${seam_mile:-<ABSENT>}"
  echo "      decode: (${mile_hex:-<none>} - C\$_EXIT1 0x35a009)/8 + 1 = $sentinel  (want $WANT_SENTINEL; ok=$mile_ok)"
  echo "  (c) SS\$_NORMAL anchor  : ${seam_ctl:-<ABSENT>}  (want 0x1; ok=$ctl_ok)"
  echo "  (d) no activation err  : ok=$err_ok"
  [ "$err_ok" -eq 0 ] && echo "      offending: $(printf '%s' "$errs" | tr '\n' '|')"

  if [ "$crt0_mile" -eq 1 ] && [ "$crt0_ctl" -eq 1 ] \
     && [ "$mile_ok" -eq 1 ] && [ "$ctl_ok" -eq 1 ] && [ "$err_ok" -eq 1 ]; then
    return 0
  fi
  return 1
}

# assert_crtl_rms <console-log> -- THE TEETH for the vms-1ef crtl_rms->N=7 proof
# (`crtl-rms-gate' mode). The crtl_rms port image (crtl_rms_test.c) does NOT
# print joint_main.c's "OVMX crt0 join" milestone line, so assert_activation
# does not apply; the crtl_rms proof is instead (a) the heap+RMS+stdio port-test
# OK line -- the malloc(8192)->mallocng path that SIGSEGV'd before vms-1ef #958,
# now completing -- (b) the executive-recorded $STATUS decoding to sentinel 7
# (0x0035A039), and (c) no activation-failure %-error. Pure function over the
# console transcript; shared verbatim by the real BOOT-A run and the can-fail
# selftest so the self-test exercises the exact gating logic.
assert_crtl_rms() {
  local log="$1"
  [ -f "$log" ] || { echo "  FAIL: no console log at $log"; return 1; }

  local port_ok seam mile_hex mile_dec sentinel="?" mile_ok=0
  port_ok=$(grep -qaE "OVMX CRTL/RMS port test: OK \(heap\+RMS\+stdio\)" "$log" && echo 1 || echo 0)
  seam=$(grep -aoE "OVMX-SEAM: image=JOINT_E2E\.EXE[^\"]*STATUS=0x[0-9A-Fa-f]+" "$log" 2>/dev/null | tail -1)
  mile_hex=$(printf '%s' "$seam" | grep -oiE '0x[0-9a-f]+' | tail -1)
  if [ -n "$mile_hex" ]; then
    mile_dec=$(( mile_hex ))
    if [ "$mile_dec" -ge "$CEXIT1" ] && [ $(( (mile_dec - CEXIT1) % 8 )) -eq 0 ]; then
      sentinel=$(( (mile_dec - CEXIT1) / 8 + 1 ))
      [ "$sentinel" -eq 7 ] && mile_ok=1
    fi
  fi

  # A crash before/at main reads back an ACCVIO-class $STATUS (%X0000002C) and
  # prints NO port-test line -- the exact pre-vms-1ef malloc->mallocng SIGSEGV.
  local errs err_ok=1
  errs=$(grep -aE "%IMGACT-F|IMGNOTFND|DEVNOTMOUNT|NOSUCHFILE|ACCVIO|terminated abnormally|signal 1[012]|signal [46]|%X0000002C" "$log" 2>/dev/null || true)
  [ -n "$errs" ] && err_ok=0

  echo "  (a) crtl_rms heap+RMS+stdio OK : port_ok=$port_ok (want 1)"
  echo "  (b) N=7 milestone seam         : ${seam:-<ABSENT>}"
  echo "      decode: (${mile_hex:-<none>} - C\$_EXIT1 0x35a009)/8 + 1 = $sentinel  (want 7; ok=$mile_ok)"
  echo "  (c) no activation err          : ok=$err_ok"
  [ "$err_ok" -eq 0 ] && echo "      offending: $(printf '%s' "$errs" | tr '\n' '|')"

  [ "$port_ok" -eq 1 ] && [ "$mile_ok" -eq 1 ] && [ "$err_ok" -eq 1 ] && return 0
  return 1
}

# ---------------------------------------------------------------------------
# build_joint_images -- build the N=3 milestone image (joint_main.c -> return 3)
# and the SS$_NORMAL control (joint_main_ok.c -> return 0) with the SAME merged
# $15 toolchain, then lay them + their producers into $WORK/joint where
# build-alpha-bootimage.sh stages them onto the ODS-2 volume.
# ---------------------------------------------------------------------------
build_joint_images() {
  local bji="$REPO/tools/cross-alpha-vms/joint-e2e/build-joint-image.sh"
  local out_n3 out_ok
  out_n3="$GATE_ROOT/joint-n3"
  out_ok="$GATE_ROOT/joint-ok"
  rm -rf "$out_n3" "$out_ok"; mkdir -p "$out_n3" "$out_ok"

  log "step 1a: build the milestone single-proc image ($MILESTONE_MAIN, sentinel $WANT_SENTINEL) with the merged toolchain"
  JOINT_MAIN="$MILESTONE_MAIN" IMG="$VMS_IMG" bash "$bji" "$out_n3" \
    || die "build-joint-image.sh (milestone $MILESTONE_MAIN) failed -- see $out_n3/build.log"
  grep -q 'LINK-S-CREATED' "$out_n3/build.log" \
    || die "milestone image did not link (no %LINK-S-CREATED) -- see $out_n3/build.log"

  log "step 1b: build the SS\$_NORMAL control image (joint_main_ok.c, return 0) with the merged toolchain"
  JOINT_MAIN=joint_main_ok.c IMG="$VMS_IMG" bash "$bji" "$out_ok" \
    || die "build-joint-image.sh (control) failed -- see $out_ok/build.log"
  grep -q 'LINK-S-CREATED' "$out_ok/build.log" \
    || die "control image did not link (no %LINK-S-CREATED) -- see $out_ok/build.log"

  # Assemble $WORK/joint exactly as build-alpha-bootimage.sh expects (JOINT=/work/joint):
  #   joint_e2e.exe (milestone), joint_e2e_ok.exe (control), DECC$SHR.EXE, LIBOTS_SHR.EXE.
  mkdir -p "$WORK/joint"
  cp "$out_n3/joint_e2e.exe"    "$WORK/joint/joint_e2e.exe"
  cp "$out_ok/joint_e2e.exe"    "$WORK/joint/joint_e2e_ok.exe"
  cp "$out_n3/DECC\$SHR.EXE"    "$WORK/joint/DECC\$SHR.EXE"
  cp "$out_n3/LIBOTS_SHR.EXE"   "$WORK/joint/LIBOTS_SHR.EXE"
  for f in "joint_e2e.exe" "joint_e2e_ok.exe" "DECC\$SHR.EXE" "LIBOTS_SHR.EXE"; do
    [ -s "$WORK/joint/$f" ] || die "joint artifact $WORK/joint/$f missing/empty after build"
  done
  log "step 1: joint images staged into $WORK/joint (milestone N=3 + SS\$_NORMAL control + producers)"
}

# ---------------------------------------------------------------------------
# assemble_boot_image -- build-alpha-bootimage.sh masters the ODS-2 system disk
# with the joint images staged into SYS$SYSEXE + the proof SYSTARTUP. The
# kernel/vms.ko/userland cross-builds are cached (they are the runtime, not the
# subject); the disk is re-mastered every call, re-reading our fresh $WORK/joint.
# ---------------------------------------------------------------------------
assemble_boot_image() {
  log "step 2: assemble the OVMX/Alpha boot image (ODS-2 master stages JOINT_E2E.EXE + control)"
  "$HERE/build-alpha-bootimage.sh" >/dev/null 2>&1 || "$HERE/build-alpha-bootimage.sh"
  [ -f "$WORK/vmlinux-boot" ] && [ -f "$WORK/ovmx-distrib-alpha.img" ] \
    || die "build-alpha-bootimage.sh finished but boot artifacts missing from $WORK"
}

# ---------------------------------------------------------------------------
# run_boot_a -- ONE real BOOT A on qemu-system-alpha + real /dev/vms. The proof
# SYSTARTUP RUNs JOINT_E2E_OK then JOINT_E2E during STDRV (before login); with
# OVMX_IMGACT_SEAM=1 IMGACT prints the executive-recorded $STATUS per image. We
# drive a console CR to Username: (LOGINOUT's OPA0: wake wait) to hold the boot
# open long enough for STDRV to finish, then capture the filtered console log.
# ---------------------------------------------------------------------------
run_boot_a() {
  rm -f "$WORK/modgpA.img" "$WORK/modgpA.raw" "$WORK/modgpA.log" "$WORK/modgpA.fifo"
  local cname="ovmx-alpha-modgp-$$"
  set +e
  timeout --kill-after="$TIMEOUT_GRACE" "$DOCKER_TIMEOUT" docker run --rm \
    --name "$cname" --memory=8g --cpus="$(nproc)" \
    -v "$WORK":/work "$BOOT_IMG" bash -euo pipefail -c '
      BT="'"$BOOT_TIMEOUT"'"
      cd /work
      cp ovmx-distrib-alpha.img modgpA.img
      FIFO=/work/modgpA.fifo; rm -f "$FIFO"; mkfifo "$FIFO"
      # OVMX_IMGACT_SEAM=1: surface the EXECUTIVE-recorded completion $STATUS per
      # activated image (GETEXIT(SEL_SELF)); the DCL RUN fork path collapses the
      # POSIX exit, so the seam is the truth for the returned value.
      timeout "$BT" qemu-system-alpha -M clipper -smp 1 -m 1024 -vga none -nic none \
          -kernel vmlinux-boot -append "console=ttyS0 panic=-1 OVMX_IMGACT_SEAM=1" \
          -drive file=modgpA.img,format=raw,if=virtio \
          -nographic -no-reboot <"$FIFO" > modgpA.raw 2>&1 &
      QP=$!
      exec 6>"$FIFO"
      trap "" PIPE
      W=0
      while kill -0 "$QP" 2>/dev/null; do
          grep -qaF "Username:" modgpA.raw 2>/dev/null && break
          printf "\r" >&6 2>/dev/null || true
          sleep 2; W=$((W + 2))
          [ "$W" -ge "$BT" ] && break
      done
      exec 6>&-
      sleep 3            # let STDRV/LOGINOUT flush the last seam + prompt
      kill "$QP" 2>/dev/null || true
      wait "$QP" 2>/dev/null || true
      rm -f "$FIFO"
      grep -avE "TSUNAMI machine check|tsunami_(read|write)" modgpA.raw > modgpA.log || true
    '
  set -e
  docker rm -f "$cname" >/dev/null 2>&1 || true
  rm -f "$WORK/modgpA.img"
  [ -f "$WORK/modgpA.log" ] || die "BOOT A produced no console log (qemu-system-alpha never started?)"
}

# ---------------------------------------------------------------------------
# selftest -- prove assert_activation() has teeth. A good fixture passes; four
# distinct breakages each RED it. Uses the SAME function the real gate uses.
# ---------------------------------------------------------------------------
GOOD_FIXTURE() {
  cat <<'EOF'
JOINT-E2E-PROOF: === CONTROL: RUN JOINT_E2E_OK (main returns 0) ===
OVMX crt0 join OK-CONTROL: activated, argc=1
OVMX-SEAM: image=JOINT_E2E_OK.EXE flavor=VMS_STD $STATUS=0x00000001
JOINT-E2E-PROOF: CONTROL-STATUS=%X00000001 SEVERITY=1
JOINT-E2E-PROOF: === MILESTONE: RUN JOINT_E2E (main returns sentinel 3) ===
OVMX crt0 join: activated, argc=1
OVMX-SEAM: image=JOINT_E2E.EXE flavor=VMS_STD $STATUS=0x0035A019
%DCL-E-ABORT, abort
JOINT-E2E-PROOF: STATUS=%X0035A019 SEVERITY=1
EOF
}

selftest() {
  local d fails=0
  d=$(mktemp -d); trap 'rm -rf "$d"' RETURN

  # 1. GOOD -> must PASS (exit 0).
  GOOD_FIXTURE > "$d/good.log"
  echo "-- selftest 1/6: GOOD transcript must PASS --"
  if assert_activation "$d/good.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: good transcript rejected"; fails=$((fails+1)); fi

  # 2. WRONG SENTINEL (N=4: 0x0035A021) -> must FAIL.
  GOOD_FIXTURE | sed 's/0x0035A019/0x0035A021/' > "$d/wrong.log"
  echo "-- selftest 2/6: wrong sentinel (N=4) must FAIL --"
  if assert_activation "$d/wrong.log" >/dev/null 2>&1; then echo "  FAIL: wrong sentinel accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  # 3. MISSING crt0-join (main never ran) -> must FAIL.
  GOOD_FIXTURE | grep -v "OVMX crt0 join: activated" > "$d/nocrt0.log"
  echo "-- selftest 3/6: missing milestone crt0-join must FAIL --"
  if assert_activation "$d/nocrt0.log" >/dev/null 2>&1; then echo "  FAIL: missing crt0-join accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  # 4. BROKEN CONTROL ANCHOR (control also reads 0x0035A019, i.e. a fixed
  #    constant, not value-sensitive) -> must FAIL.
  GOOD_FIXTURE | sed 's/image=JOINT_E2E_OK.EXE flavor=VMS_STD $STATUS=0x00000001/image=JOINT_E2E_OK.EXE flavor=VMS_STD $STATUS=0x0035A019/' > "$d/anchor.log"
  echo "-- selftest 4/6: broken SS\$_NORMAL control anchor must FAIL --"
  if assert_activation "$d/anchor.log" >/dev/null 2>&1; then echo "  FAIL: broken anchor accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  # 5. IMGACT ACTIVATION FAILURE (image not found on the ODS-2 volume) -> must FAIL.
  GOOD_FIXTURE | sed 's/OVMX-SEAM: image=JOINT_E2E.EXE flavor=VMS_STD $STATUS=0x0035A019/%IMGACT-F-IMGNOTFND, image SYS$SYSTEM:JOINT_E2E.EXE not found/' > "$d/imgact.log"
  echo "-- selftest 5/6: IMGACT activation failure (IMGNOTFND) must FAIL --"
  if assert_activation "$d/imgact.log" >/dev/null 2>&1; then echo "  FAIL: activation failure accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  # 6. ACTIVATED-BUT-CRASHED: the image is found + RUN over the ACP but SIGSEGVs
  #    (%DCL-F-ABORT ... terminated abnormally (signal 11), $STATUS=%X0000002C =
  #    SS$_ABORT 44), no crt0-join, no seam -- the exact signature of the merged
  #    $15 mechanism regressing activation on the real executive. Must FAIL.
  cat > "$d/crash.log" <<'EOF'
JOINT-E2E-PROOF: === CONTROL: RUN JOINT_E2E_OK (main returns 0) ===
%DCL-F-ABORT, image SYS$SYSTEM:JOINT_E2E_OK terminated abnormally (signal 11)
JOINT-E2E-PROOF: CONTROL-STATUS=%X0000002C SEVERITY=4
JOINT-E2E-PROOF: === MILESTONE: RUN JOINT_E2E (main returns sentinel 3) ===
%DCL-F-ABORT, image SYS$SYSTEM:JOINT_E2E terminated abnormally (signal 11)
JOINT-E2E-PROOF: STATUS=%X0000002C SEVERITY=4
EOF
  echo "-- selftest 6/6: activated-but-crashed (signal 11 / SS\$_ABORT) must FAIL --"
  if assert_activation "$d/crash.log" >/dev/null 2>&1; then echo "  FAIL: crash-on-activation accepted"; fails=$((fails+1)); else echo "  PASS (rejected)"; fi

  echo ""
  if [ "$fails" -eq 0 ]; then
    echo "=== selftest: assert_activation() has teeth (good passes, all 5 breakages red) ==="
    return 0
  fi
  echo "=== selftest FAILED: $fails case(s) wrong -- the gate cannot be trusted ==="
  return 1
}

case "$MODE" in
  selftest)
    selftest
    ;;
  gate)
    # Always prove the assertion has teeth before trusting a green boot.
    log "verifying the gate can fail (selftest) before the real boot"
    selftest || die "selftest failed -- assert_activation() cannot be trusted; aborting before the boot"
    echo ""
    build_joint_images
    assemble_boot_image
    log "step 3: BOOT A -- activate the single-proc N=3 image on the REAL executive"
    run_boot_a
    echo ""
    echo "========================================================================"
    echo "== vms-8208 module-GP API-compat: single-proc N=3 activation on the real"
    echo "== OVMX/Alpha executive (qemu-system-alpha + /dev/vms, ODS-2 ACP)"
    echo "========================================================================"
    grep -aE "JOINT-E2E-PROOF:|OVMX crt0 join|OVMX-SEAM:|%IMGACT|%DCL-" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    echo "------------------------------------------------------------------------"
    if assert_activation "$WORK/modgpA.log"; then
      echo ""
      echo "PASS: the merged \$15 module-GP toolchain's single-proc __main image ACTIVATED on"
      echo "      the real executive over the mounted ODS-2 ACP; crt0 -> decc\$main -> main ran"
      echo "      and \$STATUS = %X0035A019 (N=3). The SS\$_NORMAL control anchors value-"
      echo "      sensitivity; no activation-failure %-error appeared. K=0 no-op establish +"
      echo "      the always-present \$15 save/restore did NOT regress activation."
      exit 0
    fi
    echo ""
    echo "FAIL: the single-proc N=3 image did NOT cleanly activate on the real executive --"
    echo "      a REAL regression from the \$15 save/restore or the no-op establish. Full log:"
    echo "      $WORK/modgpA.log"
    echo "--- activation-failure signatures ---"
    grep -aE "%IMGACT|%RUN-|%DCL-|IMGNOTFND|NOSUCHFILE|DEVNOTMOUNT|ACCVIO|SS\\\$_" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  /' | tail -20 || echo "  (none captured)"
    exit 1
    ;;
  crtl-rms-gate)
    # vms-065c: the vms-1ef crtl_rms->N=7 runtime proof as a per-PR gate. Same
    # real-executive boot as `gate', but the MILESTONE image is crtl_rms_test.c
    # (heap malloc->mallocng + RMS file I/O + stdio, returns sentinel 7). A
    # LINK.EXE / toolchain / CRTL change that re-breaks the malloc->mallocng
    # activation path (vms-1ef #958) reds HERE instead of only in a manual boot.
    MILESTONE_MAIN=crtl_rms_test.c
    WANT_SENTINEL=7

    # Prove assert_crtl_rms has teeth before trusting a green boot (mirrors the
    # `gate' selftest discipline): a clean crtl_rms proof passes; the pre-vms-1ef
    # SIGSEGV signature (no port-test line + ACCVIO $STATUS) reds.
    _st=$(mktemp -d); _fails=0
    cat > "$_st/pass.log" <<'EOF'
OVMX CRTL/RMS port test: wrote+read 8192 bytes via 'PORTTEST.DAT', pattern verified
OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
EOF
    cat > "$_st/crash.log" <<'EOF'
%DCL-F-ABORT, image SYS$SYSTEM:JOINT_E2E terminated abnormally (signal 11)
JOINT-E2E-PROOF: STATUS=%X0000002C SEVERITY=4
EOF
    echo "-- crtl-rms selftest 1/2: clean heap+RMS+stdio N=7 proof must PASS --"
    if assert_crtl_rms "$_st/pass.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: clean proof rejected"; _fails=$((_fails+1)); fi
    echo "-- crtl-rms selftest 2/2: pre-vms-1ef malloc->mallocng SIGSEGV must FAIL --"
    if assert_crtl_rms "$_st/crash.log" >/dev/null 2>&1; then echo "  FAIL: crash accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    rm -rf "$_st"
    [ "$_fails" -eq 0 ] || die "crtl-rms selftest failed -- assert_crtl_rms cannot be trusted; aborting before the boot"
    echo ""

    build_joint_images
    assemble_boot_image
    log "step 3: BOOT A -- activate the crtl_rms N=7 image (heap+RMS+stdio) on the REAL executive"
    run_boot_a
    echo ""
    echo "========================================================================"
    echo "== vms-1ef crtl_rms -> N=7: heap(malloc->mallocng)+RMS+stdio activation on"
    echo "== the real OVMX/Alpha executive (qemu-system-alpha + /dev/vms, ODS-2 ACP)"
    echo "========================================================================"
    grep -aE "JOINT-E2E-PROOF:|OVMX-SEAM:|OVMX CRTL/RMS|%IMGACT|%DCL-" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    echo "------------------------------------------------------------------------"
    if assert_crtl_rms "$WORK/modgpA.log"; then
      echo ""
      echo "PASS: the crtl_rms port image ACTIVATED on the real executive over the mounted"
      echo "      ODS-2 ACP; heap(malloc->mallocng)+RMS+stdio round-tripped and \$STATUS ="
      echo "      %X0035A039 (N=7). The vms-1ef thunk->strong-descriptor fix holds at runtime."
      exit 0
    fi
    echo ""
    echo "FAIL: the crtl_rms N=7 image did NOT cleanly activate (heap+RMS+stdio + sentinel 7)"
    echo "      -- a REAL regression of the vms-1ef malloc->mallocng activation path. Full log:"
    echo "      $WORK/modgpA.log"
    grep -aE "%IMGACT|%RUN-|%DCL-|IMGNOTFND|NOSUCHFILE|DEVNOTMOUNT|ACCVIO|SS\\\$_" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  /' | tail -20 || echo "  (none captured)"
    exit 1
    ;;
  *)
    die "unknown mode '$MODE' (use: gate | crtl-rms-gate | selftest)"
    ;;
esac
