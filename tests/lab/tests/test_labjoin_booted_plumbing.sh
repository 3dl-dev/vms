#!/bin/bash
# test_labjoin_booted_plumbing.sh (rd vms-fa1a) - CI-runnable proof that the
# booted-OVMX cluster-join harness's DECISION LOGIC is correct, WITHOUT a k3s
# cluster, a VAX, or a boot.
#
# The heavy acceptance run (tools/labjoin_booted.sh against lab-2) needs the
# genuine OpenVMS VAX oracle and a slow in-pod TCG boot -- it cannot run in
# GitHub CI. But every DECISION the harness makes is a pure function of text:
# the identity collision guard, the CN_2 precheck, the QEMU tap-arg contract, and
# the four-leg verdict. This test exercises exactly that code (the real
# labjoin_lib.sh, and labjoin_booted.sh downstream of a MOCKED kubectl -- same
# technique as test_lab2run_staging.sh), so the instrument that grades the real
# lab-2 run is itself proven correct here. Its can-fail twin is
# test_labjoin_booted_negctl.sh.
#
# Exit 0 = all plumbing checks pass.
set -u
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS="$HERE/../tools"
# shellcheck source=../tools/labjoin_lib.sh
. "$TOOLS/labjoin_lib.sh"

PASS=0; FAIL=0
ok()   { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

echo "=== labjoin harness plumbing (vms-fa1a) ==="

# --- 1. Identity guard: reserved pod VAX ids refused, a free id accepted -----
for r in 1025 1026 1027; do
    if lj_guard_identity "$r" 2>/dev/null; then bad "guard should reject reserved id $r"; else ok "guard rejects reserved VAX id $r"; fi
done
if lj_guard_identity 1813 2>/dev/null; then ok "guard accepts a free id (1813)"; else bad "guard wrongly rejected free id 1813"; fi
if lj_guard_identity 70000 2>/dev/null; then bad "guard should reject out-of-range 70000"; else ok "guard rejects out-of-range id"; fi
if lj_guard_identity abc 2>/dev/null; then bad "guard should reject non-numeric"; else ok "guard rejects non-numeric id"; fi

# --- 2. Tap netdev args match the shipped launcher's OVMX_NET_MODE=tap contract
RUNQ="$TOOLS/../../../distro/boot/run-qemu.sh"
if [ -f "$RUNQ" ]; then
    : >"$TMP/vmlinuz"; : >"$TMP/initrd"
    LAUNCHER="$(env OVMX_QEMU_DRYRUN=1 OVMX_NET_MODE=tap OVMX_NET_TAP=tap4 OVMX_NET_MAC=52:54:00:00:00:f4 \
        bash "$RUNQ" "$TMP/vmlinuz" "$TMP/initrd" 2>/dev/null | tr '\n' ' ')"
    LIBARGS="$(lj_tap_netdev_args tap4 52:54:00:00:00:f4 | tr '\n' ' ')"
    # Both must carry the identical netdev + device tokens.
    if printf '%s' "$LAUNCHER" | grep -qF 'tap,id=net0,ifname=tap4,script=no,downscript=no' \
       && printf '%s' "$LIBARGS" | grep -qF 'tap,id=net0,ifname=tap4,script=no,downscript=no'; then
        ok "tap netdev arg matches run-qemu.sh OVMX_NET_MODE=tap"
    else
        bad "tap netdev arg drifted from run-qemu.sh: launcher='$LAUNCHER' lib='$LIBARGS'"
    fi
    if printf '%s' "$LAUNCHER" | grep -qF 'virtio-net-pci,netdev=net0,romfile=,mac=52:54:00:00:00:f4' \
       && printf '%s' "$LIBARGS" | grep -qF 'virtio-net-pci,netdev=net0,romfile=,mac=52:54:00:00:00:f4'; then
        ok "tap device arg (mac, romfile disabled) matches run-qemu.sh"
    else
        bad "tap device arg drifted from run-qemu.sh"
    fi
else
    bad "run-qemu.sh not found at $RUNQ (cannot check the tap-arg contract)"
fi

# --- 3. CN parse ------------------------------------------------------------
[ "$(printf 'junk\r\nCN_2\r\n' | lj_parse_cn)" = "2" ] && ok "lj_parse_cn reads CN_2" || bad "lj_parse_cn CN_2"
[ "$(printf 'CN_1\nCN_3\n'   | lj_parse_cn)" = "3" ] && ok "lj_parse_cn takes the last CN" || bad "lj_parse_cn last"

# --- 4. Verdict PASS on a fully-joined transcript set -----------------------
PCAP="$TMP/join.pcap"; printf 'binkstuff OVMXJ0 more 0x6007 frames' >"$PCAP"
OVMX_SC='View of Cluster from system OVMXJ0
  VAX1     MEMBER
  VAX2     MEMBER'
VAX_SC='View of Cluster from system VAX1
  VAX1   MEMBER
  VAX2   MEMBER
  OVMXJ0 MEMBER'
if lj_verdict OVMXJ0 "$OVMX_SC" "$VAX_SC" 3 "$PCAP" >"$TMP/v.out" 2>&1; then
    ok "verdict PASS when all four legs are present"
else
    bad "verdict wrongly FAILED a fully-joined transcript"; sed 's/^/    /' "$TMP/v.out"
fi

# --- 5. labjoin_booted.sh refuses a reserved identity (mock kubectl) ---------
mkkubectl() {  # writes a mock kubectl into $TMP/bin that reads $TMP/pod as the pod fs
cat >"$TMP/bin/kubectl" <<'MOCK'
#!/bin/bash
set -eu
shift; shift            # -n NS
verb="$1"; shift
case "$verb" in
  get) exit 0 ;;                                  # pod always healthy
  cp)
    src="$1"; dst="$2"
    case "$dst" in *:*) p="${dst#*:}"; mkdir -p "$(dirname "$FAKEPOD$p")"; cp "$src" "$FAKEPOD$p";;
                   *)  p="${src#*:}"; cp "$FAKEPOD$p" "$dst";; esac; exit 0 ;;
  exec)
    shift; [ "${1:-}" = "--" ] && shift
    case "${1:-}" in
      md5sum) md5sum "$FAKEPOD$2" ;;
      mkdir)  shift; mkdir -p "$FAKEPOD${!#}" 2>/dev/null || true ;;
      chmod)  : ;;
      sh)     : ;;                                # ip/link/printf-to-fifo no-ops
      timeout) : ;;                               # tcpdump no-op
      pkill)  : ;;
      *) : ;;
    esac; exit 0 ;;
  *) exit 0 ;;
esac
MOCK
chmod +x "$TMP/bin/kubectl"
}
mkdir -p "$TMP/bin" "$TMP/pod" "$TMP/art" "$TMP/hostl"
export FAKEPOD="$TMP/pod"
: >"$TMP/art/vmlinuz"; : >"$TMP/art/initramfs-ovmx-slim.cpio.gz"; : >"$TMP/art/ovmx-distrib.img"
mkkubectl
run_harness() {  # extra args
    PATH="$TMP/bin:$PATH" NS=ovmx-lab L="$TMP/podlogs" HOSTL="$TMP/hostl" W="$TMP/work" \
        bash "$TOOLS/labjoin_booted.sh" "$@" 2>&1
}

printf 'CN_2\r\n' >"$TMP/hostl/vax1.log"
OUT="$(run_harness vaxlab-0 tagRSV "$TMP/art" 60 OVMXBAD 1025)"; rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$OUT" | grep -qF 'collides with a lab-2 pod VAX id'; then
    ok "harness refuses a reserved identity (SCSSYSTEMID 1025) before touching the pod"
else
    bad "harness did not refuse reserved id 1025 (rc=$rc)"; printf '%s\n' "$OUT" | sed 's/^/    /'
fi

# --- 6. labjoin_booted.sh refuses a non-CN_2 pod ----------------------------
printf 'CN_1\r\n' >"$TMP/hostl/vax1.log"          # pod is a broken 1-node lab
OUT="$(run_harness vaxlab-0 tagCN1 "$TMP/art" 60 OVMXJ0 1813)"; rc=$?
if [ "$rc" -ne 0 ] && printf '%s' "$OUT" | grep -qF 'not a healthy 2-node cluster'; then
    ok "harness refuses to join a non-CN_2 pod (CLUSTER_NODES=1)"
else
    bad "harness did not refuse a CN_1 pod (rc=$rc)"; printf '%s\n' "$OUT" | sed 's/^/    /'
fi

echo ""
echo "  RESULTS: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ] && { echo "  LABJOIN PLUMBING: PASS"; exit 0; }
echo "  LABJOIN PLUMBING: FAIL"; exit 1
