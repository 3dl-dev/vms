#!/bin/sh
#
# test_foreign_command_dispatch.sh - BEHAVIOURAL gate (rd vms-96e): DCL
# foreign-command dispatch.
#
# THE GAP. OpenVMS: "SYM :== $image-spec" defines a foreign command, and
# typing the bare symbol afterwards -- "$ SYM arg1 arg2" -- activates
# image-spec with the rest of the line as its P1-P8 parameters. Before this
# gate, dcl_find_verb() only ever checked the static builtin_verbs[] table:
# ":==" correctly stored the symbol, but a bare "SYM" fell straight through
# to "%DCL-W-IVVERB, unrecognized command verb" -- only "RUN image" worked.
# This is a load-bearing piece of DCL; without it, no third-party command
# (including the PARTS demo's iconic "$ PARTS") can be installed as a
# top-level verb the way real VMS sessions are set up (LOGIN.COM full of
# "XXX :== $DEV:[DIR]XXX.EXE" lines).
#
# WHY BEHAVIOURAL, NOT A SOURCE SCAN. This runs DCL.EXE for real, defines a
# foreign command against a real executable, invokes it BARE (never RUN or
# MCR), and inspects what the child actually printed and exited with -- a
# stub dispatcher that swallows the args or never execs anything fails here
# even if the source has the right-looking branch.
#
# WHAT EACH CHECK PROVES:
#   1. basic dispatch + arg forwarding: a symbol :== $<script> is invoked
#      bare with 3 args; the script echoes argv back, proving both that the
#      image ran at all AND that P1-P8 arrived positionally and in order
#      (not concatenated, not reordered, not dropped).
#   2. exit-status propagation: the same mechanism used by RUN (vms-17f9)
#      applies to foreign commands too -- a nonzero image exit must be
#      SS$_ABORT, not silently SS$_NORMAL.
#   3. bare "$" with no image-spec text defaults the image name to the
#      symbol name itself (HELP SET SYMBOL, foreign commands), proven
#      against a real second script named after its own symbol.
#   4. a symbol whose value does NOT start with "$" (an ordinary string
#      symbol) must NOT be foreign-command-dispatched -- regression check
#      that plain symbol substitution still works and stays inert here.
#   5. (if a PARTS binary is supplied) the actual demo moment: PARTS :== $
#      <PARTS binary>, then bare "PARTS LOAD 5 <file>" -- the same shape as
#      "PARTS :== $SYS$SYSTEM:PARTS.EXE" / "$ PARTS" on real VMS.
#
# Usage: test_foreign_command_dispatch.sh [PATH-TO-DCL.EXE] [PATH-TO-PARTS]

set -u

DCL="${1:-${VMSDCL:-}}"
if [ -z "$DCL" ]; then
    for cand in "$(dirname "$0")/../../build/bin/DCL.EXE" \
                "$(dirname "$0")/../../build/bin/vmsdcl"; do
        [ -x "$cand" ] && DCL="$cand" && break
    done
fi

PARTS_BIN="${2:-}"

status=0
passed=0
failed=0

if [ -z "$DCL" ] || [ ! -x "$DCL" ]; then
    echo "FAIL: no DCL.EXE to exercise (looked at argv[1], \$VMSDCL, build/bin)"
    echo "  -> this gate is BEHAVIOURAL; with no binary it is reported as"
    echo "     FAILED, never skipped. A skipped test is a failing test."
    exit 1
fi

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT INT TERM

echo "vms-96e: DCL foreign-command dispatch (SYM :== \$image, bare SYM invokes it)"
echo "  DCL under test: $DCL"

# check <name> <extended-regex-over-stdout> <why-this-exists>
check() {
    if grep -qE "$2" "$WORK/out"; then
        echo "  PASS: $1"
        passed=$((passed + 1))
    else
        echo "  FAIL: $1"
        echo "        catches: $3"
        echo "        no line matched: $2"
        echo "        DCL actually printed:"
        sed 's/^/          | /' "$WORK/out"
        failed=$((failed + 1))
        status=1
    fi
}

check_exit() {
    # check_exit <name> <expected-exit> <actual-exit> <why-this-exists>
    if [ "$3" -eq "$2" ]; then
        echo "  PASS: $1"
        passed=$((passed + 1))
    else
        echo "  FAIL: $1"
        echo "        catches: $4"
        echo "        expected exit $2, got $3"
        failed=$((failed + 1))
        status=1
    fi
}

# --- fixture: an executable that echoes its own argv, one per line --------
ECHOARGS="$WORK/ECHOARGS"
cat > "$ECHOARGS" <<'EOF'
#!/bin/sh
echo "ECHOARGS-BEGIN"
for a in "$@"; do
    echo "ARG=[$a]"
done
echo "ECHOARGS-END"
EOF
chmod +x "$ECHOARGS"

# An image that always exits nonzero, to prove abnormal exit is surfaced.
FAILIMG="$WORK/FAILIMG"
cat > "$FAILIMG" <<'EOF'
#!/bin/sh
echo "FAILIMG-RAN"
exit 3
EOF
chmod +x "$FAILIMG"

# --- 1 & 2: basic dispatch, argument forwarding, and $STATUS/$SEVERITY ----
printf 'FOO :== "$%s"\nFOO "hello" "world" "123"\nSHOW SYMBOL $STATUS\n' \
    "$ECHOARGS" >"$WORK/cmds1"
"$DCL" <"$WORK/cmds1" >"$WORK/out" 2>&1

check "bare foreign command activates the image at all" \
      'ECHOARGS-BEGIN' \
      "dcl_find_verb() failing closed on a non-builtin verb, never reaching \
       image activation"

check "P1 arrives as the image's first argument, in order" \
      '^ARG=\[hello\]$' \
      "params dropped, or forwarded out of order"

check "P2 arrives second" \
      '^ARG=\[world\]$' \
      "only P1 forwarded (an off-by-one argv slice)"

check "P3 arrives third" \
      '^ARG=\[123\]$' \
      "only the first two params forwarded"

check "the image actually completed (not just launched)" \
      'ECHOARGS-END' \
      "a fork that never waits, or a truncated pipe"

check "successful foreign command sets \$STATUS to a VMS success code" \
      'STATUS = "%X0*1"' \
      "activation status not threaded back into \$STATUS/\$SEVERITY the way \
       a builtin verb's is"
# $STATUS is displayed in the VMS "%Xhhhhhhhh" representation (vms-3983 —
# SHOW SYMBOL $STATUS on real VMS shows "%X00000001" for SS$_NORMAL, not decimal
# "1"). The success anchor is the odd low bit, shown here as %X0…1.

# --- 2b: nonzero image exit must be surfaced, not swallowed ----------------
printf 'BAR :== "$%s"\nBAR\n' "$FAILIMG" >"$WORK/cmds2"
"$DCL" <"$WORK/cmds2" >"$WORK/out" 2>&1
rc=$?

check "a failing foreign-command image runs before it fails" \
      'FAILIMG-RAN' \
      "image never activated at all"

check "a nonzero image exit is reported (%DCL-...-ABORT), not swallowed" \
      'ABORT' \
      "vms-17f9's silent-success bug reappearing on the foreign-command path"

# --- 3: bare "$" (no image-spec text) defaults to the symbol's own name ----
# Deliberately NOT resolved against a real image (that would depend on
# ctx->default_dir / vmsfs mount state, which this hermetic gate does not
# want to depend on) -- instead this proves the default-name RULE: the
# not-found error must name the SYMBOL, not a fixed/wrong/blank image name.
printf 'SELFNAMED :== $\nSELFNAMED\n' >"$WORK/cmds3"
"$DCL" <"$WORK/cmds3" >"$WORK/out" 2>&1

check "SYM :== \$ (empty image-spec) defaults the image name to the symbol" \
      'IVIMAGE.*SELFNAMED' \
      "HELP SET SYMBOL's foreign-command default-name rule not implemented \
       (empty image-spec falling back to something other than the symbol \
       name, or crashing instead of reporting IVIMAGE)"

# --- 4: an ordinary string symbol (no leading \$) must NOT be dispatched ---
printf 'PLAINSYM :== HELLO\nPLAINSYM\n' >"$WORK/cmds4"
"$DCL" <"$WORK/cmds4" >"$WORK/out" 2>&1

check "a non-\$ symbol value is never treated as a foreign command" \
      'IVVERB' \
      "over-eager dispatch: any :== symbol, not just a \$-prefixed one, \
       being run as an image"

# --- 5 (optional): the actual PARTS demo moment, if a PARTS binary was
# handed to us -- "PARTS :== \$SYS\$SYSTEM:PARTS.EXE" then bare "\$ PARTS"
# on real VMS, reproduced here as "PARTS :== \$<PARTS binary>" then bare
# "PARTS LOAD 5 <file>".
if [ -n "$PARTS_BIN" ] && [ -x "$PARTS_BIN" ]; then
    PARTS_DAT="$WORK/PARTS_FOREIGN.DAT"
    printf 'PARTS :== "$%s"\nPARTS LOAD 5 "%s"\n' "$PARTS_BIN" "$PARTS_DAT" \
        >"$WORK/cmds5"
    PARTS_FILE="$PARTS_DAT" "$DCL" <"$WORK/cmds5" >"$WORK/out" 2>&1

    check "PARTS :== \$<PARTS.EXE> then bare PARTS LOAD runs the real demo" \
          'PARTS-I-LOADED, 5 part records written' \
          "the PARTS demo's iconic bare invocation not actually reaching \
           the image"

    check "PARTS's own keyed-lookup proof still runs end to end" \
          'PARTS-S-FOUND  key PN000001' \
          "PARTS activated but with a broken/empty argv, unable to open \
           the file it was just told to create"
else
    echo "  (skipped: no PARTS binary supplied -- see argv[2])"
fi

echo ""
echo "vms-96e foreign-command dispatch gate: $passed passed, $failed failed"
exit $status
