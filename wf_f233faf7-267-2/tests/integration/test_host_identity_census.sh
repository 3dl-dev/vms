#!/bin/sh
#
# test_host_identity_census.sh - standing gate (vms-cb5): the complete
# census of every place OVMX asks the HOST who somebody is, DERIVED FROM
# THE BUILT BINARIES.
#
# =====================================================================
# WHY THIS GATE EXISTS
# =====================================================================
# Three consecutive rounds of vms-cb5 fixed the identity-fabrication
# sites they were handed and then claimed the CLASS was settled. Each
# time the class was still alive somewhere nobody had enumerated:
#
#   round 1  deleted the getpwuid() fallback in DCL's lex_user()
#            -> five `ctx->username[0] ? ctx->username : "SYSTEM"`
#               sites survived in two other files.
#   round 2  deleted those five
#            -> the IDENTICAL getpwuid() fallback survived in
#               F$IDENTIFIER, 1840 lines below the fix, IN THE SAME
#               FILE, and another in sys_operator.c. Settling command:
#                 printf 'X = F$IDENTIFIER(1000,"NUMBER_TO_NAME")\n
#                         SHOW SYMBOL X\n' | DCL.EXE   ->   X = "BARON"
#               ("BARON" is the Linux login of the build machine.)
#
# The common shape is A PER-SITE FIX CARRYING A CLASS-WIDE CLAIM. The
# defence is not more careful reading; it is refusing to let the class
# be a list somebody typed. So this gate DERIVES the list.
#
# =====================================================================
# HOW IT DERIVES IT -- FROM THE BINARY, NOT FROM THE SOURCE
# =====================================================================
# For every product executable and shared library under the build tree
# it disassembles the image, finds every call whose target is one of the
# host-identity primitives, and maps the calling instruction's address
# back to file:line through the DWARF line table (addr2line). The census
# is the set of (primitive, source file) pairs that survive.
#
# This is deliberately stronger than grepping the source:
#   - it cannot be fooled by a call inside a comment, a string, an
#     #if 0, or a #ifdef branch that is not compiled;
#   - it reports only what actually LINKS INTO THE PRODUCT, so a site
#     that exists but is dead does not have to be argued about;
#   - it is blind to spelling. A call reached through a macro, a typedef
#     or a wrapper still shows up as a call to the primitive.
# It is weaker in one way that matters, disclosed below.
#
# =====================================================================
# WHY THESE PRIMITIVES, AND WHAT "IN THE CLASS" MEANS
# =====================================================================
# CLAUDE.md Rule 11: a VMS identity is executive-resident. Rule 10: for
# any behaviour there are two legal answers -- reproduce what OpenVMS
# does, or make the condition unreachable. "This process has no user
# name" is not a condition OpenVMS faces, so there is no legal fallback
# VALUE for it -- and asking the host is the illegal third answer, every
# time, because the host's answer is always available and always wrong.
#
# A site is IN THE CLASS when a host identity can become a VMS user
# name, UIC or identifier in output. A site is OUT of the class when the
# host identity is used AS a host identity -- a Linux uid checked
# against Linux privilege -- and never becomes a VMS name. Both kinds
# are censused here: the point is that every one of them is looked at,
# not that only the bad ones are listed.
#
# =====================================================================
# WHAT THIS GATE DOES NOT ENFORCE
# =====================================================================
# Stated because an undisclosed limit is how the refuted claims got
# written in the first place.
#
#   - It censuses CALLS TO NAMED LIBC PRIMITIVES. A site that opens and
#     parses /etc/passwd by hand, or reads it through NSS by another
#     route, is invisible here. So is a FABRICATED LITERAL -- the
#     `: "SYSTEM"` defaults this item also removed are not calls and
#     cannot appear in this census. Those are pinned behaviourally by
#     tests/qemu/test_syssvc_ident.c scenario G and by the negative
#     controls in tests/qemu/facility_defects.sh, not here.
#   - The ENVIRONMENT half of the same class (VMS_USERNAME, USER,
#     LOGNAME ...) is censused separately and by a different method, in
#     tests/integration/test_env_identity_census.sh.
#   - It scans the build tree it is pointed at. Anything not built there
#     is not censused; the kernel module is built by its own Makefile
#     and is not covered (it is kernel code and has no libc passwd
#     database to ask).
#   - It says nothing about whether a value, once obtained, is USED.
#     Four of the five declared sites below use it and are still legal;
#     the declaration text is where that argument lives.
#
# IF YOU ARE HERE BECAUSE THIS FAILED: adding your new site to the
# declared set is the WRONG first move. Ask Rule 10's question first --
# is the answer you want a VMS answer? If it is a user name, a UIC or
# an identifier, the host cannot supply it. Read the executive
# (vms_kif_getjpi_self) or the authorization file (sysuaf_lookup), or
# report that there is none.
#
# Usage: test_host_identity_census.sh [BUILD_ROOT] [SRC_ROOT]

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
SRC_ROOT_DEFAULT=$(cd "$HERE/../.." && pwd)
BUILD_ROOT="${1:-$SRC_ROOT_DEFAULT/build}"
SRC_ROOT="${2:-$SRC_ROOT_DEFAULT}"
status=0

echo "vms-cb5 host-identity census: every call into the HOST's identity"
echo "  primitives, DERIVED from the BUILT BINARIES under $BUILD_ROOT"
echo ""

# ---------------------------------------------------------------------------
# The primitives. Every libc route to "who is the caller / who is this
# uid / who is this name" that the product could plausibly reach.
# getgid/getegid are here because a GID is half a UIC -- get_uic() builds
# a VMS UIC out of getgid() and getuid() together, and a census that
# watched only the member half would have missed the group half.
# ---------------------------------------------------------------------------
PRIMS="getpwuid getpwnam getpwent getpwuid_r getpwnam_r getuid geteuid getgid getegid getlogin getlogin_r cuserid getresuid"

# ---------------------------------------------------------------------------
# THE DECLARED CENSUS.  One line per site: <primitive> <file>
#
# Line numbers are DELIBERATELY NOT recorded -- a census pinned to line
# numbers fails on every unrelated edit above it, and a gate that cries
# wolf gets an exemption added and then gets ignored. (Same reasoning,
# and the same wording, as test_env_identity_census.sh.)
#
# WHY EACH ONE IS LEGAL. Read this before adding a line.
#
#  geteuid  src/vmsdcl/dcl_cmd_misc.c
#      TCPIP SET INTERFACE / SET ROUTE. Asks whether THIS LINUX PROCESS
#      may configure a Linux network interface, because what follows is
#      a Linux ioctl and an `ip route` invocation that root-ness alone
#      decides. No VMS name, UIC or identifier is derived from it. OUT
#      OF THE CLASS -- it uses the host identity as a host identity.
#
#  geteuid  tools/vms_authorize.c
#      AUTHORIZE's privilege check, first clause. Same shape: root is
#      allowed to edit SYSUAF.DAT because SYSUAF.DAT is a root-owned
#      file. OUT OF THE CLASS. (Its SECOND clause reads getenv("USER")
#      and is a genuine env-identity path -- censused by the OTHER gate,
#      test_env_identity_census.sh, which is why USER is in that gate's
#      variable list.)
#
#  getuid   tools/vms_login.c
#  geteuid  tools/vms_login.c
#  getgid   tools/vms_login.c
#  getegid  tools/vms_login.c
#      LOGINOUT VERIFYING ITS OWN CREDENTIAL DROP. All four calls are in
#      the post-condition of setgroups/setgid/setuid: having become the
#      authenticated user's UIC, LOGINOUT reads the credentials back and
#      _exit()s if they are not what it demanded. This is the OPPOSITE
#      of the class -- it does not turn a host identity into a VMS one,
#      it checks that a VMS identity was successfully imposed on the
#      host one. Deleting these would remove a check, not a fabrication.
#
#  getuid   src/libvms/syssvc/sys_security.c
#  getgid   src/libvms/syssvc/sys_security.c
#      get_uic(), which composes (getgid() << 16) | getuid() and hands
#      it to sys$chkpro's protection arithmetic.
#      THIS IS THE ONE DECLARATION THAT IS NOT SELF-EVIDENT, so it gets
#      the argument rather than an assertion: after LOGINOUT's drop the
#      host uid/gid ARE the SYSUAF UIC BY CONSTRUCTION -- tools/vms_login.c
#      setuid()s to rec->uic_member and setgid()s to rec->uic_group and
#      dies if either fails -- so for a logged-in process this reads back
#      an executive-established identity through the host's copy of it.
#      For a process that did NOT come through LOGINOUT it is just the
#      host's uid, and that is a real gap: it belongs to the same
#      executive-residency work as the rest (Rule 11), not to this item.
#      It stays out of the class HERE for a narrower and checkable
#      reason: nothing turns it into a NAME, and its result reaches no
#      user-visible identifier. That claim is not left as prose -- it is
#      checked below.
#
# WHAT IS NOT IN THIS LIST, AND WAS, BEFORE THIS ITEM:
#      getpwnam + getpwuid  src/vmsdcl/dcl_lexical.c   (F$IDENTIFIER)
#      getuid  + getpwuid   src/libvms/syssvc/sys_operator.c (OPCOM record)
#      getpwnam x2 + getuid + getpwuid  tools/vms_mail.c
#          (sender name, recipient existence, home directory)
#      All deleted. If any of them comes back, this gate names it.
# ---------------------------------------------------------------------------
# THREE OF THESE EIGHT LINES WERE NOT IN THE FIRST DRAFT OF THIS FILE.
# I hand-listed the sites from a probe that matched only
# getpwuid|getpwnam|getuid|geteuid|getlogin|cuserid, then wrote the
# declarations from that list -- and the getgid/getegid half of the very
# UIC composition the sys_security.c note argues about was missing from
# my own census. The gate's first real run named all three. That is the
# argument for deriving the list rather than typing it, made against
# this file's own author on the day it was written.
DECLARED=$(cat <<'EOF'
geteuid src/vmsdcl/dcl_cmd_misc.c
geteuid tools/vms_authorize.c
geteuid tools/vms_login.c
getegid tools/vms_login.c
getgid tools/vms_login.c
getuid tools/vms_login.c
getgid src/libvms/syssvc/sys_security.c
getuid src/libvms/syssvc/sys_security.c
EOF
)

# ---------------------------------------------------------------------------
# THE TOOLS. A missing tool must FAIL, not skip: a census that silently
# reports nothing is indistinguishable from a clean tree, and would have
# passed on every round this gate exists to have caught.
# ---------------------------------------------------------------------------
for t in objdump addr2line; do
    if ! command -v "$t" >/dev/null 2>&1; then
        echo "FAIL: $t is not available -- this gate cannot derive the census"
        echo "      without it, and a census that cannot be derived must not"
        echo "      be reported as empty."
        exit 1
    fi
done

if [ ! -d "$BUILD_ROOT" ]; then
    echo "FAIL: build tree $BUILD_ROOT does not exist -- nothing to census"
    exit 1
fi

IMAGES=$(ls "$BUILD_ROOT"/bin/* "$BUILD_ROOT"/lib/* 2>/dev/null)
if [ -z "$IMAGES" ]; then
    echo "FAIL: no images found under $BUILD_ROOT/bin or $BUILD_ROOT/lib"
    exit 1
fi

# Alternation for the disassembly match.
PRIM_ALT=$(printf '%s' "$PRIMS" | tr ' ' '|')

TMPD=$(mktemp -d) || exit 1
trap 'rm -rf "$TMPD"' EXIT INT TERM
: > "$TMPD/obs"
: > "$TMPD/scanned"

for img in $IMAGES; do
    [ -f "$img" ] || continue
    # ELF only. Reading four bytes is cheaper and more portable than
    # depending on file(1) being installed. NOTE the exact spelling:
    # `od -c` renders the 0x7F byte as the three characters 177 with NO
    # backslash, so a pattern written '\177ELF*' matches nothing and the
    # scan silently censuses zero images. It did, on the first run of
    # this file; the missing-declared-site check below is what caught
    # it rather than a green PASS on an empty census.
    magic=$(od -An -N4 -c "$img" 2>/dev/null | tr -d ' ')
    case "$magic" in
        177ELF*) ;;
        *) continue ;;
    esac
    echo "$img" >> "$TMPD/scanned"

    # Both call spellings: aarch64 emits `bl <sym@plt>` (and `b` for a
    # tail call); x86_64 emits `call ... <sym@plt>` (and `jmp`). The
    # match is on the SYMBOL NAME IN ANGLE BRACKETS at end of line,
    # which is common to both, so this does not silently census nothing
    # on the other architecture. If the linker resolved the call
    # without a PLT the '@plt' suffix is absent, which is why it is
    # optional here.
    objdump -d "$img" 2>/dev/null \
      | grep -oE "^[[:space:]]*[0-9a-f]+:.*<($PRIM_ALT)(@plt)?>$" \
      | sed -E "s/^[[:space:]]*([0-9a-f]+):.*<($PRIM_ALT)(@plt)?>$/\1 \2/" \
      | sort -u \
      | while read -r addr sym; do
            loc=$(addr2line -e "$img" "0x$addr" 2>/dev/null | head -1)
            f=${loc%%:*}
            case "$f" in
                "$SRC_ROOT"/src/*|"$SRC_ROOT"/tools/*)
                    printf '%s %s\n' "$sym" "${f#"$SRC_ROOT"/}" >> "$TMPD/obs"
                    ;;
                *)
                    # Not product source: a test, a system header, an
                    # inline from libc, or an address DWARF could not
                    # place. Deliberately dropped -- see the scope note.
                    ;;
            esac
        done
done

sort -u "$TMPD/obs" > "$TMPD/obs.sorted"
printf '%s\n' "$DECLARED" | grep -v '^$' | sort > "$TMPD/dec.sorted"

# `grep -c` EXITS 1 ON ZERO MATCHES, so `$(grep -c . f || echo 0)` yields
# the two-line string "0\n0" and every later [ "$n" -ne 0 ] dies with
# "Illegal number". Count with wc instead, which cannot fail this way.
nimg=$(wc -l < "$TMPD/scanned" | tr -d ' ')
nobs=$(wc -l < "$TMPD/obs.sorted" | tr -d ' ')
echo "  images scanned: $nimg"
echo "OBSERVED CENSUS ($nobs sites):"
sed 's/^/  /' "$TMPD/obs.sorted"
echo ""

UNDECLARED=$(comm -23 "$TMPD/obs.sorted" "$TMPD/dec.sorted")
MISSING=$(comm -13 "$TMPD/obs.sorted" "$TMPD/dec.sorted")

if [ -n "$UNDECLARED" ]; then
    echo "FAIL: OVMX asks the host who somebody is at an undeclared site"
    printf '%s\n' "$UNDECLARED" | sed 's/^/  NEW -> /'
    echo "  Read the header of this file before declaring it."
    status=1
else
    echo "  OK: no undeclared call into a host identity primitive"
fi

# A declared site that has vanished fails too. This is not pedantry: it
# is what makes an EMPTY census impossible to pass off as a clean one.
# If the extraction above silently produced nothing -- wrong
# architecture, stripped binaries, a disassembly format this grep does
# not match -- all five declared sites go missing at once and the gate
# says so, instead of printing "0 sites" and PASS.
if [ -n "$MISSING" ]; then
    echo "FAIL: a declared site no longer appears in the built product"
    printf '%s\n' "$MISSING" | sed 's/^/  GONE -> /'
    echo "  If you deleted it (usually the right answer), delete its line"
    echo "  from DECLARED above too. If ALL of them are listed here, the"
    echo "  census did not derive -- suspect this script, not the tree."
    status=1
else
    echo "  OK: every declared site still exists"
fi

# ---------------------------------------------------------------------------
# THE ONE DERIVED PROPERTY WORTH STATING SEPARATELY.
#
# Not "there are five sites" -- a number a human has to keep true. What
# carries the security meaning is that NO NAME-PRODUCING primitive is
# reached at all. getpwuid/getpwnam/getpwent/getlogin/cuserid are the
# calls that turn a host id into a NAME; getuid/geteuid/getgid/getegid
# only ever yield a number. Every site this item deleted was one of the
# former. This is DERIVED from the census above, so it cannot be true
# here and false in the tree.
# ---------------------------------------------------------------------------
echo ""
name_prims=0
for p in getpwuid getpwnam getpwent getpwuid_r getpwnam_r getlogin getlogin_r cuserid; do
    n=$(grep "^$p " "$TMPD/obs.sorted" 2>/dev/null | wc -l | tr -d ' ')
    if [ "$n" -ne 0 ]; then
        echo "FAIL: $p is reached by the product at $n site(s) -- something can"
        echo "      turn a host account into a NAME again"
        grep "^$p " "$TMPD/obs.sorted" | sed 's/^/  -> /'
        name_prims=$((name_prims + n))
        status=1
    fi
done
# Reported off its OWN counter, not off $status: gated on $status this
# line would go silent whenever ANY other check failed, which is exactly
# when a reader most needs to know whether this property still holds.
if [ "$name_prims" -eq 0 ]; then
    echo "  OK: the product reaches NO host-name-producing primitive"
    echo "      (getpwuid/getpwnam/getpwent/getlogin/cuserid and their"
    echo "      _r forms) -- derived from the census above, not declared"
fi

# ---------------------------------------------------------------------------
# THE ONE DECLARATION THAT ARGUED "its result reaches no name", CHECKED.
#
# sys_security.c's get_uic() is declared legal partly because nothing
# turns its UIC into a user name. vms$get_uic is its only exported
# entry point, so if the product started rendering a name from it, a
# product image would have to reference the symbol. Nothing in src/ or
# tools/ calls it today -- it is used only inside sys_security.c itself
# and by tests/libvms/test_protection.c. Derived, not asserted.
# ---------------------------------------------------------------------------
echo ""
getuic_callers=0
for img in $(cat "$TMPD/scanned"); do
    case "$(basename "$img")" in
        test_*) continue ;;   # tests may call it; that is not the product
        LIBVMS\$SHR.EXE) continue ;;  # defines it
    esac
    if objdump -d "$img" 2>/dev/null | grep -qE '<vms\$get_uic(@plt)?>$'; then
        echo "  -> $(basename "$img") references vms\$get_uic"
        getuic_callers=$((getuic_callers + 1))
    fi
done
if [ "$getuic_callers" -eq 0 ]; then
    echo "  OK: no product image outside its own library calls vms\$get_uic,"
    echo "      so the host-derived UIC reaches no user-visible name"
else
    echo "FAIL: $getuic_callers product image(s) now call vms\$get_uic. The"
    echo "      sys_security.c declaration above argued that the host-derived"
    echo "      UIC reaches no name; re-derive that argument before passing."
    status=1
fi

echo ""
if [ "$status" -eq 0 ]; then
    echo "PASS: host-identity census matches"
else
    echo "FAIL: host-identity census does not match"
fi
exit $status
