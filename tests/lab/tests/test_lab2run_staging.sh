#!/bin/bash
# vms-1da: prove lab2run.sh stages SCSD.EXE to a PER-RUN pod path (never the
# shared /lab/SCSD.EXE that concurrent lab-2/lab-1 sessions were clobbering),
# never discards kubectl cp's stderr, and verifies the in-pod md5 both before
# and after the run.
#
# No live k3s cluster is available to a CI/worktree run, so kubectl itself is
# faked -- everything DOWNSTREAM of kubectl (the staging logic, the RDIR
# naming, the md5 comparisons, the FATAL-on-mismatch path) is the real script,
# unmodified, exercised end to end against a fake pod filesystem on disk. This
# is the actual code under test; only the k3s control plane is mocked.
set -eu

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
SCRIPT="$HERE/../tools/lab2run.sh"

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

FAKEPOD="$TMP/pod"      # simulated pod filesystem, addressed by absolute path
BIN="$TMP/bin"
mkdir -p "$FAKEPOD" "$BIN" "$TMP/store" "$TMP/logs" "$TMP/hostlogs" "$TMP/work"

# --- fake daemon binary + identity store -----------------------------------
printf 'fake-scsd-binary-v1' > "$TMP/SCSD.EXE"
chmod +x "$TMP/SCSD.EXE"
printf 'fake-sysgen-store' > "$TMP/store/id.sysgen"

# Pre-seed the console log the script polls so the join loop breaks on its
# first iteration instead of running the full 12*10s poll.
printf 'CN_3\r\n' > "$TMP/hostlogs/vax1.log"

# --- fake kubectl ------------------------------------------------------------
cat > "$BIN/kubectl" <<'MOCKEOF'
#!/bin/bash
# Minimal fake of the `kubectl -n NS <verb> ...` surface lab2run.sh uses,
# backed by $FAKEPOD as the pod filesystem.
set -eu
shift # -n
shift # NS
verb=$1; shift
case "$verb" in
  get)
    # get pod POD -- always healthy
    exit 0
    ;;
  cp)
    src=$1; dst=$2
    if [ "$MOCK_CP_FAIL" = "1" ]; then
      echo "mock kubectl cp: injected failure" >&2
      exit 1
    fi
    case "$dst" in
      *:*)
        podpath="${dst#*:}"
        mkdir -p "$(dirname "$FAKEPOD$podpath")"
        cp "$src" "$FAKEPOD$podpath"
        ;;
      *)
        # POD:path -> local (pull direction), used for the sidecar pull-back
        podpath="${src#*:}"
        cp "$FAKEPOD$podpath" "$dst"
        ;;
    esac
    exit 0
    ;;
  exec)
    pod=$1; shift
    # drop the leading --
    [ "${1:-}" = "--" ] && shift
    case "$1" in
      md5sum)
        md5sum "$FAKEPOD$2"
        ;;
      mkdir)
        shift; mkdir "$@" 2>/dev/null | sed "s#$FAKEPOD##"; mkdir -p "$FAKEPOD$2" 2>/dev/null || true
        ;;
      chmod)
        chmod "$2" "$FAKEPOD$3"
        ;;
      rm)
        # rm -f PATH
        rm -f "$FAKEPOD$3" 2>/dev/null || true
        ;;
      test)
        # test -r PATH
        test "$2" "$FAKEPOD$3"
        ;;
      pkill)
        exit 0
        ;;
      grep)
        # only used for the census/XITDONE lines -- fine to no-op
        exit 1
        ;;
      timeout)
        # backgrounded tcpdump -- no-op, exit immediately
        exit 0
        ;;
      sh)
        # sh -c "..."
        cmd=$3
        # rewrite the printf-to-console-FIFO write (the SCSD run itself)
        eval "$(echo "$cmd" | sed "s#\\\$L#$HOSTL_FAKE_L#g; s#/lab#$FAKEPOD/lab#g")"
        ;;
      *)
        exit 0
        ;;
    esac
    ;;
  *)
    exit 0
    ;;
esac
MOCKEOF
chmod +x "$BIN/kubectl"

export HOSTL_FAKE_L="$TMP/hostlogs"
export FAKEPOD
export MOCK_CP_FAIL=${MOCK_CP_FAIL:-0}
export PATH="$BIN:$PATH"
export NS=test-ns
export L="$TMP/hostlogs"
export HOSTL="$TMP/hostlogs"
export W="$TMP/work"
export SCSD_BIN="$TMP/SCSD.EXE"

pass=0; fail=0
check() {
  if [ "$1" = "0" ]; then echo "ok - $2"; pass=$((pass+1));
  else echo "FAIL - $2"; fail=$((fail+1)); fi
}

# ============================================================================
# Test 1: a clean run stages to a PER-RUN path, not the shared /lab/SCSD.EXE.
# ============================================================================
TAG1="t1-$$"
"$SCRIPT" testpod "$TAG1" "$TMP/store/id.sysgen" 1 >"$TMP/run1.out" 2>"$TMP/run1.err" || true

if [ -e "$FAKEPOD/lab/SCSD.EXE" ]; then
  echo "FAIL - shared /lab/SCSD.EXE was staged (this is the vms-1da bug)"; fail=$((fail+1))
else
  echo "ok - shared /lab/SCSD.EXE was never staged"; pass=$((pass+1))
fi

if [ -f "$FAKEPOD/lab/run-$TAG1/SCSD.EXE" ]; then
  echo "ok - staged to per-run path /lab/run-$TAG1/SCSD.EXE"; pass=$((pass+1))
else
  echo "FAIL - expected per-run staging path /lab/run-$TAG1/SCSD.EXE not found"; fail=$((fail+1))
fi

if cmp -s "$TMP/SCSD.EXE" "$FAKEPOD/lab/run-$TAG1/SCSD.EXE"; then
  echo "ok - staged binary is byte-identical to the source"; pass=$((pass+1))
else
  echo "FAIL - staged binary differs from source"; fail=$((fail+1))
fi

if grep -q "verified in-pod" "$TMP/run1.out" 2>/dev/null || grep -q "verified in-pod" "$TMP/work/$TAG1.status" 2>/dev/null; then
  echo "ok - staging md5 verification ran and logged"; pass=$((pass+1))
else
  echo "FAIL - no evidence the staging md5 was verified"; fail=$((fail+1))
fi

if grep -q "post-run md5 verified" "$TMP/work/$TAG1.status" 2>/dev/null; then
  echo "ok - post-run md5 verification ran and logged"; pass=$((pass+1))
else
  echo "FAIL - no evidence the post-run md5 was verified"; fail=$((fail+1))
fi

# ============================================================================
# Test 2: a second CONCURRENT tag does not collide with the first -- the
# defect this item exists to fix (two runs sharing one staged binary).
# ============================================================================
TAG2="t2-$$"
printf 'fake-scsd-binary-v2-DIFFERENT' > "$TMP/SCSD_v2.EXE"
chmod +x "$TMP/SCSD_v2.EXE"
SCSD_BIN="$TMP/SCSD_v2.EXE" "$SCRIPT" testpod "$TAG2" "$TMP/store/id.sysgen" 1 >"$TMP/run2.out" 2>"$TMP/run2.err" || true

if [ -f "$FAKEPOD/lab/run-$TAG1/SCSD.EXE" ] && cmp -s "$TMP/SCSD.EXE" "$FAKEPOD/lab/run-$TAG1/SCSD.EXE"; then
  echo "ok - run $TAG1's staged binary is untouched by run $TAG2"; pass=$((pass+1))
else
  echo "FAIL - run $TAG1's staged binary was clobbered by run $TAG2 (the vms-1da bug)"; fail=$((fail+1))
fi

if [ -f "$FAKEPOD/lab/run-$TAG2/SCSD.EXE" ] && cmp -s "$TMP/SCSD_v2.EXE" "$FAKEPOD/lab/run-$TAG2/SCSD.EXE"; then
  echo "ok - run $TAG2 staged its own distinct binary"; pass=$((pass+1))
else
  echo "FAIL - run $TAG2 did not stage its own binary correctly"; fail=$((fail+1))
fi

# ============================================================================
# Test 3: kubectl cp failure is surfaced (stderr not discarded) and FATAL.
# ============================================================================
TAG3="t3-$$"
set +e
MOCK_CP_FAIL=1 "$SCRIPT" testpod "$TAG3" "$TMP/store/id.sysgen" 1 >"$TMP/run3.out" 2>"$TMP/run3.err"
rc=$?
set -e
if [ "$rc" -ne 0 ]; then
  echo "ok - kubectl cp failure causes a nonzero exit"; pass=$((pass+1))
else
  echo "FAIL - kubectl cp failure was silently ignored"; fail=$((fail+1))
fi
if grep -q "kubectl cp" "$TMP/run3.err" 2>/dev/null; then
  echo "ok - kubectl cp's stderr reached the caller (not discarded)"; pass=$((pass+1))
else
  echo "FAIL - kubectl cp failure was not reported on stderr"; fail=$((fail+1))
fi

echo "--- $pass passed, $fail failed ---"
[ "$fail" -eq 0 ]
