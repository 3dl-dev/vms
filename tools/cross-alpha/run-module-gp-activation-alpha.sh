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
#   tools/cross-alpha/run-module-gp-activation-alpha.sh crtl-rms-gate # crtl_rms heap+RMS+stdio -> N=7 (non-veneer control)
#   tools/cross-alpha/run-module-gp-activation-alpha.sh crtl-rms-veneer-gate # vms-f49 rung 4: veneer write + INDEPENDENT ODS-2 File-ID reader
#   tools/cross-alpha/run-module-gp-activation-alpha.sh crtl-rms-fileop-gate # vms-3320: open/creat/unlink/rename/opendir/readdir/closedir veneer + INDEPENDENT DIRECTORY reader
#   tools/cross-alpha/run-module-gp-activation-alpha.sh mf-gate       # multi-.o cross-boundary -> N=5 (vms-bdd)
#   tools/cross-alpha/run-module-gp-activation-alpha.sh shipped-gate  # SHIPPED packaging path -> N=3 (vms-410)
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
# MILESTONE_EXTRA (vms-bdd): additional .c objects STRICT-linked with the milestone
# main. Empty for the N=3 / N=7 single-object gates; the `mf-gate' mode sets it to
# mf_util.c so the multi-.o cross-boundary program (mf_main.c) is built + activated.
MILESTONE_EXTRA=""
# JOINT_CRTL_RMS_VENEER (vms-f49, rung 4 of vms-b4f): default 0, so the gate/
# crtl-rms-gate/mf-gate modes build byte-identically to before. The
# `crtl-rms-veneer-gate' mode sets it to 1 so build-joint-image.sh composes the
# two-pass CRTL->RMS stdio veneer + emits LIBVMSRMS$SHR.EXE, and the port image's
# decc$fopen binds to sys$create over the ACP instead of musl-POSIX.
JOINT_CRTL_RMS_VENEER=0

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

# assert_mf <console-log> -- THE TEETH for the vms-bdd multi-.o proof (`mf-gate'
# mode). The mf_main image (mf_main.obj + mf_util.obj, STRICT-linked) calls
# ACROSS the .o boundary into mf_util (mf_dup/mf_len/mf_eq/mf_free), which pulls
# malloc/free/memcpy/strlen/strcmp from the genuine DECC$SHR -- the decc$free-
# class weak-alias export (PR #795) that a SEPARATE object referencing free()
# needs. The proof is (a) the multi-file port-test OK line -- the cross-.o
# round-trip completed on the real executive -- (b) the executive-recorded
# $STATUS decoding to sentinel 5 (0x0035A029), and (c) no activation-failure
# %-error. Pure function over the console transcript; shared verbatim by the real
# BOOT-A run and the can-fail selftest.
assert_mf() {
  local log="$1"
  [ -f "$log" ] || { echo "  FAIL: no console log at $log"; return 1; }

  local port_ok seam mile_hex mile_dec sentinel="?" mile_ok=0
  port_ok=$(grep -qaE "OVMX multi-file port test: OK \(cross-\.o " "$log" && echo 1 || echo 0)
  seam=$(grep -aoE "OVMX-SEAM: image=JOINT_E2E\.EXE[^\"]*STATUS=0x[0-9A-Fa-f]+" "$log" 2>/dev/null | tail -1)
  mile_hex=$(printf '%s' "$seam" | grep -oiE '0x[0-9a-f]+' | tail -1)
  if [ -n "$mile_hex" ]; then
    mile_dec=$(( mile_hex ))
    if [ "$mile_dec" -ge "$CEXIT1" ] && [ $(( (mile_dec - CEXIT1) % 8 )) -eq 0 ]; then
      sentinel=$(( (mile_dec - CEXIT1) / 8 + 1 ))
      [ "$sentinel" -eq 5 ] && mile_ok=1
    fi
  fi

  # A crash before/at main (or a broken cross-.o resolution / unresolved decc$free
  # regression) reads back an ACCVIO-class $STATUS and prints NO port-test line.
  local errs err_ok=1
  errs=$(grep -aE "%IMGACT-F|IMGNOTFND|DEVNOTMOUNT|NOSUCHFILE|ACCVIO|terminated abnormally|signal 1[012]|signal [46]|%X0000002C" "$log" 2>/dev/null || true)
  [ -n "$errs" ] && err_ok=0

  echo "  (a) multi-.o cross-boundary OK : port_ok=$port_ok (want 1)"
  echo "  (b) N=5 milestone seam         : ${seam:-<ABSENT>}"
  echo "      decode: (${mile_hex:-<none>} - C\$_EXIT1 0x35a009)/8 + 1 = $sentinel  (want 5; ok=$mile_ok)"
  echo "  (c) no activation err          : ok=$err_ok"
  [ "$err_ok" -eq 0 ] && echo "      offending: $(printf '%s' "$errs" | tr '\n' '|')"

  [ "$port_ok" -eq 1 ] && [ "$mile_ok" -eq 1 ] && [ "$err_ok" -eq 1 ] && return 0
  return 1
}

# assert_veneer <console-log> -- THE TEETH for the vms-f49 rung-4 un-fakeable
# CRTL->RMS veneer proof (`crtl-rms-veneer-gate' mode). This is the anti-
# fabrication payoff (INV-6): it does NOT trust the port image's own console
# text or its same-process CRTL/RMS read-back (which a ramfs satisfies
# IDENTICALLY -- that is exactly what the plain crtl-rms N=7 gate can be fooled
# by). It gates on an INDEPENDENT reader -- DCL DIRECTORY/FULL, a DIFFERENT
# accessor that runs its OWN sys$parse+sys$search over the Files-11 ACP directory
# (dcl_cmd_file.c cmd_directory, from_acp) and prints the GENUINE ODS-2 File ID
# (num,seq,rvn) the on-disk directory returned. A POSIX/ramfs write cannot appear
# in the ACP directory at all (it draws %DIRECT-W-NOFILES, NO File ID line), so a
# PORTTEST.DAT;1 File-ID line here is proof the veneer's fopen genuinely landed
# the file on the real ODS-2 volume. Pass iff:
#   (a) the veneer write completed  -- crtl_rms port-test OK + executive N=7 seam;
#   (b) THE TEETH: the INDEPENDENT DIRECTORY/FULL reader region shows
#       PORTTEST.DAT;1 with a NONZERO ODS-2 File ID and NO %DIRECT-W-NOFILES;
#   (c) no activation-failure %-error.
# Pure function over the console transcript; shared verbatim by the real BOOT-A
# run and the can-fail selftest.
assert_veneer() {
  local log="$1"
  [ -f "$log" ] || { echo "  FAIL: no console log at $log"; return 1; }

  # (a) the veneer write ran: crtl_rms heap+RMS+stdio OK line + N=7 seam. With
  # the veneer, fopen/fwrite/fclose route sys$create/$put over the ACP; a broken
  # LLP64 width (vms-1fc) would truncate the ioctl pointer, fwrite would short,
  # and the image would return <7 with no OK line -- so this already needs the
  # write path to work end to end.
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

  # (b) THE TEETH -- confine the check to the INDEPENDENT-reader region so no
  # stray earlier token can satisfy it. DIRECTORY/FULL prints the name+version
  # and the genuine File ID on one line ("PORTTEST.DAT;1  File ID:  (14,1,0)"),
  # ONLY when the entry came from the ACP search (from_acp); a ramfs/POSIX write
  # never reaches the ACP directory and draws %DIRECT-W-NOFILES instead.
  local region fid_line fid_num=0 reader_ok=0 nofiles=0 size_line size_blocks=0
  region=$(awk '/VENEER-PROOF: === INDEPENDENT READER/{f=1} f{print} /VENEER-PROOF: DIR-STATUS/{f=0}' "$log")
  printf '%s' "$region" | grep -qaE "%DIRECT-W-NOFILES" && nofiles=1
  fid_line=$(printf '%s' "$region" | grep -aoE "PORTTEST\.DAT;1[^A-Za-z]*File ID:[[:space:]]*\([0-9]+,[0-9]+,[0-9]+\)" | tail -1)
  # CONTENT proof (vms-f49): DIRECTORY/FULL prints "Size: <used>/<alloc>" in
  # ODS-2 blocks. PT_SIZE=8192 bytes == 16 x 512-byte blocks, so the independent
  # reader must see used==16 -- proving the FULL committed content landed on the
  # real ODS-2 volume, not merely that a (possibly empty) directory entry exists.
  # This is strictly STRONGER than the old same-CRTL fwrite/fread round-trip (a
  # ramfs satisfies that identically; a ramfs/POSIX write can never appear in the
  # ACP directory with a real File ID AND the full content at all).
  size_line=$(printf '%s' "$region" | grep -aoE "Size:[[:space:]]*[0-9]+/[0-9]+" | tail -1)
  size_blocks=$(printf '%s' "$size_line" | grep -oE '[0-9]+' | head -1)
  [ -z "$size_blocks" ] && size_blocks=0
  if [ -n "$fid_line" ]; then
    fid_num=$(printf '%s' "$fid_line" | grep -oE '\([0-9]+' | tr -d '(' | tail -1)
    [ -n "$fid_num" ] && [ "$fid_num" -gt 0 ] && [ "$nofiles" -eq 0 ] && [ "$size_blocks" -eq 16 ] && reader_ok=1
  fi

  # (c) the image must have ACTIVATED: only genuine activation-LOAD failures are
  # fatal (they mean the port image never ran, so nothing could land). The
  # writer program's post-commit crash (SIGSEGV / %X0000002C exit) is the TRACKED
  # bug #4 below and is deliberately NOT in this list -- it fires AFTER the
  # content commits and cannot fake the landing (b), which is the pass key.
  local errs err_ok=1
  errs=$(grep -aE "%IMGACT-F|IMGNOTFND|DEVNOTMOUNT|NOSUCHFILE" "$log" 2>/dev/null || true)
  [ -n "$errs" ] && err_ok=0

  echo "  (a) veneer write ran (informational)       : port_ok=$port_ok  seam=${seam:-<ABSENT>}"
  echo "      decode: (${mile_hex:-<none>} - C\$_EXIT1 0x35a009)/8 + 1 = $sentinel  (7 = full round-trip; <7 expected while bug #4 open)"
  echo "  (b) INDEPENDENT ACP reader (DIRECTORY/FULL): ${fid_line:-<no PORTTEST.DAT;1 File-ID line>}  ${size_line:-<no Size line>}"
  echo "      nofiles=$nofiles  fid=$fid_num  size_blocks=$size_blocks (want fid>0 AND size==16 blocks==8192B==PT_SIZE; ramfs cannot produce this; reader_ok=$reader_ok)"
  echo "  (c) image activated (no load failure)      : ok=$err_ok"
  [ "$err_ok" -eq 0 ] && echo "      offending: $(printf '%s' "$errs" | tr '\n' '|')"

  # BANKED GATE (vms-f49 -- proven on its un-fakeable CORE). PASS = the INDEPENDENT
  # reader confirms PORTTEST.DAT;1 landed on the real ODS-2 volume with a genuine
  # File ID AND the full 8192-byte content (b), and the image actually activated
  # (c). This is the whole point of rung 4: the veneer's fopen->sys$create->RMS->
  # ACP->/dev/vms write truly committed to Files-11, proven by a DIFFERENT accessor
  # (DCL DIRECTORY/FULL's own sys$parse+sys$search over the ACP) -- something a
  # same-CRTL round-trip, or any ramfs/POSIX write, cannot establish. It has real
  # teeth on the write path: break the write and the reader draws %DIRECT-W-NOFILES
  # or a wrong size and THIS gate FAILS.
  #
  # The writer program's post-commit cleanup crash -- mallocng free -> free_group
  # -> free(g->mem) hitting get_meta's `assert(meta->mem==base)` with base->meta
  # NULL, in the veneer/stdio path AFTER the content committed -- is tracked as
  # vms-b14 bug #4 (mallocng group-release on the alpha-dec-vms substrate; deeper
  # than a typedef, next step is a stack-walk to pin the exact free frame) and
  # blocks vms-fd1. It does NOT affect this landing proof, so the writer's
  # sentinel=7 / exit status are informational only above, not pass-gating.
  [ "$reader_ok" -eq 1 ] && [ "$err_ok" -eq 1 ] && return 0
  return 1
}

# assert_fileop <console-log> -- THE TEETH for the vms-3320 CRTL->RMS FILE-OP
# veneer proof (`crtl-rms-fileop-gate' mode). Extends the vms-f49 veneer proof
# from the stdio family to open/creat/unlink/remove/rename/opendir/readdir/
# closedir: the port image (crtl_rms3_test.c) creats FOPCRE.DAT, creats+unlinks
# FOPDEL.DAT, creats+renames FOPSRC.DAT->FOPDST.DAT via the VECTOR-SUBSTITUTED
# decc$* file-ops, then the boot's INDEPENDENT reader (DCL DIRECTORY over the
# ACP -- a DIFFERENT accessor) inspects the volume. Pass iff, in the
# INDEPENDENT-reader region:
#   (b1) FOPCRE.DAT present WITH a nonzero ODS-2 File ID (creat landed);
#   (b2) FOPDST.DAT present WITH a nonzero ODS-2 File ID (rename target landed);
#   (b3) FOPDEL.DAT %DIRECT-W-NOFILES (unlink removed it);
#   (b4) FOPSRC.DAT %DIRECT-W-NOFILES (rename moved it away);
# AND (c) no activation-LOAD failure. The port program's own sentinel-7 self-
# enumeration is informational only (a post-op cleanup crash cannot undo a
# landing the independent reader already confirmed -- the vms-f49 banked-gate
# discipline). Pure function over the transcript; shared by the boot + selftest.
assert_fileop() {
  local log="$1"
  [ -f "$log" ] || { echo "  FAIL: no console log at $log"; return 1; }

  # (a) informational: the port image's own self-verify (sentinel 7 + OK line).
  local port_ok seam mile_hex sentinel="?"
  port_ok=$(grep -qaE "OVMX CRTL/RMS3 file-op test: OK" "$log" && echo 1 || echo 0)
  seam=$(grep -aoE "OVMX-SEAM: image=JOINT_E2E\.EXE[^\"]*STATUS=0x[0-9A-Fa-f]+" "$log" 2>/dev/null | tail -1)
  mile_hex=$(printf '%s' "$seam" | grep -oiE '0x[0-9a-f]+' | tail -1)
  if [ -n "$mile_hex" ] && [ "$(( mile_hex ))" -ge "$CEXIT1" ] && [ $(( ( $(( mile_hex )) - CEXIT1) % 8 )) -eq 0 ]; then
    sentinel=$(( ( $(( mile_hex )) - CEXIT1) / 8 + 1 ))
  fi

  # (b) THE TEETH -- confine to the INDEPENDENT-reader region, then sub-region by
  # the per-file "--- ... ---" markers so a token cannot leak across files.
  local region cre_reg dst_reg del_reg src_reg
  region=$(awk '/FILEOP-PROOF: === INDEPENDENT READER/{f=1} f{print} /FILEOP-PROOF: === END INDEPENDENT READER/{f=0}' "$log")
  cre_reg=$(printf '%s\n' "$region" | awk '/created file FOPCRE.DAT/{f=1} f{print} /FILEOP-PROOF: CRE-STATUS/{f=0}')
  dst_reg=$(printf '%s\n' "$region" | awk '/renamed target FOPDST.DAT/{f=1} f{print} /FILEOP-PROOF: DST-STATUS/{f=0}')
  del_reg=$(printf '%s\n' "$region" | awk '/unlinked file FOPDEL.DAT/{f=1} f{print} /FILEOP-PROOF: DEL-STATUS/{f=0}')
  src_reg=$(printf '%s\n' "$region" | awk '/rename source FOPSRC.DAT/{f=1} f{print} /FILEOP-PROOF: SRC-STATUS/{f=0}')

  local cre_fid dst_fid cre_ok=0 dst_ok=0 del_ok=0 src_ok=0
  cre_fid=$(printf '%s' "$cre_reg" | grep -aoE "FOPCRE\.DAT;[0-9]+[^A-Za-z]*File ID:[[:space:]]*\([0-9]+" | grep -oE '\([0-9]+' | tr -d '(' | tail -1)
  dst_fid=$(printf '%s' "$dst_reg" | grep -aoE "FOPDST\.DAT;[0-9]+[^A-Za-z]*File ID:[[:space:]]*\([0-9]+" | grep -oE '\([0-9]+' | tr -d '(' | tail -1)
  [ -n "$cre_fid" ] && [ "$cre_fid" -gt 0 ] && ! printf '%s' "$cre_reg" | grep -qaE "%DIRECT-W-NOFILES" && cre_ok=1
  [ -n "$dst_fid" ] && [ "$dst_fid" -gt 0 ] && ! printf '%s' "$dst_reg" | grep -qaE "%DIRECT-W-NOFILES" && dst_ok=1
  printf '%s' "$del_reg" | grep -qaE "%DIRECT-W-NOFILES" && del_ok=1
  printf '%s' "$src_reg" | grep -qaE "%DIRECT-W-NOFILES" && src_ok=1

  # (c) activation-LOAD failure only (a post-landing crash is not fatal here).
  local errs err_ok=1
  errs=$(grep -aE "%IMGACT-F|IMGNOTFND|DEVNOTMOUNT" "$log" 2>/dev/null || true)
  [ -n "$errs" ] && err_ok=0

  echo "  (a) port self-verify (informational)      : port_ok=$port_ok  seam=${seam:-<ABSENT>}  sentinel=$sentinel (7 = full)"
  echo "  (b) INDEPENDENT ACP reader (DIRECTORY):"
  echo "      b1 creat  FOPCRE.DAT present, fid=${cre_fid:-<none>}  ok=$cre_ok"
  echo "      b2 rename FOPDST.DAT present, fid=${dst_fid:-<none>}  ok=$dst_ok"
  echo "      b3 unlink FOPDEL.DAT GONE (%DIRECT-W-NOFILES)         ok=$del_ok"
  echo "      b4 rnsrc  FOPSRC.DAT GONE (%DIRECT-W-NOFILES)         ok=$src_ok"
  echo "  (c) image activated (no load failure)     : ok=$err_ok"
  [ "$err_ok" -eq 0 ] && echo "      offending: $(printf '%s' "$errs" | tr '\n' '|')"

  [ "$cre_ok" -eq 1 ] && [ "$dst_ok" -eq 1 ] && [ "$del_ok" -eq 1 ] && [ "$src_ok" -eq 1 ] && [ "$err_ok" -eq 1 ] && return 0
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

  # MILESTONE_EXTRA (vms-bdd): space-separated ADDITIONAL .c sources compiled into
  # their own objects and STRICT-linked alongside the milestone main -- the multi-.o
  # rung (`mf-gate' sets MILESTONE_MAIN=mf_main.c MILESTONE_EXTRA=mf_util.c so
  # mf_main.obj calls across the boundary into mf_util.obj). Empty for the N=3 and
  # N=7 gates, so they build byte-identically.
  # vms-f49: both the milestone AND the control build with the SAME veneer flag so
  # both link against the identical staged DECC$SHR (+ LIBVMSRMS$SHR) symbol vector
  # -- a veneer milestone with a non-veneer control would stage one DECC$SHR but
  # link the other image against a different one. Default 0 keeps every other mode
  # byte-identical.
  log "step 1a: build the milestone image ($MILESTONE_MAIN${MILESTONE_EXTRA:+ + $MILESTONE_EXTRA}, sentinel $WANT_SENTINEL${JOINT_CRTL_RMS_VENEER:+ veneer=$JOINT_CRTL_RMS_VENEER}) with the merged toolchain"
  JOINT_MAIN="$MILESTONE_MAIN" JOINT_EXTRA="${MILESTONE_EXTRA:-}" \
    JOINT_CRTL_RMS_VENEER="$JOINT_CRTL_RMS_VENEER" IMG="$VMS_IMG" bash "$bji" "$out_n3" \
    || die "build-joint-image.sh (milestone $MILESTONE_MAIN) failed -- see $out_n3/build.log"
  grep -q 'LINK-S-CREATED' "$out_n3/build.log" \
    || die "milestone image did not link (no %LINK-S-CREATED) -- see $out_n3/build.log"

  log "step 1b: build the SS\$_NORMAL control image (joint_main_ok.c, return 0) with the merged toolchain"
  JOINT_MAIN=joint_main_ok.c JOINT_CRTL_RMS_VENEER="$JOINT_CRTL_RMS_VENEER" IMG="$VMS_IMG" bash "$bji" "$out_ok" \
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
  # vms-f49: on a veneer build, stage LIBVMSRMS$SHR.EXE too -- build-joint-image.sh
  # emits it into $out_n3 whenever JOINT_CRTL_RMS_VENEER=1. Its presence in
  # $WORK/joint is exactly the signal build-alpha-bootimage.sh keys on to stage the
  # producer into SYS$SHARE + swap in the VENEER-proof SYSTARTUP.
  if [ "$JOINT_CRTL_RMS_VENEER" = 1 ]; then
    # vms-f49: the veneer image needs the FULL executive producer graph on
    # SYS$SHARE at activation, not just LIBVMSRMS$SHR -- LIBVMSRMS$SHR imports
    # (transitively) from LIBVMS/LIBVMSFS/LIBVMSLNM/LIBVMSPROCESS/LIBVMSSYS$SHR.
    # build-joint-image.sh emits all of them to $out_n3; stage each into
    # $WORK/joint so build-alpha-bootimage.sh masters them onto the ODS-2 volume.
    for _p in LIBVMSRMS LIBVMS LIBVMSFS LIBVMSLNM LIBVMSPROCESS LIBVMSSYS; do
      [ -s "$out_n3/${_p}\$SHR.EXE" ] \
        || die "veneer build produced no ${_p}\$SHR.EXE in $out_n3 (JOINT_CRTL_RMS_VENEER=1 expected the full producer graph)"
      cp "$out_n3/${_p}\$SHR.EXE" "$WORK/joint/${_p}\$SHR.EXE"
    done
    # vms-3320: carry the FILE-OP marker (dropped by build-joint-image.sh when
    # JOINT_MAIN=crtl_rms3_test.c) into $WORK/joint so build-alpha-bootimage.sh
    # stages the FILE-OP independent-reader SYSTARTUP (DIRECTORY of the FOP*.DAT
    # set) instead of the stdio VENEER one (which reads PORTTEST.DAT). The
    # selective staging above would otherwise drop it.
    [ -f "$out_n3/FILEOP_PROOF" ] && cp "$out_n3/FILEOP_PROOF" "$WORK/joint/FILEOP_PROOF"
    log "step 1: joint images staged into $WORK/joint (VENEER milestone N=$WANT_SENTINEL + control + DECC\$SHR/LIBOTS + full RMS producer graph LIBVMSRMS/LIBVMS/LIBVMSFS/LIBVMSLNM/LIBVMSPROCESS/LIBVMSSYS\$SHR)"
  else
    log "step 1: joint images staged into $WORK/joint (milestone N=$WANT_SENTINEL + SS\$_NORMAL control + producers)"
  fi
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
          -kernel vmlinux-boot -append "console=ttyS0 panic=-1 OVMX_IMGACT_SEAM=1 ${BOOT_APPEND_EXTRA:-}" \
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
  mf-gate)
    # vms-bdd: the multi-.o STRICT-link + activation rung. Same real-executive
    # boot as `crtl-rms-gate', but the MILESTONE image is a genuine MULTI-object
    # program: mf_main.obj calls ACROSS the .o boundary into mf_util.obj, which
    # pulls malloc/free/memcpy/strlen/strcmp from the genuine DECC$SHR. This is
    # the exact shape that hit %LINK-F-UNDEF decc$free before PR #795 (a SEPARATE
    # object referencing free() under a STRICT link); it now links zero-deferred
    # and must activate to sentinel 5. A LINK.EXE / mk_decc_shr.sh regression that
    # re-breaks the multi-.o STRICT link (or the weak-alias export) reds HERE.
    MILESTONE_MAIN=mf_main.c
    MILESTONE_EXTRA=mf_util.c
    WANT_SENTINEL=5

    # Prove assert_mf has teeth before trusting a green boot (mirrors the
    # crtl-rms-gate selftest discipline).
    _st=$(mktemp -d); _fails=0
    cat > "$_st/pass.log" <<'EOF'
OVMX multi-file port test: OK (cross-.o malloc/free/strlen/strcmp/memcpy) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a029
EOF
    cat > "$_st/crash.log" <<'EOF'
%DCL-F-ABORT, image SYS$SYSTEM:JOINT_E2E terminated abnormally (signal 11)
JOINT-E2E-PROOF: STATUS=%X0000002C SEVERITY=4
EOF
    cat > "$_st/wrong.log" <<'EOF'
OVMX multi-file port test: OK (cross-.o malloc/free/strlen/strcmp/memcpy) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
EOF
    echo "-- mf selftest 1/3: clean multi-.o N=5 proof must PASS --"
    if assert_mf "$_st/pass.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: clean proof rejected"; _fails=$((_fails+1)); fi
    echo "-- mf selftest 2/3: activation crash (no port line + ACCVIO) must FAIL --"
    if assert_mf "$_st/crash.log" >/dev/null 2>&1; then echo "  FAIL: crash accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- mf selftest 3/3: wrong sentinel (N=7 not 5) must FAIL --"
    if assert_mf "$_st/wrong.log" >/dev/null 2>&1; then echo "  FAIL: wrong sentinel accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    rm -rf "$_st"
    [ "$_fails" -eq 0 ] || die "mf selftest failed -- assert_mf cannot be trusted; aborting before the boot"
    echo ""

    build_joint_images
    assemble_boot_image
    log "step 3: BOOT A -- activate the multi-.o N=5 image (cross-.o libc surface) on the REAL executive"
    run_boot_a
    echo ""
    echo "========================================================================"
    echo "== vms-bdd multi-.o -> N=5: STRICT-linked multi-object program (mf_main +"
    echo "== mf_util, cross-.o malloc/free/strlen/strcmp/memcpy) activation on the"
    echo "== real OVMX/Alpha executive (qemu-system-alpha + /dev/vms, ODS-2 ACP)"
    echo "========================================================================"
    grep -aE "JOINT-E2E-PROOF:|OVMX-SEAM:|OVMX multi-file|%IMGACT|%DCL-" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    echo "------------------------------------------------------------------------"
    if assert_mf "$WORK/modgpA.log"; then
      echo ""
      echo "PASS: the multi-.o port image ACTIVATED on the real executive over the mounted"
      echo "      ODS-2 ACP; the cross-.o malloc/free/strlen/strcmp/memcpy round-trip"
      echo "      completed and \$STATUS = %X0035A029 (N=5). The STRICT multi-object link"
      echo "      (decc\$free export PR #795 + thunk-descriptor PR #958) holds at runtime."
      exit 0
    fi
    echo ""
    echo "FAIL: the multi-.o N=5 image did NOT cleanly activate (cross-.o libc + sentinel 5)"
    echo "      -- a REAL regression of the multi-object STRICT-link / activation path. Full log:"
    echo "      $WORK/modgpA.log"
    grep -aE "%IMGACT|%RUN-|%DCL-|IMGNOTFND|NOSUCHFILE|DEVNOTMOUNT|ACCVIO|SS\\\$_" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  /' | tail -20 || echo "  (none captured)"
    exit 1
    ;;
  crtl-rms-veneer-gate)
    # vms-f49 (rung 4 of vms-b4f): the un-fakeable CRTL->RMS veneer proof. Same
    # crtl_rms milestone image (heap+RMS+stdio, sentinel 7) as `crtl-rms-gate',
    # but built with the CRTL->RMS stdio VENEER wired in (JOINT_CRTL_RMS_VENEER=1)
    # so its decc$fopen/fwrite/fclose bind to sys$create/$put over the Files-11
    # ACP (-> LIBVMSRMS$SHR -> ioctl(/dev/vms)) instead of musl-POSIX. The proof
    # is NOT the port image's own console/CRTL read-back (a ramfs satisfies that
    # identically -- exactly what `crtl-rms-gate' cannot distinguish); it is an
    # INDEPENDENT reader (DCL DIRECTORY/FULL, a different accessor over the ACP)
    # asserting PORTTEST.DAT;1 exists on the ODS-2 volume with a genuine File ID.
    # This is what VALIDATES the vms-1fc LLP64 width fix at runtime: a truncated
    # ioctl pointer makes the veneer write reach nothing, and the independent
    # reader draws %DIRECT-W-NOFILES -> the gate reds.
    MILESTONE_MAIN=crtl_rms_veneer_test.c   # qualified VDA0:[SYSTMP]PORTTEST.DAT (vms-f49)
    WANT_SENTINEL=7
    JOINT_CRTL_RMS_VENEER=1

    # Prove assert_veneer has teeth before trusting a green boot. The key case is
    # the NEGATIVE/REJECTION one (2/6): a same-CRTL success (port-test OK + N=7
    # seam) that a ramfs satisfies IDENTICALLY must FAIL when the INDEPENDENT
    # DIRECTORY reader shows %DIRECT-W-NOFILES -- proving the reader, not the
    # console/CRTL state, is what gates. A gate that cannot fail certifies nothing.
    _st=$(mktemp -d); _fails=0
    cat > "$_st/pass.log" <<'EOF'
OVMX CRTL/RMS port test: wrote+read 8192 bytes via 'PORTTEST.DAT', pattern verified
OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL PORTTEST.DAT (a DIFFERENT accessor over the ACP) ===

Directory DKA0:[SYSMGR]

PORTTEST.DAT;1                 File ID:  (14,1,0)
Size:            16/16          Owner:    [001,004]

Total of 1 file.
VENEER-PROOF: DIR-STATUS=%X00000001 SEVERITY=1
EOF
    # NEGATIVE: same-CRTL success but the file landed on ramfs -> the independent
    # ACP reader finds nothing. MUST FAIL.
    cat > "$_st/ramfs.log" <<'EOF'
OVMX CRTL/RMS port test: wrote+read 8192 bytes via 'PORTTEST.DAT', pattern verified
OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL PORTTEST.DAT (a DIFFERENT accessor over the ACP) ===
%DIRECT-W-NOFILES, no files found
VENEER-PROOF: DIR-STATUS=%X00018292 SEVERITY=0
EOF
    # NO File ID line (a from_acp=0 / passthrough-style entry with no ODS-2 File
    # ID) -> MUST FAIL: the File ID is the un-fakeable token.
    cat > "$_st/nofid.log" <<'EOF'
OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL PORTTEST.DAT (a DIFFERENT accessor over the ACP) ===
PORTTEST.DAT;1
VENEER-PROOF: DIR-STATUS=%X00000001 SEVERITY=1
EOF
    # ZERO File ID -> MUST FAIL (a genuine ODS-2 create never mints fid 0).
    cat > "$_st/zerofid.log" <<'EOF'
OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL PORTTEST.DAT (a DIFFERENT accessor over the ACP) ===
PORTTEST.DAT;1                 File ID:  (0,0,0)
VENEER-PROOF: DIR-STATUS=%X00000001 SEVERITY=1
EOF
    # WRONG CONTENT SIZE (fid present, but only a partial 8/16-block landing) ->
    # MUST FAIL. The content-size assertion (used == 16 blocks == 8192B == PT_SIZE)
    # is what proves the FULL committed content landed, not merely a directory
    # entry; a short/partial write must not pass. (Supersedes the old "wrong
    # sentinel" fixture -- the writer's sentinel is no longer pass-gating, see the
    # banked-gate note in assert_veneer / vms-b14 bug #4.)
    cat > "$_st/wrongsize.log" <<'EOF'
OVMX CRTL/RMS port test: OK (heap+RMS+stdio) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL PORTTEST.DAT (a DIFFERENT accessor over the ACP) ===
PORTTEST.DAT;1                 File ID:  (14,1,0)
Size:             8/16          Owner:    [001,004]
VENEER-PROOF: DIR-STATUS=%X00000001 SEVERITY=1
EOF
    # ACTIVATION/PRE-LANDING CRASH (image died with NO independent-reader landing)
    # -> MUST FAIL: a crash before the content commits leaves nothing in the ACP
    # directory, so reader_ok=0. (Distinct from the banked case below, which has a
    # crash AND a real landing.)
    cat > "$_st/crash.log" <<'EOF'
%DCL-F-ABORT, image SYS$SYSTEM:JOINT_E2E terminated abnormally (signal 11)
JOINT-E2E-PROOF: STATUS=%X0000002C SEVERITY=4
EOF
    # BANKED-PASS REALITY (vms-b14 bug #4): the writer SIGSEGVs in post-commit
    # cleanup (no port-test OK line, %X0000002C exit, signal 11) but the content
    # ALREADY committed, so the INDEPENDENT reader still sees a real fid + the full
    # 16-block size -> MUST PASS. This is exactly the current boot; the cleanup
    # crash cannot fake or undo the proven landing.
    cat > "$_st/bankcrash.log" <<'EOF'
%DCL-F-ABORT, image SYS$SYSTEM:JOINT_E2E terminated abnormally (signal 11)
JOINT-E2E-PROOF: STATUS=%X0000002C SEVERITY=4
VENEER-PROOF: === INDEPENDENT READER: DIRECTORY/FULL VDA0:[SYSTMP]PORTTEST.DAT (a DIFFERENT accessor over the ACP) ===
PORTTEST.DAT;1                 File ID:  (71,1,0)
Size:            16/16          Owner:    [001,004]
VENEER-PROOF: DIR-STATUS=%X00000001 SEVERITY=1
EOF
    echo "-- veneer selftest 1/7: clean veneer write + independent File-ID+size reader must PASS --"
    if assert_veneer "$_st/pass.log"     >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: clean proof rejected"; _fails=$((_fails+1)); fi
    echo "-- veneer selftest 2/7: same-CRTL success but ramfs (%DIRECT-W-NOFILES) must FAIL --"
    if assert_veneer "$_st/ramfs.log"    >/dev/null 2>&1; then echo "  FAIL: ramfs round-trip accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- veneer selftest 3/7: PORTTEST.DAT;1 with NO File ID line must FAIL --"
    if assert_veneer "$_st/nofid.log"    >/dev/null 2>&1; then echo "  FAIL: missing File ID accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- veneer selftest 4/7: zero File ID (0,0,0) must FAIL --"
    if assert_veneer "$_st/zerofid.log"  >/dev/null 2>&1; then echo "  FAIL: zero File ID accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- veneer selftest 5/7: wrong content size (partial 8/16-block landing) must FAIL --"
    if assert_veneer "$_st/wrongsize.log" >/dev/null 2>&1; then echo "  FAIL: partial-size landing accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- veneer selftest 6/7: pre-landing crash (no independent-reader landing) must FAIL --"
    if assert_veneer "$_st/crash.log"    >/dev/null 2>&1; then echo "  FAIL: crash-without-landing accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- veneer selftest 7/7: banked reality -- post-commit crash BUT real fid+size landing must PASS (vms-b14 #4) --"
    if assert_veneer "$_st/bankcrash.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: banked post-commit-crash landing rejected"; _fails=$((_fails+1)); fi
    rm -rf "$_st"
    [ "$_fails" -eq 0 ] || die "veneer selftest failed -- assert_veneer cannot be trusted; aborting before the boot"
    echo ""

    build_joint_images
    assemble_boot_image
    log "step 3: BOOT A -- activate the VENEER crtl_rms image + run the INDEPENDENT DIRECTORY reader on the REAL executive"
    run_boot_a
    echo ""
    echo "========================================================================"
    echo "== vms-f49 rung 4: CRTL->RMS veneer -> real ODS-2 landing, PROVEN by an"
    echo "== INDEPENDENT ACP reader (DIRECTORY/FULL File ID) on the real OVMX/Alpha"
    echo "== executive (qemu-system-alpha + /dev/vms). Validates the vms-1fc width fix."
    echo "========================================================================"
    grep -aE "VENEER-PROOF:|OVMX CRTL/RMS|OVMX-SEAM:|PORTTEST\.DAT|File ID:|%DIRECT|%IMGACT|%DCL-" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    echo "------------------------------------------------------------------------"
    if assert_veneer "$WORK/modgpA.log"; then
      echo ""
      echo "PASS: the veneer-wired port image's decc\$fopen genuinely landed PORTTEST.DAT on"
      echo "      the real Files-11 ODS-2 volume over the ACP -- an INDEPENDENT reader"
      echo "      (DIRECTORY/FULL, a different accessor than the writer's CRTL/RMS handle)"
      echo "      returned a genuine ODS-2 File ID AND the full 8192-byte content (16 blocks),"
      echo "      which a ramfs/POSIX write can never produce in the ACP directory. The vms-1fc"
      echo "      LLP64 width fix holds at runtime (the ioctl pointer was NOT truncated), and"
      echo "      the vms-b4f toolchain fixes (emutls / sv# skew / calloc weak-override reloc)"
      echo "      compose end to end. NOTE: the writer's post-commit mallocng cleanup crash is"
      echo "      tracked as vms-b14 bug #4 and does not affect this proven landing."
      exit 0
    fi
    echo ""
    echo "FAIL: the veneer write did NOT land on the real ODS-2 volume with its full content"
    echo "      (the INDEPENDENT ACP reader saw no genuine File ID, or a wrong/partial size)."
    echo "      %DIRECT-W-NOFILES / a missing File ID / size != 16 blocks means the veneer's"
    echo "      fopen->sys\$create->RMS->ioctl(/dev/vms)->ACP write regressed (e.g. the vms-1fc"
    echo "      truncated-pointer symptom, or a broken producer link). Full log: $WORK/modgpA.log"
    grep -aE "VENEER-PROOF:|%IMGACT|%RUN-|%DCL-|IMGNOTFND|NOSUCHFILE|DEVNOTMOUNT|ACCVIO|%DIRECT|SS\\\$_" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  /' | tail -25 || echo "  (none captured)"
    echo "--- guest-kernel fault signature (if the image faulted) ---"
    grep -aiE "memory violation|segmentation|segfault|unaligned|Oops|BUG:|bad address|panic" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  /' | tail -20 || echo "  (no guest fault line captured)"
    echo "--- last 60 console lines ---"
    tail -60 "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    exit 1
    ;;
  crtl-rms-fileop-gate)
    # vms-3320 (parent vms-b4f, blocks vms-fd1): the un-fakeable CRTL->RMS
    # FILE-OP veneer proof -- open/creat/unlink/remove/rename/opendir/readdir/
    # closedir beyond the stdio family. Same VENEER path as crtl-rms-veneer-gate
    # (JOINT_CRTL_RMS_VENEER=1) but the MILESTONE image is crtl_rms3_test.c: it
    # creats FOPCRE.DAT, creats+unlinks FOPDEL.DAT, creats+renames FOPSRC.DAT->
    # FOPDST.DAT via the VECTOR-SUBSTITUTED decc$* file-ops (bound by sv# index
    # to the crtl_rms_stdio.c veneer -> sys$create/$erase/$rename -> the ACP),
    # leaving FOPCRE.DAT + FOPDST.DAT behind. The proof is an INDEPENDENT reader
    # (DCL DIRECTORY over the ACP, a different accessor) seeing FOPCRE.DAT +
    # FOPDST.DAT with genuine ODS-2 File IDs and FOPDEL.DAT/FOPSRC.DAT gone.
    MILESTONE_MAIN=crtl_rms3_test.c
    WANT_SENTINEL=7
    JOINT_CRTL_RMS_VENEER=1
    # Fault-capture: print the user PC of any fatal signal so a crash (e.g. the
    # pre-existing veneer-build mallocng crash vms-c5d) can be mapped to a symbol
    # (subtract the DECC$SHR +0x2c000 slide: file_off = VA - 0x20000000000 - 0x2c000).
    export BOOT_APPEND_EXTRA="ignore_loglevel print-fatal-signals=1 loglevel=8"

    # Prove assert_fileop has teeth before trusting a green boot (mirrors the
    # crtl-rms-veneer-gate selftest discipline: a can-fail gate certifies nothing).
    _st=$(mktemp -d); _fails=0
    cat > "$_st/pass.log" <<'EOF'
OVMX CRTL/RMS3 file-op test: OK (open+creat+unlink+rename+opendir+readdir+closedir over RMS) argc=1
OVMX-SEAM: image=JOINT_E2E.EXE stdcall_returned=1 has_exited=1 $STATUS=0x0035a039
FILEOP-PROOF: === INDEPENDENT READER: DIRECTORY over the ACP (a DIFFERENT accessor) ===
FILEOP-PROOF: --- created file FOPCRE.DAT (must be present) ---
FOPCRE.DAT;1                   File ID:  (21,1,0)
FILEOP-PROOF: CRE-STATUS=%X00000001 SEVERITY=1
FILEOP-PROOF: --- renamed target FOPDST.DAT (must be present, genuine File ID) ---
FOPDST.DAT;1                   File ID:  (23,1,0)
FILEOP-PROOF: DST-STATUS=%X00000001 SEVERITY=1
FILEOP-PROOF: --- unlinked file FOPDEL.DAT (must be GONE) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: DEL-STATUS=%X00018292 SEVERITY=0
FILEOP-PROOF: --- rename source FOPSRC.DAT (must be GONE) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: SRC-STATUS=%X00018292 SEVERITY=0
FILEOP-PROOF: === END INDEPENDENT READER ===
EOF
    # NEGATIVE 1: creat did NOT land (independent reader sees FOPCRE.DAT gone). FAIL.
    cat > "$_st/nocre.log" <<'EOF'
FILEOP-PROOF: === INDEPENDENT READER: DIRECTORY over the ACP (a DIFFERENT accessor) ===
FILEOP-PROOF: --- created file FOPCRE.DAT (must be present) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: CRE-STATUS=%X00018292 SEVERITY=0
FILEOP-PROOF: --- renamed target FOPDST.DAT (must be present, genuine File ID) ---
FOPDST.DAT;1                   File ID:  (23,1,0)
FILEOP-PROOF: DST-STATUS=%X00000001 SEVERITY=1
FILEOP-PROOF: --- unlinked file FOPDEL.DAT (must be GONE) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: --- rename source FOPSRC.DAT (must be GONE) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: === END INDEPENDENT READER ===
EOF
    # NEGATIVE 2: unlink did NOT remove FOPDEL.DAT (reader still sees it). FAIL.
    cat > "$_st/nodel.log" <<'EOF'
FILEOP-PROOF: === INDEPENDENT READER: DIRECTORY over the ACP (a DIFFERENT accessor) ===
FILEOP-PROOF: --- created file FOPCRE.DAT (must be present) ---
FOPCRE.DAT;1                   File ID:  (21,1,0)
FILEOP-PROOF: --- renamed target FOPDST.DAT (must be present, genuine File ID) ---
FOPDST.DAT;1                   File ID:  (23,1,0)
FILEOP-PROOF: --- unlinked file FOPDEL.DAT (must be GONE) ---
FOPDEL.DAT;1                   File ID:  (22,1,0)
FILEOP-PROOF: --- rename source FOPSRC.DAT (must be GONE) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: === END INDEPENDENT READER ===
EOF
    # NEGATIVE 3: activation load failure -> FAIL even if a stale region parsed.
    cat > "$_st/imgact.log" <<'EOF'
%IMGACT-F-IMGNOTFND, image file not found LIBVMSRMS$SHR
FILEOP-PROOF: === INDEPENDENT READER: DIRECTORY over the ACP (a DIFFERENT accessor) ===
FILEOP-PROOF: --- created file FOPCRE.DAT (must be present) ---
FOPCRE.DAT;1                   File ID:  (21,1,0)
FILEOP-PROOF: --- renamed target FOPDST.DAT (must be present, genuine File ID) ---
FOPDST.DAT;1                   File ID:  (23,1,0)
FILEOP-PROOF: --- unlinked file FOPDEL.DAT (must be GONE) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: --- rename source FOPSRC.DAT (must be GONE) ---
%DIRECT-W-NOFILES, no files found
FILEOP-PROOF: === END INDEPENDENT READER ===
EOF
    echo "-- fileop selftest 1/4: clean independent-reader proof must PASS --"
    if assert_fileop "$_st/pass.log" >/dev/null 2>&1; then echo "  PASS"; else echo "  FAIL: clean proof rejected"; _fails=$((_fails+1)); fi
    echo "-- fileop selftest 2/4: creat did not land (FOPCRE gone) must FAIL --"
    if assert_fileop "$_st/nocre.log" >/dev/null 2>&1; then echo "  FAIL: accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- fileop selftest 3/4: unlink no-op (FOPDEL still present) must FAIL --"
    if assert_fileop "$_st/nodel.log" >/dev/null 2>&1; then echo "  FAIL: accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    echo "-- fileop selftest 4/4: activation load failure must FAIL --"
    if assert_fileop "$_st/imgact.log" >/dev/null 2>&1; then echo "  FAIL: accepted"; _fails=$((_fails+1)); else echo "  PASS (rejected)"; fi
    rm -rf "$_st"
    [ "$_fails" -eq 0 ] || die "fileop selftest failed -- assert_fileop cannot be trusted; aborting before the boot"
    echo ""

    build_joint_images
    assemble_boot_image
    log "step 3: BOOT A -- activate the FILE-OP veneer image + run the INDEPENDENT DIRECTORY reader on the REAL executive"
    run_boot_a
    echo ""
    echo "========================================================================"
    echo "== vms-3320: CRTL->RMS FILE-OP veneer (open/creat/unlink/remove/rename/"
    echo "== opendir/readdir/closedir) -> real ODS-2 effects, PROVEN by an INDEPENDENT"
    echo "== ACP reader (DIRECTORY) on the real OVMX/Alpha executive (qemu-system-alpha"
    echo "== + /dev/vms). Blocks vms-fd1 (the full alpha-dec-vms GCC port)."
    echo "========================================================================"
    grep -aE "FILEOP-PROOF:|OVMX CRTL/RMS3|OVMX-SEAM:|FOP...\.DAT|File ID:|%DIRECT|%IMGACT|%DCL-" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    echo "------------------------------------------------------------------------"
    if assert_fileop "$WORK/modgpA.log"; then
      echo ""
      echo "PASS: the FILE-OP veneer-wired port image's decc\$open/creat/unlink/rename/"
      echo "      opendir/readdir/closedir genuinely reached the real Files-11 ODS-2 volume"
      echo "      over the ACP -- an INDEPENDENT reader (DCL DIRECTORY, a different accessor"
      echo "      than the writer's CRTL handle) saw FOPCRE.DAT + FOPDST.DAT with genuine ODS-2"
      echo "      File IDs and FOPDEL.DAT/FOPSRC.DAT GONE, which a ramfs/POSIX write can never"
      echo "      produce in the ACP directory. The 8 file-op decc\$ names bind by symbol-vector"
      echo "      INDEX to the veneer (mk_decc_shr.sh in-place substitution, sv# stable)."
      exit 0
    fi
    echo ""
    echo "FAIL: the file-op veneer did NOT reach the real ODS-2 volume as expected (the"
    echo "      INDEPENDENT reader disagreed: a created/renamed file missing a File ID, or a"
    echo "      deleted/renamed-away file still present). Full log: $WORK/modgpA.log"
    grep -aE "FILEOP-PROOF:|%IMGACT|%RUN-|%DCL-|IMGNOTFND|NOSUCHFILE|DEVNOTMOUNT|ACCVIO|%DIRECT|SS\\\$_" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  /' | tail -30 || echo "  (none captured)"
    echo "--- last 60 console lines ---"
    tail -60 "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    exit 1
    ;;
  shipped-gate)
    # vms-410: proves the Alpha shareable graph ships as part of the ORDINARY
    # build-alpha-bootimage.sh packaging path, not as something only THIS
    # gate's own build_joint_images() knows to inject. `gate`/`crtl-rms-gate`/
    # `mf-gate` above all pre-stage $WORK/joint themselves (their own
    # milestone image) before calling assemble_boot_image; this mode
    # deliberately does the OPPOSITE -- it clears $WORK/joint first, so the
    # ONLY way DECC$SHR.EXE / LIBOTS_SHR.EXE / JOINT_E2E.EXE can land on the
    # mastered ODS-2 volume's SYS$SHARE is build-alpha-bootimage.sh's OWN
    # built-in shareable-graph build (added for vms-410). If that packaging
    # step regresses (or is ever reverted), $WORK/joint stays empty,
    # assemble_boot_image's SYSTARTUP swap never triggers, and this gate
    # fails HONESTLY on the missing-shareable assertion below rather than
    # silently falling back to the caller-injection pattern. Reuses
    # assert_activation() unchanged: build-alpha-bootimage.sh's default
    # packaging builds the SAME joint_main.c/joint_main_ok.c milestone+control
    # pair as `gate`, so the real-executive activation teeth are identical.
    log "verifying the gate can fail (selftest) before the real boot"
    selftest || die "selftest failed -- assert_activation() cannot be trusted; aborting before the boot"
    echo ""
    log "step 1: clear any pre-staged joint artifacts -- the SHIPPED packaging path alone must supply them"
    rm -rf "$WORK/joint"
    assemble_boot_image
    [ -s "$WORK/joint/DECC\$SHR.EXE" ] && [ -s "$WORK/joint/LIBOTS_SHR.EXE" ] && [ -s "$WORK/joint/joint_e2e.exe" ] \
        || die "build-alpha-bootimage.sh's own packaging did not stage the shareable graph into \$WORK/joint -- vms-410 packaging regression (no caller pre-staged it, so this is the ONLY source)"
    log "step 3: BOOT A -- activate the SHIPPED single-proc N=3 image against the SYSLIB-staged shareables on the REAL executive"
    run_boot_a
    echo ""
    echo "========================================================================"
    echo "== vms-410 shipped shareable graph: SYSLIB-staged genuine alpha DECC\$SHR +"
    echo "== LIBOTS\$SHR (build-alpha-bootimage.sh's own packaging, not caller-"
    echo "== injected) activation on the real OVMX/Alpha executive"
    echo "========================================================================"
    grep -aE "JOINT-E2E-PROOF:|OVMX crt0 join|OVMX-SEAM:|%IMGACT|%DCL-" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  | /' || true
    echo "------------------------------------------------------------------------"
    if assert_activation "$WORK/modgpA.log"; then
      echo ""
      echo "PASS: the SHIPPED Alpha boot image's own packaging staged the genuine DECC\$SHR +"
      echo "      LIBOTS\$SHR into SYS\$SHARE, and a consumer image bound against them ACTIVATED"
      echo "      on the real executive over the mounted ODS-2 ACP; \$STATUS = %X0035A019 (N=3)."
      echo "      The SS\$_NORMAL control anchors value-sensitivity; no activation-failure"
      echo "      %-error appeared. Alpha ships + activates real VMS shareable images (vms-410)."
      exit 0
    fi
    echo ""
    echo "FAIL: the SHIPPED single-proc N=3 image did NOT cleanly activate against the"
    echo "      packaging-staged shareables on the real executive -- a REAL vms-410 packaging"
    echo "      or activation regression. Full log: $WORK/modgpA.log"
    echo "--- activation-failure signatures ---"
    grep -aE "%IMGACT|%RUN-|%DCL-|IMGNOTFND|NOSUCHFILE|DEVNOTMOUNT|ACCVIO|SS\\\$_" "$WORK/modgpA.log" 2>/dev/null | sed 's/^/  /' | tail -20 || echo "  (none captured)"
    exit 1
    ;;
  *)
    die "unknown mode '$MODE' (use: gate | crtl-rms-gate | crtl-rms-veneer-gate | mf-gate | shipped-gate | selftest)"
    ;;
esac
