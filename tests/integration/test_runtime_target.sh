#!/bin/sh
#
# test_runtime_target.sh - Rule 9 standing gate: ONE runtime target
#
# Operator ruling 2026-07-28 (CLAUDE.md Project-Specific Rule 9): the Docker
# RUNTIME layer is dead. OVMX has exactly one runtime -- the real-kernel/QEMU
# path, where vms.ko provides the VMS executive via /dev/vms.
#
# WHY THIS GATE EXISTS. This ruling was made, recorded in prose, and then
# forgotten -- an agent re-derived "Docker is a live dev/CI target" from the
# repo (the files are still there) and designed an executive around Docker's
# lack of /dev/vms. Prose did not hold. A gate does.
#
# The deeper trap it guards: because CI runs in Docker and Docker has no
# /dev/vms, the kernel executive is unprovable in CI -- so OVMX grew
# per-process userspace fakes (logical names, process table, event flags,
# mailboxes) that report success while sharing nothing. THE ARCHITECTURE
# DRIFTED TO FIT THE TEST HARNESS. This gate makes that drift loud.
#
# If you are here because this failed: do NOT add an allowlist entry. Move the
# work to the QEMU path, where a real executive exists.
#
# NOTE, 2026-07-29 -- this header used to end "...or fail honestly when
# /dev/vms is absent (SS$_NOSUCHDEV, as sys_lock.c does)". THAT ADVICE IS
# SUPERSEDED and was itself the defect. Governing rule: what would VMS do?
# SYSBOOT loads the executive before any process exists, so VMS is NEVER in
# the state where a running system has no executive. "Fail honestly with
# SS$_NOSUCHDEV" encoded "OVMX runs, minus the executive" -- a system that
# does not exist. A handled-but-impossible-on-VMS state is the same class of
# lie as a fake that reports success; it just fails more politely.
#
# The rule now is a GUARANTEE, not an error path: PID 1 refuses to boot
# without the executive and pins it open for the life of the system
# (src/ovmx_init/ovmx_init.c, executive_attach), so no caller can observe its
# absence. Per-call fallbacks are DELETED, not corrected. SS$_NOSUCHDEV keeps
# its real meaning -- a caller named a VMS device that does not exist.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

echo "Rule 9 gate: one runtime target (kernel/QEMU); Docker is not a runtime"

# --- 1. The dead-legacy runtime files must stay marked ------------------
# They are retained only until CI migrates off them. While they exist they
# must announce that they are not a runtime, so nobody re-derives otherwise.
for f in "$SRC_ROOT/Dockerfile" "$SRC_ROOT/docker-compose.yml"; do
    [ -f "$f" ] || continue          # deleted == migration finished == fine
    if grep -q "DEAD LEGACY" "$f"; then
        echo "  OK: $(basename "$f") is marked dead legacy"
    else
        echo "FAIL: $(basename "$f") exists without the DEAD LEGACY marker"
        echo "  -> it is not a runtime target (Rule 9); mark it or delete it."
        status=1
    fi
done

# --- 2. No NEW Docker-runtime entry points -----------------------------
# Exactly one compose file may exist, and only as marked legacy.
extra=$(find "$SRC_ROOT" -maxdepth 2 -name "docker-compose*.y*ml" \
          -not -path "*/.claude/*" -not -path "*/build*" 2>/dev/null \
        | grep -v "^$SRC_ROOT/docker-compose.yml$" || true)
if [ -n "$extra" ]; then
    echo "FAIL: additional docker-compose entry point(s) — Docker is not a runtime:"
    echo "$extra" | sed 's/^/  /'
    status=1
else
    echo "  OK: no new docker-compose runtime entry points"
fi

# --- 3. No silent userspace fallback for executive facilities ----------
# The executive lives in vms.ko behind /dev/vms. When it is absent the
# correct behavior is an honest error, never a per-process fake that
# reports success. Flag code that branches on /dev/vms absence toward a
# "local"/"fallback"/"emulate" path.
# Comment lines are excluded: documenting the policy is exactly right and must
# not trip the gate. A real fallback is executable code, which does not start
# with a comment marker.
#
# THE SS$_NOSUCHDEV EXEMPTION IS GONE (2026-07-29). This check used to skip any
# file that merely CONTAINED the string "SS$_NOSUCHDEV" anywhere -- so a file
# could carry a genuine fallback and be waved through because some unrelated
# line elsewhere in it mentioned the constant. Worse, the exemption encoded the
# superseded rule: "fail honestly with SS$_NOSUCHDEV" is no longer the correct
# behaviour for an absent executive (see the header). There is nothing left to
# exempt.
hits=$(grep -rnE '(no|without|missing|absent).{0,24}/dev/vms|/dev/vms.{0,40}(fall ?back|fallback|emulate|in-process|userspace fake)' \
        --include=*.c --include=*.h "$SRC_ROOT/src" 2>/dev/null \
       | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' || true)
if [ -n "$hits" ]; then
    echo "FAIL: possible silent userspace fallback for an executive facility:"
    echo "$hits" | sed 's/^/  /'
    echo "  -> the executive is integral; make its absence unreachable, do not"
    echo "     handle it. See CLAUDE.md Rule 9 and docs/design-executive-retrofit.md."
    status=1
else
    echo "  OK: no silent /dev/vms fallback path detected"
fi

# --- structural helpers for check 3b ------------------------------------
# Check 3b is the only check here that has to reason about CONTROL FLOW, so it
# gets a small structural reader rather than more greps. Everything below is
# character-level: brace/paren depth, statement boundaries, callee identity.
# Three rounds of this check were evaded by token searches; a token search
# cannot express "the halt is the last reachable statement".

# strip_comments: remove /* */ and // comments from stdin.
# Prose about halting must never stand in for halting, and a `}` or `;` inside
# a comment must not desynchronise the statement reader below.
strip_comments() {
    awk '
        {
            line = $0; out = ""; i = 1
            while (i <= length(line)) {
                if (inblk) {
                    p = index(substr(line, i), "*/")
                    if (p == 0) { i = length(line) + 1 } else { inblk = 0; i = i + p + 1 }
                    continue
                }
                two = substr(line, i, 2)
                if (two == "/*") { inblk = 1; i += 2; out = out " "; continue }
                if (two == "//") { break }
                out = out substr(line, i, 1); i++
            }
            print out
        }
    '
}

# func_body <file> <name>: the definition of <name>, comments stripped.
# A call site ends in `;` and is skipped, so the first hit is the definition.
func_body() {
    strip_comments < "$1" | awk -v fn="$2" '
        BEGIN { re = "(^|[^A-Za-z0-9_])" fn "[ \t]*\\(" }
        !in_func && $0 ~ re && $0 !~ /;[ \t]*$/ && $0 !~ /^[ \t]*#/ { in_func = 1 }
        in_func {
            print
            o = gsub(/{/, "{"); c = gsub(/}/, "}")
            if (o > 0) started = 1
            depth += o - c
            if (started && depth <= 0) exit
        }
    '
}

# last_top_stmt: read a block (or a single bare statement) on stdin and print
# its LAST TOP-LEVEL statement, normalised to one line with string literals
# blanked. "Top-level" means depth 0 relative to the block's own braces, so a
# statement nested inside an `if` is NOT a top-level statement of the block --
# which is exactly what makes a conditional halt detectable.
last_top_stmt() {
    sed -E 's/"([^"\\]|\\.)*"/""/g' | awk '
        { s = s $0 " " }
        END {
            p = 0; open_at = 0
            for (i = 1; i <= length(s); i++) {
                ch = substr(s, i, 1)
                if (ch == "(") p++
                else if (ch == ")") p--
                else if (p == 0 && ch == "{") { open_at = i; break }
                else if (p == 0 && ch == ";") { break }
            }
            if (open_at > 0) {
                d = 0
                for (i = open_at; i <= length(s); i++) {
                    ch = substr(s, i, 1)
                    if (ch == "{") d++
                    else if (ch == "}") { d--; if (d == 0) break }
                }
                body = substr(s, open_at + 1, i - open_at - 1)
            } else body = s

            n = 0; cur = ""; p = 0; d = 0
            for (i = 1; i <= length(body); i++) {
                ch = substr(body, i, 1)
                cur = cur ch
                if (ch == "(") p++
                else if (ch == ")") p--
                else if (ch == "{") d++
                else if (ch == "}") { d--; if (d == 0) { n++; stmt[n] = cur; cur = "" } }
                else if (ch == ";" && p == 0 && d == 0) { n++; stmt[n] = cur; cur = "" }
            }
            if (cur ~ /[^ \t]/) { n++; stmt[n] = cur }
            for (j = n; j >= 1; j--) {
                t = stmt[j]
                gsub(/^[ \t;]+/, "", t); gsub(/[ \t;]+$/, "", t)
                if (t != "") { print t; exit }
            }
        }
    '
}

# bare_call_name: print the callee if stdin is EXACTLY one call expression
# `NAME(...)` -- nothing before it, nothing after the matching `)`. An `if`,
# a ternary, an `&&`/`||` guard or a trailing statement all fail this, so the
# call it names is unconditional within its block.
bare_call_name() {
    awk '
        { s = s $0 " " }
        END {
            gsub(/^[ \t]+/, "", s); gsub(/[ \t]+$/, "", s)
            if (!match(s, /^[A-Za-z_][A-Za-z0-9_]*[ \t]*\(/)) exit
            name = substr(s, 1, RLENGTH); sub(/[ \t]*\($/, "", name)
            if (name ~ /^(if|for|while|switch|do|return|goto|else|sizeof)$/) exit
            p = 0
            for (i = RLENGTH; i <= length(s); i++) {
                ch = substr(s, i, 1)
                if (ch == "(") p++
                else if (ch == ")") { p--; if (p == 0) break }
            }
            rest = substr(s, i + 1); gsub(/[ \t;]/, "", rest)
            if (rest != "") exit
            print name
        }
    '
}

# The recognised halt entry points, and the primitives that actually end the
# process. HALT_ENTRIES pins the callee by NAME; terminates() then re-derives
# FROM THE SOURCE that the named function really ends the system, so a
# same-named non-halting stub cannot satisfy the check either.
HALT_ENTRIES="execinit_halt ovmx_exec_halt halt_now _exit exit abort reboot"
PRIMITIVE_TERMINATORS="_exit exit abort reboot"

in_set() {
    case " $2 " in *" $1 "*) return 0 ;; esac
    return 1
}

# terminates <file> <name>: follow the tail call chain (halt wrapper ->
# halt_now -> _exit/reboot) and succeed only if it bottoms out in a primitive
# that does not return.
terminates() {
    tn="$2"
    ti=0
    while [ "$ti" -lt 5 ]; do
        if in_set "$tn" "$PRIMITIVE_TERMINATORS"; then return 0; fi
        tb=$(func_body "$1" "$tn")
        [ -n "$tb" ] || return 1
        # A halt wrapper must have NO path back to its caller, so it may not
        # contain a `return` at all. Otherwise the escape hatch just moves one
        # frame down: `if (getenv("OVMX_ALLOW_NO_EXEC")) return;` ahead of the
        # terminator leaves the last statement looking perfectly terminal.
        if printf '%s\n' "$tb" \
                | grep -qE '(^|[^A-Za-z0-9_])return([^A-Za-z0-9_]|$)'; then
            return 1
        fi
        tn=$(printf '%s\n' "$tb" | last_top_stmt | bare_call_name)
        [ -n "$tn" ] || return 1
        ti=$((ti + 1))
    done
    return 1
}

# --- 3b. The executive must be INTEGRAL, not optional -------------------
# The guarantee that makes check 3 enforceable: PID 1 refuses to bring the
# system up unless the executive is reachable, and pins it for the life of
# the system. If this erodes, every per-call fallback we just deleted becomes
# necessary again -- so the gate holds the guarantee itself, not just its
# consequences.
#
# BEHAVIOURAL, NOT TOKEN-PRESENCE (2026-07-30, vms-a35 round 2). The original
# version of this check only asked whether the strings "executive_attach",
# "execinit_halt" and "/dev/vms" appeared ANYWHERE in ovmx_init.c -- so
# executive_attach() could be reduced to a no-op and the check would still
# pass as long as those three tokens survived somewhere in the file.
#
# ROUND 3 CORRECTION, and the reason this comment is long. The round-2 rewrite
# was STILL satisfiable by something other than the behaviour under test: it
# asked whether a halt/exit/reboot token appeared anywhere in the FUNCTION, and
# the module-load-ENOENT branch above contains one. So replacing the /dev/vms
# halt with `fprintf("...continuing without executive..."); return;` -- verbatim
# the silent fallback Rule 9 exists to forbid -- left this check printing
# "OK: ... halts on failure" over code that boots straight past the missing
# executive to a login prompt. A gate that certifies the regression it is
# guarding against is worse than no gate. The halt must therefore be located
# INSIDE THE FAILURE BRANCH FOR THE OPEN, not merely somewhere in the function.
#
# The properties, checked structurally against the extracted body of
# executive_attach(). Each has its own minimal negative control in
# tests/integration/test_runtime_target_negctl.sh that trips it AND NO OTHER --
# a mutation that trips several properties at once proves nothing about any
# single one of them, which is how the round-2 hole survived its own "proof".
#   (a) it opens "/dev/vms" and captures the descriptor in a variable;
#   (b) there is an `if (<that variable> < 0)` / `== -1` failure branch;
#   (c) a halt/exit/reboot call appears INSIDE that failure branch;
#   (d) no return/goto precedes the halt inside that branch -- a warn-then-
#       return leaves the caller running, which is the whole defect;
#   (e) the descriptor is never closed inside the function -- a
#       probe-and-release is not a pin;
#   (f) the branch is TERMINAL: its LAST TOP-LEVEL statement is an
#       unconditional call to a recognised halt entry point (see below);
#   (g) that entry point really ends the system, re-derived from the source;
#   (h) none of the pinned halt names is redefined by a macro.
#
# BOUNDARY OF THIS CHECK, stated so nobody reads more into its OK line than it
# says. It asserts things about the FUNCTION executive_attach(), not about PID
# 1's call graph: a tree in which executive_attach() is never called still
# passes 3b, and I confirmed that by execution rather than assuming it. That
# gap is covered BEHAVIOURALLY, not here -- tests/qemu/test_executive_integral.sh
# Boot B (vms.ko absent) and Boot C (module loads, /dev/vms never appears) both
# boot the real image and assert the halt line appears and `Username:` does not,
# so an uncalled executive_attach() fails CI there. Closing it in this lint
# would need the same statement-level machinery applied to main(); a weaker
# "is the name mentioned somewhere" version is the exact defect class this file
# has been fixed for three rounds running, so it is not added here.
#
# ROUND 4 CORRECTION, and why (f)/(g) exist. (c) and (d) together are a
# TOKEN-ORDER check, not a terminality check: (d) only compares the byte offset
# of the first return/goto against the first halt token. A halt with a
# reachable path OUT of the branch still satisfied both, so THREE one-hunk
# mutations were certified with "OK: ... halts inside the open-failure branch":
#     if (getenv("OVMX_ALLOW_NO_EXEC") == NULL) ovmx_exec_halt(...);
#     fprintf(stderr, "%OVMX-W-EXEC, continuing without executive\n");
#         -- Rule 9's warn-and-continue AND Rule 10's env-var bridge at once;
#     if (errno != ENODEV) ovmx_exec_halt(...);
#         -- falls through on one errno;
#     ovmx_exec_halt_reason(...);
#         -- (c)'s regex matched any identifier containing "halt"; nothing
#            checked WHICH function was called or whether it halts.
# All three boot PID 1 to a login prompt with no executive. The property the
# check actually needs is that the failure branch CANNOT RETURN TO THE CALLER
# AT ALL, so (f) requires the halt to be the branch's last top-level statement
# -- nothing after it, nothing enclosing it -- and (g) pins the callee.
# Each of the three, plus a gutted `ovmx_exec_halt` body and a halt followed by
# one more statement, is a recorded control in test_runtime_target_negctl.sh.
init_c="$SRC_ROOT/src/ovmx_init/ovmx_init.c"
if [ ! -f "$init_c" ]; then
    echo "FAIL: $init_c is missing -- the boot-time executive guarantee lives there"
    status=1
else
    body=$(func_body "$init_c" executive_attach)

    if [ -z "$body" ]; then
        echo "FAIL: could not locate executive_attach() in ovmx_init.c"
        status=1
    else
        reason=""

        # (a) the descriptor is captured
        openvar=$(printf '%s\n' "$body" \
            | grep -oE '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*open[[:space:]]*\([[:space:]]*"/dev/vms"' \
            | head -1 | sed -E 's/[[:space:]]*=.*//')

        if [ -z "$openvar" ]; then
            reason="$reason
    - no open(\"/dev/vms\") result is captured into a variable"
        else
            # (b) extract the FAILURE BRANCH for that variable: everything
            # controlled by `if (<openvar> < 0)` / `== -1`, whether braced or a
            # single statement. Character-level brace/semicolon tracking, so a
            # halt sitting in an unrelated branch elsewhere in the function
            # cannot satisfy (c).
            branch=$(printf '%s\n' "$body" | awk -v v="$openvar" '
                function feed(t,   i, ch) {
                    for (i = 1; i <= length(t); i++) {
                        ch = substr(t, i, 1)
                        out = out ch
                        if (ch == "{") { seen = 1; depth++ }
                        else if (ch == "}") {
                            depth--
                            if (seen && depth <= 0) { done = 1; return }
                        }
                        else if (ch == ";" && seen == 0 && depth == 0) { done = 1; return }
                    }
                    out = out "\n"
                }
                BEGIN {
                    re = "if[ \t]*\\([ \t]*" v "[ \t]*(<[ \t]*0|==[ \t]*-1)[ \t]*\\)"
                    found = 0; done = 0; depth = 0; seen = 0; out = ""
                }
                done { next }
                !found { if (match($0, re)) { found = 1; feed(substr($0, RSTART + RLENGTH)) } next }
                { feed($0) }
                END { if (found) print out }
            ')

            if [ -z "$branch" ]; then
                reason="$reason
    - the open() result ($openvar) has no \`if ($openvar < 0)\` failure branch"
            else
                # $body is already comment-free (func_body strips them), so
                # prose about halting cannot stand in for halting.
                code=$(printf '%s\n' "$branch" | tr '\n' ' ')
                offsets=$(printf '%s\n' "$code" | awk '{
                    h = match($0, /[A-Za-z_]*halt[A-Za-z_]*[ \t]*\(|reboot[ \t]*\(|exit[ \t]*\(|abort[ \t]*\(/)
                    e = match($0, /(^|[^A-Za-z0-9_])(return|goto)([^A-Za-z0-9_]|$)/)
                    print h, e
                }')
                halt_at=$(printf '%s' "$offsets" | cut -d' ' -f1)
                esc_at=$(printf '%s' "$offsets" | cut -d' ' -f2)

                # (c) the halt is in the failure branch
                if [ "${halt_at:-0}" -eq 0 ]; then
                    reason="$reason
    - the /dev/vms failure branch does not halt: no halt/exit/reboot call
      inside \`if ($openvar < 0)\`. Warn-and-continue is the silent fallback
      Rule 9 forbids -- PID 1 must not survive a missing executive."
                # (d) nothing escapes to the caller before it
                elif [ "${esc_at:-0}" -ne 0 ] && [ "$esc_at" -lt "$halt_at" ]; then
                    reason="$reason
    - the /dev/vms failure branch returns to the caller before it halts, so
      the halt is unreachable and PID 1 continues without an executive."
                else
                    # (f) TERMINALITY. The last top-level statement of the
                    # branch must be a bare, unconditional call to a recognised
                    # halt entry point. A conditional halt, a halt followed by
                    # anything, or a call to some other function all leave a
                    # path back to the caller.
                    last=$(printf '%s\n' "$branch" | last_top_stmt)
                    callee=$(printf '%s\n' "$last" | bare_call_name)
                    if [ -z "$callee" ] || ! in_set "$callee" "$HALT_ENTRIES"; then
                        reason="$reason
    - the /dev/vms failure branch does not END in an unconditional halt: its
      last top-level statement is
          $last
      Recognised halt entry points: $HALT_ENTRIES.
      A halt that is nested in a condition, or followed by another statement,
      leaves a path out of the branch -- PID 1 survives a missing executive."
                    # (g) and the entry point must really end the system, so a
                    # same-named stub that only prints cannot satisfy (f).
                    elif ! terminates "$init_c" "$callee"; then
                        reason="$reason
    - the /dev/vms failure branch calls $callee(), but $callee() no longer
      terminates the system: its call chain does not bottom out in one of
      $PRIMITIVE_TERMINATORS. A halt that returns is not a halt."
                    fi
                fi
            fi

            # (h) the pinned names are reserved. Pinning the callee by name is
            # only worth anything if the name still means the function the
            # gate inspected -- `#define ovmx_exec_halt(w, d) fprintf(...)`
            # would otherwise turn the check into a spelling test.
            halt_alt=$(printf '%s' "$HALT_ENTRIES" | tr ' ' '|')
            shadow=$(strip_comments < "$init_c" \
                     | grep -nE "^[[:space:]]*#[[:space:]]*define[[:space:]]+($halt_alt)[[:space:](]" || true)
            if [ -n "$shadow" ]; then
                reason="$reason
    - a halt entry point is REDEFINED by a macro in ovmx_init.c, so the pinned
      name no longer names the function this gate inspected:
$(printf '%s\n' "$shadow" | sed 's/^/          /')"
            fi

            # (e) the descriptor is pinned, not probed and released
            if printf '%s\n' "$body" \
                    | grep -qE "\\bclose[[:space:]]*\\([[:space:]]*${openvar}[[:space:]]*\\)"; then
                reason="$reason
    - the /dev/vms descriptor ($openvar) is closed inside executive_attach() -- not pinned"
            fi
        fi

        if [ -n "$reason" ]; then
            echo "FAIL: executive_attach() no longer establishes the executive guarantee:$reason"
            echo "  -> PID 1 must refuse to boot without /dev/vms and hold it open"
            echo "     for the life of the system (pins vms.ko: rmmod -> EBUSY)."
            status=1
        else
            echo "  OK: executive_attach() opens /dev/vms; the open-failure branch is terminal"
            echo "      (ends in an unconditional call to a halt that reaches $PRIMITIVE_TERMINATORS)"
            echo "      and the descriptor is never closed"
        fi
    fi
fi

# --- 3c. No per-call "is the executive there?" test --------------------
# Opening /dev/vms is unconditional; branching on whether it succeeded is the
# reintroduction of the deleted fallback, whatever status it returns and
# however indirectly the branch is reached. Two passes:
#   (i)  the return value is used inline, e.g. `vms_kif_open() < 0`,
#        `if (vms_kif_open())`, `switch (vms_kif_open())`.
#   (ii) the return value is captured into a variable first and THAT variable
#        is consumed as a presence value anywhere later in the file.
#
# ROUND 3 CORRECTION. Pass (ii) previously recognised only `if`/`while` as
# branch contexts, and its comparison alternative could not match `>=`/`<=` AT
# ALL (the `>` matched, the space class matched empty, then the digit class was
# asked to match the `=`). Two evasions went straight through it, both printing
# "OK: no per-call executive-presence test":
#     int rc = vms_kif_open(); return rc >= 0 ? 0 : -1;   /* the DELETED body */
#     int rc = vms_kif_open(); switch (rc) { case -1: ... }
# The first is verbatim the ensure_kif_open() body this check's own comment
# cites as flagged, merely hoisted into a variable. So: the operator set is now
# <, >, <=, >=, ==, !=, and the branch contexts are if/while/switch, the
# ternary, &&, ||, ! and return. All three shapes (plus round 1's `if (rc < 0)`)
# are recorded as negative controls in test_runtime_target_negctl.sh.
#
# Returning the captured value at all counts: handing a caller the executive's
# open-status IS the fallback, one frame further out. The two shapes that
# remain legal are the ones in the tree today:
#     (void)vms_kif_open();      /* discard -- the result is not a decision */
#     vms_kif_open();
direct=$(grep -rnE 'vms_kif_open[[:space:]]*\([[:space:]]*\)[[:space:]]*(<|>|=|!|\?|&|\|)|[!][[:space:]]*vms_kif_open[[:space:]]*\(|(if|while|switch)[[:space:]]*\([^)]*vms_kif_open|\breturn\b[^;]*vms_kif_open[[:space:]]*\([[:space:]]*\)' \
         --include=*.c --include=*.h "$SRC_ROOT/src" 2>/dev/null \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' || true)

#
# ROUND 4 CORRECTION. Pass (ii) ended with `| grep -vE '=[[:space:]]*vms_kif_open'`,
# meant to skip the assignment itself -- but it dropped the WHOLE LINE, so any
# line carrying BOTH the capture and the use was invisible. All three shapes
# above walk straight through it when written on one line:
#     int rc = vms_kif_open(); if (rc < 0) { }
#     int rc = vms_kif_open(); (void)(rc >= 0 ? 0 : -1);
#     int rc = vms_kif_open(); switch (rc) { case -1: break; default: break; }
# Round 3 fixed the multi-line FORMATTING of the class, not the class. The
# exclusion is now textual, not line-level: only the `<x> = vms_kif_open()`
# fragment is blanked, and what remains of the line is still scanned. The
# one-line form of each shape is a recorded control in the negctl suite.
indirect=""
for f in $(grep -rlE 'vms_kif_open[[:space:]]*\(' --include=*.c --include=*.h "$SRC_ROOT/src" 2>/dev/null); do
    vars=$(grep -oE '\b[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*vms_kif_open[[:space:]]*\([[:space:]]*\)' "$f" \
            | sed -E 's/[[:space:]]*=.*//' | sort -u)
    [ -n "$vars" ] || continue
    # Line-numbered file with ONLY the capture fragments blanked out.
    scan=$(grep -n '' "$f" \
           | sed -E 's/[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*vms_kif_open[[:space:]]*\([[:space:]]*\)//g')
    for v in $vars; do
        [ -n "$v" ] || continue
        hit=$(printf '%s\n' "$scan" \
                | grep -E "(if|while|switch)[[:space:]]*\\([^)]*\\b${v}\\b|\\b${v}\\b[[:space:]]*(<=|>=|==|!=|<|>)|[0-9)][[:space:]]*(<=|>=|==|!=|<|>)[[:space:]]*\\b${v}\\b|\\b${v}\\b[[:space:]]*(&&|\\|\\||\\?)|(&&|\\|\\|)[[:space:]]*[!]?[[:space:]]*\\b${v}\\b|[!][[:space:]]*\\b${v}\\b|\\breturn\\b[^;]*\\b${v}\\b" \
                | grep -vE '^[0-9]+:[[:space:]]*(\*|//|/\*)' || true)
        if [ -n "$hit" ]; then
            indirect="$indirect
$f: variable '$v' captures vms_kif_open() and is consumed as a presence value later:
$hit"
        fi
    done
done

probe="$direct$indirect"
if [ -n "$probe" ]; then
    echo "FAIL: code branches on whether the executive could be opened:"
    echo "$probe" | sed 's/^/  /'
    echo "  -> that condition is unreachable by construction (PID 1 guarantees"
    echo "     it). Do not handle it; open unconditionally."
    status=1
else
    echo "  OK: no per-call executive-presence test"
fi

# --- 4. Rule 9 must still be in CLAUDE.md ------------------------------
# The gate enforces the mechanics; CLAUDE.md carries the reasoning. If the
# rule is deleted, the gate is cargo cult -- fail loudly rather than drift.
if grep -q "One runtime target: the kernel/QEMU path" "$SRC_ROOT/CLAUDE.md" 2>/dev/null; then
    echo "  OK: CLAUDE.md still carries Rule 9"
else
    echo "FAIL: CLAUDE.md no longer carries Rule 9 (one runtime target)"
    echo "  -> this gate enforces a rule that must stay written down."
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "Rule 9 gate: PASS"
else
    echo "Rule 9 gate: FAIL"
fi
exit "$status"
