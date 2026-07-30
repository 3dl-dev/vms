#!/bin/sh
#
# test_runtime_target_negctl.sh - negative controls for the Rule 9 gate.
#
# WHY THIS EXISTS. test_runtime_target.sh is a lint. Twice now it has printed
# "OK: executive_attach() ... halts on failure" / "OK: no per-call
# executive-presence test" over code that did neither, because each of its
# assertions was satisfiable by something other than the behaviour under test:
#   round 1 -- token presence anywhere in the file;
#   round 2 -- a halt token anywhere in the FUNCTION, satisfied by the
#              unrelated module-load branch, so `fprintf("...continuing
#              without executive..."); return;` passed;
#   round 2 -- an operator class that could not match `>=` at all, so
#              `return rc >= 0 ? 0 : -1;` -- the exact body Rule 9 deleted --
#              passed;
#   round 3 -- (c)/(d) were a TOKEN-ORDER check, so a halt with a reachable
#              path out of the branch passed (`if (getenv(...) == NULL)
#              ovmx_exec_halt(...); fprintf("continuing without executive");`),
#              as did a halt renamed to a function that does not halt;
#   round 3 -- 3c's assignment exclusion dropped the whole LINE, so every
#              evasion written on ONE line was invisible.
# A gate nobody has tried to evade is an assertion about nothing. So the
# evasions are checked in, and they run in CI.
#
# THE RULE THIS FILE ENFORCES ON ITSELF: every property gets its own MINIMAL
# mutation that trips THAT property AND NO OTHER. Round 2's "proof" deleted the
# whole function body, tripping every property at once, which is why the
# individually-weak property was never exposed. Each case below therefore
# asserts both a required substring (the property fired) and a set of forbidden
# substrings (no other property fired), so a mutation that goes red for the
# wrong reason fails this test.
#
# Usage: test_runtime_target_negctl.sh [SRC_ROOT]

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
GATE="$SRC_ROOT/tests/integration/test_runtime_target.sh"
status=0
passed=0
failed=0

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "Rule 9 gate negative controls: every property must have an evasion that trips it"

# ---------------------------------------------------------------------------
# A sandbox copy of exactly what the gate reads. Mutations never touch the
# real tree.
# ---------------------------------------------------------------------------
ROOT="$WORK/tree"
mkdir -p "$ROOT/tests/integration"
cp -a "$SRC_ROOT/src" "$ROOT/src"
cp -a "$SRC_ROOT/CLAUDE.md" "$ROOT/CLAUDE.md"
[ -f "$SRC_ROOT/Dockerfile" ] && cp -a "$SRC_ROOT/Dockerfile" "$ROOT/Dockerfile"
[ -f "$SRC_ROOT/docker-compose.yml" ] && cp -a "$SRC_ROOT/docker-compose.yml" "$ROOT/docker-compose.yml"

INIT_C="$ROOT/src/ovmx_init/ovmx_init.c"
# Check 3c's fixture must be whatever function actually binds a caller to the
# executive. That used to be sys_lock.c's bind_to_executive(); vms-9fc DELETED
# it, because binding per facility was how four separate callers each forgot to
# register. The sequence now lives once, in vms_kif.c's kif_bind(), and this
# fixture MUST follow it: an injection anchored to a function that no longer
# exists is a silent no-op -- the tree stays unmutated, the gate correctly goes
# green, and every 3c control then reports "the evasion was CERTIFIED, not
# caught". That is a broken control, not a broken gate.
KIF_C="$ROOT/src/libvmssys/vms_kif.c"
INIT_ORIG="$WORK/ovmx_init.c.orig"
KIF_ORIG="$WORK/vms_kif.c.orig"
cp "$INIT_C" "$INIT_ORIG"
cp "$KIF_C" "$KIF_ORIG"

restore() {
    cp "$INIT_ORIG" "$INIT_C"
    cp "$KIF_ORIG" "$KIF_C"
}

# ---------------------------------------------------------------------------
# POSITIVE CONTROL. The unmutated sandbox must PASS. Without this, every
# negative control below could be going red because the sandbox itself is
# broken, and would prove nothing about the mutation.
# ---------------------------------------------------------------------------
out=$(sh "$GATE" "$ROOT" 2>&1)
if [ $? -eq 0 ]; then
    echo "  PASS: positive control - unmutated sandbox tree passes the gate"
    passed=$((passed + 1))
else
    echo "  FAIL: positive control - unmutated sandbox tree FAILS the gate, so no"
    echo "        negative control below can attribute a RED to its mutation"
    echo "$out" | sed 's/^/          /'
    failed=$((failed + 1))
    status=1
fi

# ---------------------------------------------------------------------------
# expect_red <name> <required-substring> [forbidden-substring ...]
#
# Runs the gate on the mutated sandbox and requires:
#   - a nonzero exit (the gate went RED at all);
#   - the required substring (the RIGHT property fired);
#   - none of the forbidden substrings (NO OTHER property fired, i.e. the
#     mutation is minimal and this property is independently load-bearing).
# ---------------------------------------------------------------------------
expect_red() {
    name="$1"; need="$2"; shift 2
    out=$(sh "$GATE" "$ROOT" 2>&1)
    rc=$?
    ok=1
    why=""

    if [ "$rc" -eq 0 ]; then
        ok=0
        why="$why
        gate exited 0 -- the evasion was CERTIFIED, not caught"
    fi
    if ! printf '%s\n' "$out" | grep -qF "$need"; then
        ok=0
        why="$why
        expected reason not reported: $need"
    fi
    for bad in "$@"; do
        if printf '%s\n' "$out" | grep -qF "$bad"; then
            ok=0
            why="$why
        NOT minimal -- also tripped an unrelated property: $bad"
        fi
    done

    if [ "$ok" -eq 1 ]; then
        echo "  PASS: $name"
        passed=$((passed + 1))
    else
        echo "  FAIL: $name$why"
        echo "$out" | sed 's/^/          | /'
        failed=$((failed + 1))
        status=1
    fi
    restore
}

# Reason fragments, one per property of check 3b. Used both as the required
# string for that property's own control and as the forbidden string for every
# other property's control.
R_CAPTURE='no open("/dev/vms") result is captured into a variable'
R_BRANCH='has no `if (executive_fd < 0)` failure branch'
R_HALT='does not halt: no halt/exit/reboot call'
R_ESCAPE='returns to the caller before it halts'
R_CLOSE='is closed inside executive_attach() -- not pinned'
R_TERMINAL='does not END in an unconditional halt'
R_NOTERM='terminates the system: its call chain does not bottom out'
R_SHADOW='a halt entry point is REDEFINED by a macro'
R_PROBE='code branches on whether the executive could be opened'

# ===========================================================================
# CHECK 3b -- the executive is integral. One control per property.
# ===========================================================================

# (a) descriptor not captured. Everything else about the function is untouched.
sed -i 's#^    executive_fd = open("/dev/vms", O_RDWR | O_CLOEXEC);$#    (void)open("/dev/vms", O_RDWR | O_CLOEXEC);#' "$INIT_C"
expect_red "3b(a) open() result discarded instead of captured" \
    "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_TERMINAL" "$R_NOTERM" "$R_SHADOW"

# (b) captured, halted, pinned -- but the test is not a failure test. The halt
# is left exactly where it is, so only the branch property can fire.
sed -i 's#^    if (executive_fd < 0) {$#    if (executive_fd > 1000000) {#' "$INIT_C"
expect_red "3b(b) failure test replaced by a condition that is never true" \
    "$R_BRANCH" "$R_CAPTURE" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_TERMINAL" "$R_NOTERM" "$R_SHADOW"

# (c) THE ADVERSARY'S ONE-LINE REGRESSION (vms-a35 round 2). The halt inside the
# open-failure branch becomes warn-and-continue. The module-load branch above
# still contains a halt -- which is precisely what made round 2's whole-function
# token search certify this. Behaviourally: PID 1 boots to `Username:` with no
# executive (test_executive_integral.sh Boot C: 10 passed / 4 FAILED).
sed -i 's#^        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));$#        fprintf(stderr, "%%OVMX-W-EXEC, continuing without executive: %s\\n", strerror(errno));\n        return;#' "$INIT_C"
expect_red "3b(c) halt replaced by warn-and-continue (round-2 escape)" \
    "$R_HALT" "$R_CAPTURE" "$R_BRANCH" "$R_ESCAPE" "$R_CLOSE" "$R_TERMINAL" "$R_NOTERM" "$R_SHADOW"

# (d) the halt is still there, but a return reaches the caller first, so the
# halt is dead code. Distinct property from (c): the halt IS in the branch.
sed -i 's#^        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));$#        return;\n        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));#' "$INIT_C"
expect_red "3b(d) return before the halt makes the halt unreachable" \
    "$R_ESCAPE" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_CLOSE" "$R_TERMINAL" "$R_NOTERM" "$R_SHADOW"

# (e) probe-and-release: the open succeeds, halts on failure, and then hands the
# descriptor back, so vms.ko is no longer pinned and rmmod succeeds mid-life.
sed -i 's#^    printf("%%OVMX-I-EXEC, VMS executive attached on /dev/vms\\n");$#    close(executive_fd);\n    printf("%%OVMX-I-EXEC, VMS executive attached on /dev/vms\\n");#' "$INIT_C"
expect_red "3b(e) descriptor closed again -- probe-and-release, not a pin" \
    "$R_CLOSE" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_TERMINAL" "$R_NOTERM" "$R_SHADOW"

# ---------------------------------------------------------------------------
# (f) TERMINALITY. Four shapes, all of which passed rounds 1-3 because (c)/(d)
# only asked "is there a halt token, and does a return come before it?".
# Each keeps a halt token in the branch and keeps it FIRST, so (c) and (d) are
# both satisfied -- only terminality is violated. That is what makes them
# minimal controls for this property and no other.
# ---------------------------------------------------------------------------

# (f-i) THE ENV-VAR ESCAPE HATCH. Rule 9's warn-and-continue and Rule 10's
# env-var bridge in a single hunk: the halt fires only when the variable is
# unset, and the branch then falls out and prints the (now false) success line.
sed -i 's#^        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));$#        if (getenv("OVMX_ALLOW_NO_EXEC") == NULL)\n            ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));\n        fprintf(stderr, "%%OVMX-W-EXEC, continuing without executive\\n");#' "$INIT_C"
expect_red "3b(f-i) halt behind an env-var escape hatch, then falls through" \
    "$R_TERMINAL" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_NOTERM" "$R_SHADOW"

# (f-ii) CONDITIONAL HALT. One errno survives. Nothing else changes.
sed -i 's#^        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));$#        if (errno != ENODEV)\n            ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));#' "$INIT_C"
expect_red "3b(f-ii) halt guarded by an errno test -- falls through on one errno" \
    "$R_TERMINAL" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_NOTERM" "$R_SHADOW"

# (f-iii) NON-HALTING HALT. The old regex matched ANY identifier containing
# "halt" and never checked which function was called.
sed -i 's#^        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));$#        ovmx_exec_halt_reason("VMS executive device /dev/vms did not open", strerror(errno));#' "$INIT_C"
expect_red "3b(f-iii) call renamed to an unrecognised halt-like function" \
    "$R_TERMINAL" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_NOTERM" "$R_SHADOW"

# (f-iv) the halt is unconditional and first, but is no longer LAST -- the
# branch keeps going after it. This is the purest statement of the property.
sed -i 's#^        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));$#        ovmx_exec_halt("VMS executive device /dev/vms did not open", strerror(errno));\n        fprintf(stderr, "%%OVMX-W-EXEC, carrying on\\n");#' "$INIT_C"
expect_red "3b(f-iv) statement after the halt -- branch is not terminal" \
    "$R_TERMINAL" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_NOTERM" "$R_SHADOW"

# ---------------------------------------------------------------------------
# (g) THE HALT MUST REALLY HALT. executive_attach() is untouched -- it still
# calls ovmx_exec_halt() unconditionally as its last statement. Only
# ovmx_exec_halt()'s own body is gutted, so it prints and returns. Pinning the
# callee by NAME alone would certify this; the gate re-derives from the source
# that the call chain bottoms out in _exit/reboot/abort.
# execinit_halt() is defined ABOVE ovmx_exec_halt(), so its halt_now() is not
# touched -- keeping the mutation to a single function.
awk '
    /^static void ovmx_exec_halt\(/ { inf = 1 }
    inf && /^    halt_now\(\);$/ { inf = 0; next }
    { print }
' "$INIT_C" > "$INIT_C.new" && mv "$INIT_C.new" "$INIT_C"
expect_red "3b(g) ovmx_exec_halt() gutted -- a halt that returns is not a halt" \
    "$R_NOTERM" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_TERMINAL" "$R_SHADOW"

# (g-ii) THE ESCAPE HATCH ONE FRAME DOWN. executive_attach() and
# ovmx_exec_halt() are both untouched and both look perfectly terminal; the
# early return is in halt_now(), whose LAST statement is still _exit(1). Only a
# chain-wide "no return anywhere" rule catches this.
sed -i 's#^    fflush(NULL);$#    fflush(NULL);\n    if (getenv("OVMX_ALLOW_NO_EXEC"))\n        return;#' "$INIT_C"
expect_red "3b(g-ii) early return inside halt_now(), past which _exit() still stands" \
    "$R_NOTERM" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_TERMINAL" "$R_SHADOW"

# (h) MACRO SHADOW. Every call to ovmx_exec_halt() now expands to an fprintf,
# while the function the gate inspects is still there and still halts. Pinning
# the callee BY NAME is a spelling test unless the name is reserved.
sed -i 's#^static int load_kernel_module(const char \*path)$#\#define ovmx_exec_halt(w, d) fprintf(stderr, "%%s\\n", (w))\n\nstatic int load_kernel_module(const char *path)#' "$INIT_C"
expect_red "3b(h) halt entry point redefined by a macro" \
    "$R_SHADOW" "$R_CAPTURE" "$R_BRANCH" "$R_HALT" "$R_ESCAPE" "$R_CLOSE" "$R_TERMINAL" "$R_NOTERM"

# ===========================================================================
# CHECK 3c -- no per-call executive-presence test. All four shapes below are
# real evasions that were demonstrated against an earlier revision of the gate.
# ===========================================================================

# (i) round 1's shape: capture, then `if (rc < 0)`.
cat > "$WORK/bind.c" <<'EOF'
static int kif_bind(void)
{
    int rc = vms_kif_open();
    if (rc < 0)
        return -1;
    return 0;
}
EOF
replace_bind() {
    awk -v repl="$1" '
        BEGIN { while ((getline line < repl) > 0) body = body line "\n"; close(repl) }
        /^static void kif_bind\(void\)$/ { skip = 1; printf "%s", body; next }
        skip && /^}$/ { skip = 0; next }
        skip { next }
        { print }
    ' "$KIF_C" > "$KIF_C.new" && mv "$KIF_C.new" "$KIF_C"
}
replace_bind "$WORK/bind.c"
expect_red "3c(i) capture then if (rc < 0)" "$R_PROBE"

# (ii) THE ROUND-2 ESCAPE: verbatim the ensure_kif_open() body Rule 9 deleted,
# hoisted into a variable. `>=` was invisible to the old operator class.
cat > "$WORK/bind.c" <<'EOF'
static int kif_bind(void)
{
    int rc = vms_kif_open();
    return rc >= 0 ? 0 : -1;
}
EOF
replace_bind "$WORK/bind.c"
expect_red "3c(ii) capture then 'return rc >= 0 ? 0 : -1' (deleted fallback, hoisted)" "$R_PROBE"

# (iii) THE OTHER ROUND-2 ESCAPE: switch was not a branch context.
cat > "$WORK/bind.c" <<'EOF'
static int kif_bind(void)
{
    int rc = vms_kif_open();
    switch (rc) {
    case -1:
        return -1;
    default:
        break;
    }
    return 0;
}
EOF
replace_bind "$WORK/bind.c"
expect_red "3c(iii) capture then switch (rc)" "$R_PROBE"

# (iv) inline shape, for completeness -- the one the direct pass always caught.
cat > "$WORK/bind.c" <<'EOF'
static int kif_bind(void)
{
    return vms_kif_open() >= 0 ? 0 : -1;
}
EOF
replace_bind "$WORK/bind.c"
expect_red "3c(iv) inline 'return vms_kif_open() >= 0 ? 0 : -1'" "$R_PROBE"

# ---------------------------------------------------------------------------
# (v)-(vii) THE SAME THREE SHAPES ON ONE LINE. Round 3 fixed the multi-line
# FORMATTING of this class, not the class: the gate discarded any line matching
# `=[[:space:]]*vms_kif_open`, so a line carrying BOTH the capture and the use
# was never scanned at all. (i)-(iii) above went RED while these went green.
# Whitespace is not a semantic boundary; these must be controls in their own
# right or the same fix can regress without any test noticing.
# ---------------------------------------------------------------------------

# (v) one-line form of (i).
cat > "$WORK/bind.c" <<'EOF'
static int kif_bind(void)
{
    int rc = vms_kif_open(); if (rc < 0) { }
    return 0;
}
EOF
replace_bind "$WORK/bind.c"
expect_red "3c(v) ONE LINE: 'int rc = vms_kif_open(); if (rc < 0) { }'" "$R_PROBE"

# (vi) one-line form of (ii) -- this is the exact escape the round-2 challenge
# text wrote, which round 3 then only closed in its multi-line spelling.
cat > "$WORK/bind.c" <<'EOF'
static int kif_bind(void)
{
    int rc = vms_kif_open(); (void)(rc >= 0 ? 0 : -1);
    return 0;
}
EOF
replace_bind "$WORK/bind.c"
expect_red "3c(vi) ONE LINE: 'int rc = vms_kif_open(); (void)(rc >= 0 ? 0 : -1);'" "$R_PROBE"

# (vii) one-line form of (iii).
cat > "$WORK/bind.c" <<'EOF'
static int kif_bind(void)
{
    int rc = vms_kif_open(); switch (rc) { case -1: break; default: break; }
    return 0;
}
EOF
replace_bind "$WORK/bind.c"
expect_red "3c(vii) ONE LINE: 'int rc = vms_kif_open(); switch (rc) { ... }'" "$R_PROBE"

echo ""
echo "=== Rule 9 negative controls: $passed passed, $failed failed ==="
exit "$status"
