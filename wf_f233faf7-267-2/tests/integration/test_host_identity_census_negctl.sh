#!/bin/sh
#
# test_host_identity_census_negctl.sh - the evasions (vms-cb5).
#
# test_host_identity_census.sh derives its census with a toolchain
# pipeline: objdump | grep | addr2line. That is the shape MOST able to
# fail silently. If the disassembly spelling differs on another
# architecture, or the symbol suffix is not '@plt', or addr2line returns
# '??', the pipeline yields nothing -- and "nothing" is exactly what a
# clean tree yields. A gate whose broken output is indistinguishable
# from its passing output asserts nothing at all, and that is the same
# defect (a census nobody had run) the gate itself exists to fix.
#
# So every property that gate claims gets its own minimal mutation here,
# and each must turn it RED for its own reason -- or, for the two claims
# that are about what the census EXCLUDES, must leave it GREEN while
# provably not naming the excluded site.
#
# HOW A MUTATION IS INJECTED, and why it is not a source edit. The gate
# reads BINARIES; a source edit would require a full rebuild per control
# (minutes each) and would mean writing into the repo tree while other
# builds may be running. Instead each control compiles a few lines of C
# into its own image and uses gcc's -fdebug-prefix-map to make the DWARF
# line table report a path under the source tree. That is precisely what
# the gate consumes, so the injection exercises the real extraction
# path -- and it also means these controls test the gate as it will
# behave for a REAL new call site, not a simulation of one.
#
# Usage: test_host_identity_census_negctl.sh [SRC_ROOT] [BUILD_ROOT]

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC_ROOT="${1:-$(cd "$HERE/../.." && pwd)}"
BUILD_ROOT="${2:-$SRC_ROOT/build}"
GATE="$HERE/test_host_identity_census.sh"
status=0

for t in gcc objdump addr2line; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "FAIL: $t is not available -- these controls cannot run, and a"
        echo "      control that cannot run must not report success."
        exit 1
    fi
done
if [ ! -d "$BUILD_ROOT/bin" ]; then
    echo "FAIL: $BUILD_ROOT/bin does not exist -- build first"
    exit 1
fi

SANDBOX=$(mktemp -d) || exit 1
trap 'rm -rf "$SANDBOX"' EXIT INT TERM

echo "vms-cb5 host-identity census — evasions"
echo ""

# fresh_build: a build tree that is the REAL one plus room for injected
# images. Hardlinks, so it costs nothing and the declared sites all still
# resolve -- which is what lets a control trip ONE property instead of
# also tripping "every declared site vanished".
fresh_build() {
    rm -rf "$SANDBOX/b"
    mkdir -p "$SANDBOX/b"
    cp -al "$BUILD_ROOT/bin" "$SANDBOX/b/bin" 2>/dev/null || cp -a "$BUILD_ROOT/bin" "$SANDBOX/b/bin"
    if [ -d "$BUILD_ROOT/lib" ]; then
        cp -al "$BUILD_ROOT/lib" "$SANDBOX/b/lib" 2>/dev/null || cp -a "$BUILD_ROOT/lib" "$SANDBOX/b/lib"
    fi
}

# inject <image-name> <mapped-subdir> <body-file>
#   compiles <body-file> into $SANDBOX/b/bin/<image-name>, with DWARF
#   paths rewritten to $SRC_ROOT/<mapped-subdir>/.
inject() {
    _img="$1"; _sub="$2"; _src="$3"
    mkdir -p "$SANDBOX/c"
    cp "$_src" "$SANDBOX/c/injected.c"
    gcc -g -O0 -fdebug-prefix-map="$SANDBOX/c=$SRC_ROOT/$_sub" \
        -o "$SANDBOX/b/bin/$_img" "$SANDBOX/c/injected.c" 2>"$SANDBOX/cc.err"
    if [ $? -ne 0 ]; then
        echo "FAIL: could not compile the injected image $_img"
        sed 's/^/      /' "$SANDBOX/cc.err"
        status=1
        return 1
    fi
    return 0
}

run_gate() { sh "$GATE" "$SANDBOX/b" "$SRC_ROOT" 2>&1; }

expect_red() {   # expect_red <label> [<substring that must appear>]
    _label="$1"; _needle="${2:-}"
    _out=$(run_gate); _rc=$?
    if [ "$_rc" -eq 0 ]; then
        echo "FAIL: $_label -- the gate stayed GREEN"
        printf '%s\n' "$_out" | sed 's/^/      /'
        status=1
        return
    fi
    if [ -n "$_needle" ] && ! printf '%s\n' "$_out" | grep -q "$_needle"; then
        echo "FAIL: $_label -- went red, but not for the stated reason"
        echo "      (expected output to mention: $_needle)"
        printf '%s\n' "$_out" | sed 's/^/      /'
        status=1
        return
    fi
    echo "  OK (went red): $_label"
}

expect_green() { # expect_green <label> [<substring that must NOT appear>]
    _label="$1"; _banned="${2:-}"
    _out=$(run_gate); _rc=$?
    if [ "$_rc" -ne 0 ]; then
        echo "FAIL: $_label -- the gate went RED"
        printf '%s\n' "$_out" | sed 's/^/      /'
        status=1
        return
    fi
    if [ -n "$_banned" ] && printf '%s\n' "$_out" | grep -q "$_banned"; then
        echo "FAIL: $_label -- stayed green but the census NAMED the site"
        printf '%s\n' "$_out" | sed 's/^/      /'
        status=1
        return
    fi
    echo "  OK (stayed green): $_label"
}

# --------------------------------------------------------------------
# 0. The unmutated tree passes. Without this every "went red" below
#    could be explained by the gate simply always failing.
# --------------------------------------------------------------------
fresh_build
expect_green "an unmutated build tree passes"

# --------------------------------------------------------------------
# A. A NEW UNDECLARED SITE IS CAUGHT -- and one that is NOT a
#    name-producing primitive, so it trips the undeclared-site property
#    ALONE. This is what separates property A from property B below: if
#    B were only an echo of A, this control would print B's message too.
# --------------------------------------------------------------------
fresh_build
cat > "$SANDBOX/A.c" <<'EOF'
#include <unistd.h>
#include <stdio.h>
int main(void) { printf("%u\n", (unsigned)getegid()); return 0; }
EOF
if inject NEGCTL_A.EXE src "$SANDBOX/A.c"; then
    out=$(run_gate); rc=$?
    if [ "$rc" -eq 0 ]; then
        echo "FAIL: A: a new getegid() site under src/ was not caught"
        status=1
    elif ! printf '%s\n' "$out" | grep -q "NEW -> getegid src/injected.c"; then
        echo "FAIL: A: went red but did not name the new site"
        printf '%s\n' "$out" | sed 's/^/      /'
        status=1
    elif printf '%s\n' "$out" | grep -q "turn a host account into a NAME again"; then
        echo "FAIL: A: the name-producing property ALSO fired on a getegid()"
        echo "      site -- it is not independent of the undeclared-site check"
        status=1
    else
        echo "  OK (went red): A: a new undeclared site is caught, and ONLY the"
        echo "                    undeclared-site property fires"
    fi
fi

# --------------------------------------------------------------------
# B. A NAME-PRODUCING PRIMITIVE trips its own derived property, with its
#    own message, in addition to A. This is the defect that survived
#    three rounds of vms-cb5: getpwuid() answering a VMS question.
# --------------------------------------------------------------------
fresh_build
cat > "$SANDBOX/B.c" <<'EOF'
#include <pwd.h>
#include <unistd.h>
#include <stdio.h>
int main(void) {
    struct passwd *pw = getpwuid((uid_t)1000);
    printf("%s\n", pw ? pw->pw_name : "");
    return 0;
}
EOF
if inject NEGCTL_B.EXE src "$SANDBOX/B.c"; then
    out=$(run_gate); rc=$?
    ok=1
    [ "$rc" -eq 0 ] && ok=0
    printf '%s\n' "$out" | grep -q "NEW -> getpwuid src/injected.c" || ok=0
    printf '%s\n' "$out" | grep -q "getpwuid is reached by the product" || ok=0
    if [ "$ok" -eq 1 ]; then
        echo "  OK (went red): B: a restored getpwuid() trips BOTH the"
        echo "                    undeclared-site check and the derived"
        echo "                    no-name-producing-primitive property"
    else
        echo "FAIL: B: a restored getpwuid() under src/ was not fully caught"
        printf '%s\n' "$out" | sed 's/^/      /'
        status=1
    fi
fi

# --------------------------------------------------------------------
# C. THE CENSUS CANNOT DERIVE TO NOTHING AND PASS. Point the gate at a
#    build tree that contains only the injected image: every declared
#    site is absent, and the gate must say so rather than print a short
#    census and a PASS. This is the control for the failure mode that
#    worries me most -- the extraction silently matching nothing on an
#    architecture whose disassembly spells calls differently.
# --------------------------------------------------------------------
rm -rf "$SANDBOX/b"; mkdir -p "$SANDBOX/b/bin"
cat > "$SANDBOX/C.c" <<'EOF'
#include <stdio.h>
int main(void) { printf("no identity calls here\n"); return 0; }
EOF
if inject NEGCTL_C.EXE src "$SANDBOX/C.c"; then
    expect_red "C: an EMPTY census fails instead of passing as clean" "GONE ->"
fi

# --------------------------------------------------------------------
# D. A SITE UNDER tests/ DOES NOT ENTER THE CENSUS. The gate claims this
#    scope limit in its header, and tests really do call getpwuid --
#    tests/qemu/test_syssvc_ident.c does, on purpose, to prove the Linux
#    name existed at the moment DCL declined to answer with it. If that
#    entered the census the gate would be permanently red for a reason
#    that is not a defect, and would get an exemption added and then get
#    ignored.
# --------------------------------------------------------------------
fresh_build
if inject NEGCTL_D.EXE tests "$SANDBOX/B.c"; then
    expect_green "D: a getpwuid() site under tests/ is excluded" "injected.c"
fi

# --------------------------------------------------------------------
# E. THE GATE READS THE BINARY, NOT THE SOURCE. This is the whole reason
#    it exists next to the source-scanning census, so it gets a control:
#    a file whose TEXT is full of getpwuid -- in a comment, in a string,
#    and in a #if 0 block -- but which compiles to no such call must not
#    enter the census. A grep-based gate fails this control; that is the
#    point.
# --------------------------------------------------------------------
fresh_build
cat > "$SANDBOX/E.c" <<'EOF'
#include <stdio.h>
/* getpwuid(getuid()) -- named in a comment, which a source scan matches */
#if 0
#include <pwd.h>
int dead(void) { struct passwd *pw = getpwuid(0); return pw != 0; }
#endif
int main(void) { printf("getpwnam getlogin cuserid\n"); return 0; }
EOF
if inject NEGCTL_E.EXE src "$SANDBOX/E.c"; then
    expect_green "E: getpwuid in a comment, a string and dead code does NOT enter the census" "injected.c"
fi

# --------------------------------------------------------------------
# F. THE vms$get_uic CLAIM IS CHECKED, NOT ASSERTED. The gate declares
#    sys_security.c's host-derived UIC legal partly because nothing
#    renders a NAME from it, and backs that by requiring no product
#    image outside its own library to reference the symbol. Give one
#    image a reference and the gate must notice.
# --------------------------------------------------------------------
fresh_build
cat > "$SANDBOX/F.c" <<'EOF'
#include <stdio.h>
#include <stdint.h>
__attribute__((noinline)) uint32_t vms$get_uic(void) { return 0x00010004u; }
int main(void) { printf("%u\n", vms$get_uic()); return 0; }
EOF
if inject NEGCTL_F.EXE src "$SANDBOX/F.c"; then
    expect_red "F: a product image referencing vms\$get_uic is caught" "call vms\$get_uic"
fi

echo ""
if [ "$status" -eq 0 ]; then
    echo "PASS: every host-identity census property can be tripped, and the two"
    echo "      exclusions it claims really exclude"
else
    echo "FAIL: at least one host-identity census property could not be tripped"
fi
exit $status
