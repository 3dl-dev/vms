#!/bin/bash
# entrypoint.sh -- boot NetBSD/vax under SIMH and assert it is really NetBSD/vax.
#
# rd vms-0041 (epic vms-8e8). The proving ground for the OVMX/NetBSD SYSKRNL:
# P3 (libvmssys VAX backend) and P4 (OVMX/NetBSD-vax boots + executive test)
# build on top of "we can install and boot NetBSD/vax non-interactively, in a
# container, and prove what booted."
#
# MODES (first argument, default 'smoke'):
#   install  Install NetBSD/vax once from the pinned ISO, producing the cached
#            SIMH disk image ($WORKDIR/wd0.img). SLOW (VAX is a ~1980s CPU under
#            emulation) -- tens of minutes. Idempotent: a warm cache is a no-op.
#   smoke    Boot the CACHED disk (fast), log in, run `uname -srm`, and assert
#            the output is exactly "NetBSD <version> vax". Exit 0 iff it is.
#   negctl   Negative control: run the same boot+assert but demand the WRONG
#            uname ("NetBSD <version> hppa"). The assertion MUST fail; negctl
#            exits 0 only when the harness correctly reported failure -- proving
#            the smoke gate has teeth and cannot be a rubber stamp.
#   all      install, then smoke (for a cold local run).
#
# EVERY SIMH invocation is wrapped in a HARD `timeout`. A hang FAILS, it does not
# spin: a prior emulator proof in this repo once ran unbounded for 1h43m. anita
# owns the pexpect child that drives SIMH over a pty; on timeout, `timeout` kills
# anita, the pty closes, and SIMH exits. (Unlike lab-alpha's AXPbox, which dies
# if any console client disconnects, SIMH-VAX is driven by a single long-lived
# stdio console for the whole run, so there is no reconnect trap -- the only way
# the console closes mid-run is our own timeout kill, which is the intended
# failure path.)
set -euo pipefail

MODE="${1:-smoke}"

# --- pinned, reproducible inputs ---------------------------------------------
# NEVER "latest". NetBSD/vax 10.1, from the official NetBSD CDN, verified by the
# checksum published in the release's images/SHA512 file. 10.1 matches the
# NetBSD version the OVMX/NetBSD-vax cross toolchain + syssrc are pinned to
# (tools/cross-vax/Dockerfile, tests/netbsd/netbsd_version.env), so a cross-built
# elf32-vax kernel module (P4-B, rd vms-f78bb) is ABI-matched to THIS running
# kernel -- the lab substrate and the cross-build target are one version.
NETBSD_VERSION="${NETBSD_VERSION:-10.1}"
ISO_NAME="NetBSD-${NETBSD_VERSION}-vax.iso"
ISO_URL="${ISO_URL:-https://cdn.netbsd.org/pub/NetBSD/NetBSD-${NETBSD_VERSION}/images/${ISO_NAME}}"
# SHA512 of NetBSD-10.1-vax.iso (cdn.netbsd.org .../NetBSD-10.1/images/SHA512).
# If you bump NETBSD_VERSION you MUST update this from that release's SHA512.
ISO_SHA512="${ISO_SHA512:-aa763aa2240e4623adf09dd1a1ed2da0e3b96959d33544d52026a0c7c7448c6f0da8517bf059b9c53a9786782c0373b2e3da84de4b36cc5aeb669d219ac0f225}"

# Minimal set list -- all we need to boot to a shell and answer uname. Fewer
# sets means a dramatically shorter VAX install and a smaller cached disk.
SETS="${SETS:-kern-GENERIC,base,etc}"

CACHE="${CACHE:-/cache}"
WORKDIR="${WORKDIR:-${CACHE}/anita-work}"
ISO_PATH="${CACHE}/${ISO_NAME}"

# What a genuine NetBSD/vax `uname -srm` must print.
EXPECT_UNAME="${EXPECT_UNAME:-NetBSD ${NETBSD_VERSION} vax}"
# The deliberately-wrong expectation the negative control demands.
WRONG_UNAME="${WRONG_UNAME:-NetBSD ${NETBSD_VERSION} hppa}"

# Hard caps (seconds). Override for a faster forced-timeout negative control.
INSTALL_TIMEOUT="${INSTALL_TIMEOUT:-5400}"   # 90 min
BOOT_TIMEOUT="${BOOT_TIMEOUT:-1200}"         # 20 min

ANITA="${ANITA:-anita}"

log() { echo "[lab-vax] $*"; }
die() { echo "[lab-vax] FATAL: $*" >&2; exit 1; }

banner() {
  log "SIMH:  $(cat /etc/simh-version.txt 2>/dev/null || echo '?')"
  log "anita: $(cat /etc/anita-version.txt 2>/dev/null || echo '?') / $("${ANITA}" --version 2>/dev/null || true)"
  log "NetBSD/vax ${NETBSD_VERSION}  sets=${SETS}  cache=${CACHE}"
}

# --- pinned ISO fetch + checksum verify --------------------------------------
fetch_iso() {
  mkdir -p "${CACHE}"
  if [ -f "${ISO_PATH}" ] && echo "${ISO_SHA512}  ${ISO_PATH}" | sha512sum -c --status -; then
    log "ISO present and verified: ${ISO_PATH}"
    return 0
  fi
  log "downloading ${ISO_URL}"
  curl -fSL --retry 3 --connect-timeout 30 -o "${ISO_PATH}.part" "${ISO_URL}"
  echo "${ISO_SHA512}  ${ISO_PATH}.part" | sha512sum -c --status - \
    || { rm -f "${ISO_PATH}.part"; die "ISO checksum mismatch for ${ISO_NAME} -- refusing to use it"; }
  mv "${ISO_PATH}.part" "${ISO_PATH}"
  log "ISO verified (SHA512 OK): ${ISO_PATH}"
}

# --- install: produce the cached disk once -----------------------------------
do_install() {
  fetch_iso
  mkdir -p "${WORKDIR}"
  if [ -f "${WORKDIR}/wd0.img" ]; then
    log "cached disk already present: ${WORKDIR}/wd0.img -- install is a no-op"
    return 0
  fi
  log "installing NetBSD/vax ${NETBSD_VERSION} under SIMH (SLOW; hard cap ${INSTALL_TIMEOUT}s)"
  # anita picks the simh vmm for the vax arch automatically; --vmm simh is
  # explicit belt-and-suspenders. It screen-scrapes sysinst over the serial
  # console and leaves the installed system in ${WORKDIR}/wd0.img.
  timeout --signal=KILL "${INSTALL_TIMEOUT}" \
    "${ANITA}" --workdir "${WORKDIR}" --vmm simh --sets "${SETS}" \
      --structured-log-file "${WORKDIR}/install-console.log" \
      install "${ISO_PATH}"
  [ -f "${WORKDIR}/wd0.img" ] || die "install finished but ${WORKDIR}/wd0.img is missing"
  log "install complete. cached disk: $(ls -lh "${WORKDIR}/wd0.img" | awk '{print $5}')"
}

# --- boot the cached disk, run uname, assert against $want -------------------
# Returns 0 iff the guest's `uname -srm` equals $want AND that line is present
# in the console transcript. Prints the transcript's uname line for the record.
boot_and_assert() {
  local want="$1" tlog="$2"
  fetch_iso
  [ -f "${WORKDIR}/wd0.img" ] || die "no cached disk (${WORKDIR}/wd0.img) -- run 'install' first"

  # In-guest assertion: print the real uname (for the transcript), then demand
  # an EXACT-line match of $want. `grep -qx` sets anita's exit status, because
  # anita's `boot --run` exits with the guest command's $?.
  local runcmd="uname -srm; uname -srm | grep -qx '${want}'"

  log "booting cached disk; asserting uname == '${want}' (hard cap ${BOOT_TIMEOUT}s)"
  local rc=0
  timeout --signal=KILL "${BOOT_TIMEOUT}" \
    "${ANITA}" --workdir "${WORKDIR}" --vmm simh --no-install \
      --structured-log-file "${tlog}" \
      --run "${runcmd}" --run-timeout "${BOOT_TIMEOUT}" \
      boot "${ISO_PATH}" || rc=$?

  # Secondary, independent confirmation from the transcript itself, so a pass
  # never rests on anita's exit-code plumbing alone.
  local seen=1
  grep -aqF "NetBSD ${NETBSD_VERSION} vax" "${tlog}" 2>/dev/null && seen=0

  log "anita exit=${rc}; transcript-contains-real-uname=$([ ${seen} -eq 0 ] && echo yes || echo no)"
  echo "----- guest uname line(s) in transcript -----"
  grep -aE 'NetBSD [0-9.]+ (vax|hppa|amd64|i386)' "${tlog}" 2>/dev/null | sed 's/^/  /' | head -5 || true
  echo "---------------------------------------------"

  # Pass = anita ran the assertion and it succeeded (rc 0) AND we actually saw
  # a real NetBSD/vax uname line on the wire.
  if [ "${rc}" -eq 0 ] && [ "${seen}" -eq 0 ]; then
    return 0
  fi
  return 1
}

do_smoke() {
  local tlog="${WORKDIR}/smoke-console.log"
  mkdir -p "${WORKDIR}"
  if boot_and_assert "${EXPECT_UNAME}" "${tlog}"; then
    log "PASS: NetBSD/vax booted under SIMH and uname -srm == '${EXPECT_UNAME}'"
    exit 0
  fi
  die "SMOKE FAILED: did not observe a healthy NetBSD/vax with uname == '${EXPECT_UNAME}' (see ${tlog})"
}

do_negctl() {
  local tlog="${WORKDIR}/negctl-console.log"
  mkdir -p "${WORKDIR}"
  log "NEGATIVE CONTROL: demanding the WRONG uname '${WRONG_UNAME}' -- this MUST fail"
  if boot_and_assert "${WRONG_UNAME}" "${tlog}"; then
    die "NEGATIVE CONTROL DID NOT FAIL: the harness accepted a wrong assertion -- the gate has no teeth"
  fi
  log "PASS: negative control failed as required (harness can distinguish wrong from right)"
  exit 0
}

banner
case "${MODE}" in
  install) do_install ;;
  smoke)   do_smoke ;;
  negctl)  do_negctl ;;
  all)     do_install; do_smoke ;;
  *)       die "unknown mode '${MODE}' (want: install | smoke | negctl | all)" ;;
esac
