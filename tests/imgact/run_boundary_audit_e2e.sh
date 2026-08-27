#!/bin/bash
# run_boundary_audit_e2e.sh -- executive-boundary AUDIT tracer INTEGRATION proof
# (vms-617, Phase A under vms-040). This is the imgact-path counterpart to the
# hosted unit proof (tests/boundary_audit): it drives a REAL activated OVMX
# image through the REAL IMGACT.EXE activation site, with the freestanding
# raw-clone supervisor armed by IMGACT itself when OVMX_BOUNDARY_AUDIT=1.
#
# Done-conditions proven here (design doc §Done-condition, via the ACTUAL IMGACT
# path -- not a mock of the tracer):
#   (1) POSITIVE       -- the image's raw socket + raw openat(VMS-volume path) +
#                         raw clone each surface as a finding naming the syscall
#                         and the image.
#   (3) TRANSPARENT    -- the image's observable result (stdout, exit status, the
#                         written marker) is BYTE-IDENTICAL with the tracer on vs
#                         off (SECCOMP_USER_NOTIF_FLAG_CONTINUE alters nothing).
#   (4) NEGCTL         -- the planted raw bypass makes the report NON-EMPTY; with
#                         the tracer disabled the report is EMPTY -- so the
#                         instrument cannot silently pass a bypass, and its
#                         non-emptiness in the ON case is genuinely the tracer.
#
# Rule 9 / INV-6: the tracer's own mechanism (unprivileged seccomp user-notif)
# is a real kernel capability. If the kernel refuses it, this exits 77 (ctest
# SKIP, with a reason) -- never a fake PASS -- exactly as the unit proof does.
#
# x86_64 Phase A (design §Width note): the test image's syscall stub is x86_64,
# so on any other build arch this SKIPs (a second arch is a follow-up, like the
# tracer's own NR table).
set -u

CC="${CC:-cc}"
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$SELF_DIR/../.." && pwd)"        # repo root
IMGACT_DIR="$SRC/src/imgact"
ARCHDIR="$IMGACT_DIR/arch/x86_64"

say()  { printf '%s\n' "$*"; }
fail() { printf 'FAILURE: %s\n' "$*"; exit 1; }
skip() { printf 'SKIP: %s\n' "$*"; exit 77; }

# ---- arch gate (Phase A: x86_64) -------------------------------------------
MACH="$(uname -m)"
[ "$MACH" = "x86_64" ] || skip "Phase A is x86_64-only (build arch: $MACH); a second arch is a follow-up"

TMP="$(mktemp -d "${TMPDIR:-/tmp}/ovmx_ba_e2e.XXXXXX")" || fail "mktemp"
cleanup() { rm -rf "$TMP"; }
trap cleanup EXIT

IMGACT="$TMP/IMGACT.EXE"
BYPASS="$TMP/bypass"
MARKER="$TMP/marker.out"
LOG="$TMP/findings.jsonl"

# ---- build IMGACT.EXE (with the boundary-audit TU) to a temp, off-tree ------
# Replicates src/imgact/Makefile's freestanding static-PIE recipe with absolute
# -I paths, so a shared checkout is never mutated (build artifacts stay in TMP).
IMGACT_CFLAGS="-std=gnu11 -O2 -Wall -Wextra
  -I$IMGACT_DIR/include -I$SRC/src/vmslink/include -I$SRC/src/libvms/include
  -I$SRC/src/kernel -I$SRC/src/boundary_audit/include
  -fPIC -fvisibility=hidden -ffreestanding -fno-stack-protector -fno-builtin
  -fno-asynchronous-unwind-tables"
IMGACT_LDFLAGS="-nostdlib -nostartfiles -shared -Wl,-e,_start -Wl,-z,norelro -Wl,--build-id=none"
IMGACT_SRCS="$IMGACT_DIR/imgact.c $IMGACT_DIR/known_images.c $IMGACT_DIR/imgact_acp.c
  $IMGACT_DIR/imgact_xfer.c $IMGACT_DIR/imgact_boundary_audit.c
  $SRC/src/boundary_audit/boundary_audit_filter.c $ARCHDIR/start.S"

say "== build IMGACT.EXE (x86_64, +boundary_audit) =="
# shellcheck disable=SC2086
$CC $IMGACT_CFLAGS $IMGACT_LDFLAGS -o "$IMGACT" $IMGACT_SRCS \
	|| fail "IMGACT.EXE build failed"

# ---- capability probe: unprivileged seccomp user-notif listener ------------
cat > "$TMP/probe.c" <<'EOF'
#define _GNU_SOURCE
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <unistd.h>
int main(void){
	struct sock_filter f[]={ BPF_STMT(BPF_RET|BPF_K, SECCOMP_RET_ALLOW) };
	struct sock_fprog p={ .len=1, .filter=f };
	if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)) return 2;
	long r=syscall(SYS_seccomp, SECCOMP_SET_MODE_FILTER,
		       SECCOMP_FILTER_FLAG_NEW_LISTENER, &p);
	return r<0?1:0;
}
EOF
$CC -O2 -o "$TMP/probe" "$TMP/probe.c" || fail "probe build failed"
if ! "$TMP/probe"; then
	skip "unprivileged seccomp user-notif listener refused by the kernel (needs no_new_privs + user-notif). This is the tracer's own mechanism -- not faking a pass (INV-6)."
fi

# ---- build the test image (real activation: PT_INTERP=IMGACT.EXE) ----------
say "== build test image (PT_INTERP=$IMGACT) =="
$CC -std=gnu11 -O2 -Wall -pie -fPIE -nostdlib -ffreestanding -fno-stack-protector \
	-DBA_MARKER_PATH="\"$MARKER\"" \
	-Wl,--dynamic-linker="$IMGACT" -Wl,--hash-style=sysv -Wl,-z,norelro \
	-Wl,-e,_start -o "$BYPASS" "$SELF_DIR/boundary_audit_bypass.c" \
	|| fail "test image build failed"

# run the image; args: 1=tracer-on(1/0). Captures stdout+exit+marker.
# Returns via globals RUN_OUT / RUN_RC / RUN_MARK.
run_image() {
	local on="$1"
	local outf="$TMP/out.$on"
	rm -f "$MARKER"
	if [ "$on" = "1" ]; then
		rm -f "$LOG"
		OVMX_BOUNDARY_AUDIT=1 OVMX_BOUNDARY_AUDIT_LOG="$LOG" \
			timeout 30 "$BYPASS" > "$outf" 2>&1
	else
		# tracer OFF: env unset entirely (default runtime, un-audited)
		timeout 30 "$BYPASS" > "$outf" 2>&1
	fi
	RUN_RC=$?
	RUN_OUT="$(cat "$outf" 2>/dev/null)"
	RUN_MARK="$(cat "$MARKER" 2>/dev/null)"
}

# wait until the supervisor (a reparented child) has flushed the log, or time out
wait_for_log() {
	local i=0
	while [ $i -lt 100 ]; do
		[ -s "$LOG" ] && return 0
		sleep 0.05
		i=$((i+1))
	done
	return 1
}

has_syscall() { grep -q "\"syscall\":\"$1\"" "$LOG" 2>/dev/null; }

# ---------- POSITIVE (done-condition 1) ----------
say "== [positive] raw socket/openat/clone surface through the IMGACT path =="
run_image 1
[ "$RUN_RC" -eq 0 ]  || fail "audited image did not exit 0 (rc=$RUN_RC): $RUN_OUT"
case "$RUN_OUT" in *"BOUNDARY-BYPASS: PASS"*) ;; *) fail "expected PASS line, got: $RUN_OUT";; esac
wait_for_log || fail "no findings were flushed (tracer did not install at the activation site)"
say "-- findings --"; cat "$LOG"
has_syscall socket || fail "no finding names raw socket"
has_syscall openat || fail "no finding names raw openat"
has_syscall clone  || fail "no finding names raw clone"
grep -q "\"image\":\"$BYPASS\"" "$LOG" || fail "findings do not name the activated image"
say "positive OK"

# ---------- TRANSPARENCY (done-condition 3) ----------
say "== [transparency] observable result identical with tracer on vs off =="
run_image 1; ON_OUT="$RUN_OUT"; ON_RC="$RUN_RC"; ON_MARK="$RUN_MARK"
run_image 0; OFF_OUT="$RUN_OUT"; OFF_RC="$RUN_RC"; OFF_MARK="$RUN_MARK"
[ "$ON_OUT"  = "$OFF_OUT" ]  || fail "stdout differs on/off:\n ON=[$ON_OUT]\nOFF=[$OFF_OUT]"
[ "$ON_RC"   = "$OFF_RC"  ]  || fail "exit status differs on/off: ON=$ON_RC OFF=$OFF_RC"
[ "$ON_MARK" = "$OFF_MARK" ] || fail "written marker differs on/off: ON=[$ON_MARK] OFF=[$OFF_MARK]"
[ "$ON_MARK" = "marker-ok" ] || fail "CONTINUE altered the real write (marker=[$ON_MARK])"
say "transparency OK (out/rc/marker byte-identical; marker landed)"

# ---------- NEGCTL (done-condition 4) ----------
say "== [negctl] the instrument cannot silently pass a bypass =="
# ON: the planted raw bypass MUST make the report non-empty and name the bypass.
run_image 1
wait_for_log || fail "negctl: armed tracer produced an EMPTY report for a planted bypass"
has_syscall socket || fail "negctl: planted raw socket bypass not surfaced"
# OFF: with the tracer disabled the report stays empty -- proving the ON
# non-emptiness is genuinely the tracer, and a disabled/mis-installed tracer is
# VISIBLY empty (it would fail this gate, never silently pass a bypass).
rm -f "$LOG"
run_image 0
if [ -s "$LOG" ]; then
	fail "negctl: findings log is non-empty with the tracer DISABLED (spurious)"
fi
say "negctl OK (armed => bypass surfaced; disabled => empty, cannot silently pass)"

say ""
say "ALL BOUNDARY-AUDIT IMGACT INTEGRATION CHECKS PASSED (vms-617)"
exit 0
