#!/usr/bin/env bash
#
# capture_oracle.sh (rd vms-55d) -- capture the byte-exact golden DISPLAY output
# of a NAMED SURFACE from a LIVE OpenVMS lab oracle, for the vms-050 UX-fidelity
# golden-comparison gates (feeds vms-c38 / vms-d008).
#
# WHAT IT DOES. Given a surface (a small data file naming an arch + the DCL
# commands a user would type), it drives the live lab's console, runs the
# commands between unique markers, slices the transcript byte-exactly, and writes
# a versioned golden (docs/oracle/golden/<surface>.golden) plus a provenance
# sidecar (.golden.meta). This is OBSERVATION of documented tool output on the
# real system -- the sanctioned clean-room grounding (CLAUDE.md Rule 8): never
# disassembly, never VSI source.
#
# LAB TRAPS RESPECTED (learned the hard way, vms-580 / vms-a01):
#   - base64 END-TO-END through the console FIFO; DCL is full of $ and " that
#     every shell between here and the FIFO would expand.
#   - drive through the pod's input FIFO, NEVER a second console connection:
#     AXPbox (Alpha) EXITS when a console client disconnects; a TCP readiness
#     probe powers the machine off. FIFO-only is the only safe path.
#   - the boot parks at "SYSTEM job terminated"; a bare RETURN wakes OPA0: before
#     Username: appears.
#   - login is prompt-synchronised, one line at a time (batched sends race the
#     boot chatter and fail as %LOGIN-F-*; never actually a bad password).
#   - the shared labs (vaxlab-0/1, lab-1) are single-instance / may be BUSY with
#     another investigation; this tool always scales UP its OWN isolated replica
#     (cur+1) and drives the new highest-index pod, then scales back -- it never
#     touches a pod it did not create.
#   - `strings` intermittently drops the SRM/console control stream; read with a
#     printable-char filter (tr -cd) instead.
#
# USAGE:
#   tools/oracle/capture_oracle.sh <surface>            # capture -> golden
#   tools/oracle/capture_oracle.sh <surface> --keep-lab # leave the replica up
#   tools/oracle/capture_oracle.sh list                 # list known surfaces
#   tools/oracle/capture_oracle.sh selftest             # slice logic, no lab
#
# EXIT: 0 iff a non-empty golden was captured (or selftest passed); nonzero else.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"                 # tools/oracle
REPO="$(cd "$HERE/../.." && pwd)"
SURF_DIR="$HERE/surfaces"
GOLDEN_DIR="$REPO/docs/oracle/golden"
NS="${OVMX_LAB_NS:-ovmx-lab}"

BEGIN_MARK='%%OVMX-ORACLE-BEGIN%%'
END_MARK='%%OVMX-ORACLE-END%%'

die() { echo "capture_oracle: $*" >&2; exit 1; }
log() { echo "[capture_oracle] $*" >&2; }

# ---------------------------------------------------------------------------
# extract_golden -- THE TEETH. Pure function over a raw console transcript:
# slice strictly between the standalone BEGIN/END marker OUTPUT lines (which
# start with %%; the command ECHOES start with "$ " so they never match), then
# drop any residual marker-WRITE echo. stdin -> stdout. Shared verbatim by the
# live capture and by `selftest`, so what the gate trusts is what the test
# proves.
# ---------------------------------------------------------------------------
extract_golden() {
    # DELETE CRs (the console emits CR-LF; converting CR->LF would double every
    # line into blanks). Keep printable bytes + LF so column spacing is byte-exact.
    tr -cd '[:print:]\n\r' | tr -d '\r' \
      | awk -v b="$BEGIN_MARK" -v e="$END_MARK" '
            index($0,b)==1 { cap=1; next }
            index($0,e)==1 { cap=0 }
            cap { print }' \
      | grep -v 'OVMX-ORACLE-' || true
}

# ---------------------------------------------------------------------------
# apply_normalize -- stdin -> stdout, applying the surface's NORMALIZE sed
# program (identity when unset, so byte-exact surfaces are unchanged). This is
# the SYMMETRIC mask: capture applies it to the oracle display, and the vms-c38
# golden-comparison gate applies the IDENTICAL mask to OVMX's own output before
# diffing (via `capture_oracle.sh normalize <surface> < ovmx_output`), so a
# volatile field (a free-page count, a timestamp, a PID) can never red a
# LAYOUT-fidelity diff -- only a real structural divergence (a missing section,
# a renamed column, a shifted width) does. A deterministic surface sets no
# NORMALIZE and stays byte-exact.
# ---------------------------------------------------------------------------
apply_normalize() {
    if [ -n "${NORMALIZE:-}" ]; then sed "$NORMALIZE"; else cat; fi
}

selftest() {
    echo "=== capture_oracle selftest: extract_golden() slices byte-exactly ==="
    local fails=0 tmp got want
    tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' RETURN

    # A synthetic console transcript: boot chatter, the marker frame, two
    # commands with their echoes + output, control bytes sprinkled in.
    printf '%s\n' \
      'boot chatter line' \
      '$ WRITE SYS$OUTPUT "%%OVMX-ORACLE-BEGIN%%"' \
      '%%OVMX-ORACLE-BEGIN%%' \
      '$ SHOW SYMBOL X' \
      '  X = 42   Hex = 0000002A  Octal = 00000000052' \
      '$ WRITE SYS$OUTPUT "%%OVMX-ORACLE-END%%"' \
      '%%OVMX-ORACLE-END%%' \
      '$ ' > "$tmp/in.log"

    got="$(extract_golden < "$tmp/in.log")"
    want='$ SHOW SYMBOL X
  X = 42   Hex = 0000002A  Octal = 00000000052'
    if [ "$got" = "$want" ]; then echo "  PASS: clean frame sliced exactly"; else
        echo "  FAIL: clean frame"; printf 'got=[%s]\nwant=[%s]\n' "$got" "$want"; fails=$((fails+1)); fi

    # Negative: no markers -> empty (must not leak boot chatter).
    printf '%s\n' 'boot chatter' '$ SHOW TIME' '  1-JAN' > "$tmp/nomark.log"
    got="$(extract_golden < "$tmp/nomark.log")"
    if [ -z "$got" ]; then echo "  PASS: no markers -> empty (no chatter leak)"; else
        echo "  FAIL: leaked without markers: [$got]"; fails=$((fails+1)); fi

    # Column bytes preserved exactly (leading spaces are load-bearing).
    printf '%s\n' "$BEGIN_MARK" '    two   leading   groups' "$END_MARK" > "$tmp/cols.log"
    got="$(extract_golden < "$tmp/cols.log")"
    if [ "$got" = '    two   leading   groups' ]; then echo "  PASS: exact column spacing preserved"; else
        echo "  FAIL: spacing mangled: [$got]"; fails=$((fails+1)); fi

    # CR-LF line endings (the real console) must NOT double into blank lines.
    printf '%s\r\n' "$BEGIN_MARK" '$ SHOW X' '  X = 1' "$END_MARK" > "$tmp/crlf.log"
    got="$(extract_golden < "$tmp/crlf.log")"
    want='$ SHOW X
  X = 1'
    if [ "$got" = "$want" ]; then echo "  PASS: CR-LF endings do not insert blank lines"; else
        echo "  FAIL: CR-LF doubled: [$got]"; fails=$((fails+1)); fi

    # NORMALIZE digit-mask: masks every value BUT preserves column alignment
    # (same width), so two captures with different numbers normalize identically.
    local memrow='  Main Memory (128.00Mb)          262144      217508'
    got="$(NORMALIZE='s/[0-9]/#/g'; printf '%s\n' "$memrow" | apply_normalize)"
    want='  Main Memory (###.##Mb)          ######      ######'
    if [ "$got" = "$want" ]; then echo "  PASS: digit-mask masks values, preserves alignment"; else
        echo "  FAIL: digit-mask: [$got]"; fails=$((fails+1)); fi
    # Two different-number rows normalize to the SAME masked form (reproducible).
    local a b
    a="$(NORMALIZE='s/[0-9]/#/g'; printf '%s\n' '  Free  217508  Modified  2646' | apply_normalize)"
    b="$(NORMALIZE='s/[0-9]/#/g'; printf '%s\n' '  Free  191204  Modified  3901' | apply_normalize)"
    if [ "$a" = "$b" ]; then echo "  PASS: differing volatile fields normalize identically"; else
        echo "  FAIL: not reproducible under mask: [$a] vs [$b]"; fails=$((fails+1)); fi
    # Identity when NORMALIZE is unset (byte-exact surfaces unchanged).
    got="$(NORMALIZE=''; printf '%s\n' "$memrow" | apply_normalize)"
    if [ "$got" = "$memrow" ]; then echo "  PASS: no NORMALIZE -> byte-exact passthrough"; else
        echo "  FAIL: identity broken: [$got]"; fails=$((fails+1)); fi

    echo "=== $( [ $fails -eq 0 ] && echo 'selftest OK' || echo "selftest FAILED ($fails)" ) ==="
    return $fails
}

# ---------------------------------------------------------------------------
# lab plumbing (per arch)
# ---------------------------------------------------------------------------
# echoes: STS NODE USER PASS
lab_for_arch() {
    case "$1" in
        vax)   echo "vaxlab vax1 SYSTEM system" ;;
        alpha) echo "alphalab alpha1 SYSTEM ovmxlab2026" ;;
        *) die "unknown arch '$1' (use: vax | alpha)" ;;
    esac
}

kexec() { kubectl -n "$NS" exec "$1" -- sh -c "$2"; }

fifo_send() {   # pod node line
    local b; b="$(printf '%s' "$3" | base64 -w0)"
    kexec "$1" "{ echo $b | base64 -d; echo; } > /lab/k8s-labs/$1/logs/$2.log.in"
}
console_tail() { # pod node n
    kexec "$1" "cat /lab/k8s-labs/$1/logs/$2.log 2>/dev/null | tr -cd '[:print:]\n\r' | tr '\r' '\n' | tail -${3:-6}" 2>/dev/null || true
}

# scale_up_isolated <sts> -- scales the sts by +1 and sets the GLOBALS
# _PREV_REPLICAS (for scale-back) and POD (the freshly-created highest-index,
# 0-indexed, pod). Deliberately NOT run in a $() subshell -- the globals must
# survive into the caller so the cleanup trap can scale back.
scale_up_isolated() {
    local sts="$1"
    _PREV_REPLICAS="$(kubectl -n "$NS" get sts "$sts" -o jsonpath='{.spec.replicas}' 2>/dev/null)" \
        || die "no statefulset $sts in ns $NS"
    [ -n "$_PREV_REPLICAS" ] || die "could not read $sts replica count"
    POD="${sts}-${_PREV_REPLICAS}"   # 0-indexed: the new highest pod
    kubectl -n "$NS" scale "sts/$sts" --replicas=$((_PREV_REPLICAS + 1)) >/dev/null
}

wait_boot() {  # pod node  (up to ~7 min)
    # Read a WIDE tail: the console parks at the SYSTEM-startup accounting block,
    # whose "SYSTEM ... job terminated" marker sits ~6 lines above the block's
    # last line -- a tail -3 scrolls it off and never matches (the bug that made
    # a perfectly-booted node look like a timeout). tail -15 catches it, and the
    # "Username:" case (already woken) is the last line either way.
    local pod="$1" node="$2" i t
    for i in $(seq 1 42); do
        t="$(console_tail "$pod" "$node" 15)"
        if printf '%s' "$t" | grep -qE 'Username:|job terminated'; then return 0; fi
        if printf '%s' "$t" | grep -qE 'Bugcheck|P00>>>'; then die "node $node unhealthy (halted at SRM/bugcheck)"; fi
        sleep 10
    done
    die "node $node did not reach login within timeout"
}

wait_pat() {  # pod node pattern [maxsecs]  -- poll the console tail for a regex
    local pod="$1" node="$2" pat="$3" max="${4:-40}" i
    for ((i = 0; i < max; i += 2)); do
        console_tail "$pod" "$node" 8 | grep -qE "$pat" && return 0
        sleep 2
    done
    return 1
}

login() {  # pod node user pass -- prompt-synchronised, one line at a time
    local pod="$1" node="$2" user="$3" pass="$4"
    fifo_send "$pod" "$node" ''            # bare RETURN wakes OPA0:
    if ! wait_pat "$pod" "$node" 'Username:' 30; then
        fifo_send "$pod" "$node" ''        # one more nudge
        wait_pat "$pod" "$node" 'Username:' 30 || die "no Username: prompt after wake"
    fi
    fifo_send "$pod" "$node" "$user"
    wait_pat "$pod" "$node" 'Password:' 20 || die "no Password: prompt after username"
    fifo_send "$pod" "$node" "$pass"
    # A good login shows the Welcome banner; a bad one shows %LOGIN-F-* (never
    # actually a bad password -- a send raced the boot chatter).
    wait_pat "$pod" "$node" 'Welcome to OpenVMS' 45 \
        || die "login did not complete (no Welcome banner within timeout)"
    # The banner is still printing; send a RETURN and wait for the DCL prompt
    # line ("$ ") to SETTLE before anyone types a command into a half-ready CLI.
    fifo_send "$pod" "$node" ''
    wait_pat "$pod" "$node" '\$ *$' 20 || die "no settled DCL prompt after login banner"
    fifo_send "$pod" "$node" 'SET TERMINAL/PAGE=0/WIDTH=132/NOBROADCAST'
    wait_pat "$pod" "$node" '\$ *$' 15 || true
}

# ---------------------------------------------------------------------------
# main
# ---------------------------------------------------------------------------
[ $# -ge 1 ] || { grep -E '^#( |$)' "$0" | sed 's/^# \{0,1\}//'; exit 2; }

case "$1" in
  selftest) selftest; exit $? ;;
  list) ls "$SURF_DIR"/*.surface 2>/dev/null | xargs -n1 basename 2>/dev/null | sed 's/\.surface$//' || echo "(no surfaces)"; exit 0 ;;
  normalize)
    # capture_oracle.sh normalize <surface> < input  -- apply the surface's mask
    # to stdin (what the vms-c38 gate runs on OVMX output, symmetric with the
    # oracle capture). Prints stdin unchanged for a byte-exact surface.
    [ $# -ge 2 ] || die "usage: capture_oracle.sh normalize <surface> < input"
    nsurf="$SURF_DIR/$2.surface"
    [ -f "$nsurf" ] || die "no surface '$2' ($nsurf)"
    NORMALIZE=""
    # shellcheck disable=SC1090
    . "$nsurf"
    apply_normalize
    exit $? ;;
esac

SURFACE="$1"; KEEP_LAB=0; [ "${2:-}" = "--keep-lab" ] && KEEP_LAB=1
SURF_FILE="$SURF_DIR/$SURFACE.surface"
[ -f "$SURF_FILE" ] || die "no surface '$SURFACE' ($SURF_FILE). Try: capture_oracle.sh list"

# surface file: ARCH=, DESC=, COMMANDS=( 'DCL 1' ... ), optional NORMALIZE=sed
ARCH=""; DESC=""; NORMALIZE=""; COMMANDS=()
# shellcheck disable=SC1090
. "$SURF_FILE"
[ -n "$ARCH" ] && [ "${#COMMANDS[@]}" -gt 0 ] || die "surface '$SURFACE' must set ARCH and COMMANDS"

read -r STS NODE USER PASS <<<"$(lab_for_arch "$ARCH")"
log "surface=$SURFACE arch=$ARCH sts=$STS node=$NODE cmds=${#COMMANDS[@]}"

_PREV_REPLICAS=""; POD=""
scale_up_isolated "$STS"
cleanup() {
    if [ "$KEEP_LAB" -eq 0 ] && [ -n "$_PREV_REPLICAS" ]; then
        log "scaling $STS back to $_PREV_REPLICAS"
        kubectl -n "$NS" scale "sts/$STS" --replicas="$_PREV_REPLICAS" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT
log "using isolated replica $POD (was $_PREV_REPLICAS)"

# wait Running, then boot
for i in $(seq 1 30); do
    kubectl -n "$NS" get pod "$POD" 2>/dev/null | grep -q 'Running' && break; sleep 4
done
wait_boot "$POD" "$NODE"
login "$POD" "$NODE" "$USER" "$PASS"

# run the surface between markers
fifo_send "$POD" "$NODE" "WRITE SYS\$OUTPUT \"$BEGIN_MARK\""; sleep 2
for c in "${COMMANDS[@]}"; do fifo_send "$POD" "$NODE" "$c"; sleep 3; done
fifo_send "$POD" "$NODE" "WRITE SYS\$OUTPUT \"$END_MARK\""; sleep 3

# Pipe the raw console straight into extract_golden -- extract_golden's tr -cd
# drops the console's control/NUL bytes, so GOLD is clean text (capturing the
# raw log into a $() variable first would strip NULs with a noisy warning).
GOLD="$(kexec "$POD" "cat /lab/k8s-labs/$POD/logs/$NODE.log" 2>/dev/null | extract_golden | apply_normalize || true)"
[ -n "$GOLD" ] || die "empty capture -- markers not found in the console log (boot/login race?)"

mkdir -p "$GOLDEN_DIR"
OUT="$GOLDEN_DIR/$SURFACE.golden"
printf '%s\n' "$GOLD" > "$OUT"
{
  echo "surface: $SURFACE"
  echo "arch: $ARCH"
  echo "description: ${DESC:-}"
  echo "oracle: OpenVMS $( [ "$ARCH" = vax ] && echo 'VAX V7.3' || echo 'Alpha V8.4' ) (lab $STS/$NODE, isolated replica $POD)"
  echo "captured: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
  echo "method: capture_oracle.sh -- FIFO console drive, marker-sliced display (CLAUDE.md Rule 8 observation)"
  if [ -n "$NORMALIZE" ]; then
    echo "normalize: '$NORMALIZE'   # SYMMETRIC mask -- the vms-c38 gate applies the IDENTICAL"
    echo "                          # mask to OVMX output: capture_oracle.sh normalize $SURFACE < ovmx_output"
  else
    echo "normalize: (none -- byte-exact)"
  fi
  echo "commands:"; for c in "${COMMANDS[@]}"; do echo "  - $c"; done
} > "$OUT.meta"

log "wrote $(wc -l <"$OUT") line golden -> ${OUT#$REPO/}"
echo "$OUT"
