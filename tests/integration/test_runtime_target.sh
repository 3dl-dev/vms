#!/bin/sh
#
# test_runtime_target.sh - Rule 9 standing gate: ONE runtime MODEL
#
# Operator ruling 2026-07-28 (CLAUDE.md Project-Specific Rule 9): the Docker
# RUNTIME layer is dead. OVMX runs on a real host kernel that provides the VMS
# executive as an in-kernel facility reached through /dev/vms.
#
# Generalized 2026-08-12 (operator ratification, rd vms-fff / epic vms-8e8): the
# rule is substrate-neutral -- "one runtime MODEL", two sanctioned SYSKRNLs that
# expose the identical /dev/vms contract: OVMX/Linux (executive = vms.ko) and
# OVMX/NetBSD (executive = the vms pseudo-device, initially VAX). What the gate
# forbids is UNCHANGED: Docker is never a runtime on either, and a userspace fake
# of any executive facility (INV-6) is forbidden on either. Broadening WHICH
# kernels are sanctioned does not relax any check below.
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
#
# vms-28f (boot-plumbing seam). PID 1's boot syscalls moved behind
# src/ovmx_init/ovmx_boot.h so ONE ovmx_init.c serves every substrate with no
# #ifdef fork (INV-DRIFT). executive_attach() still lives in ovmx_init.c and
# still holds the GUARANTEE (halt-on-failure + pin), but now captures its
# descriptor from the seam's ovmx_boot_open_executive() instead of opening
# /dev/vms inline. Check 3b proves the guarantee (the POLICY, in ovmx_init.c);
# check 3b-backend proves the seam call really opens the executive device (the
# MECHANISM, in ovmx_boot_linux.c) so the seam cannot fake it. The Rule 9
# property is UNCHANGED; only which file each half is anchored to moved.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

echo "Rule 9 gate: one runtime model (real host kernel via /dev/vms); Docker is not a runtime"

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
#
# WORD BOUNDARIES on the absence words (\b...\b) -- not cosmetic. Without them
# the "no" alternative matches the substring inside mkNOd("/dev/vms"), so the
# LEGITIMATE, devfs-less-SYSKRNL node creation (ovmx_boot_netbsd.c, generalized
# Rule 9) tripped this check as a phantom "fallback". The absence words are
# WORDS ("no /dev/vms", "missing /dev/vms"), so anchoring them to word
# boundaries removes that false positive without weakening the intent -- and the
# node creation is not merely excused here, it is STRICTLY validated by
# check 3-node below (honest getdevmajor-guarded major, or the gate fails).
hits=$(grep -rnE '\b(no|without|missing|absent)\b.{0,24}/dev/vms|/dev/vms.{0,40}(fall ?back|fallback|emulate|in-process|userspace fake)' \
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

# --- 3-node. /dev/vms node creation is honest on a devfs-less SYSKRNL --------
# GENERALIZED Rule 9 (docs/runtime-target.md; docs/design-ovmx-netbsd-syskrnl.md):
# OVMX has exactly ONE executive, reached through /dev/vms, behind a PLUGGABLE
# SYSKRNL. On the Linux SYSKRNL the module + devtmpfs auto-create /dev/vms, so
# ANY userspace mknod of it is suspect -- a fabricated node is a fake executive
# (INV-6), which is why check 3 (above) flags absence-of-devfs prose near
# /dev/vms. On a devfs-LESS SYSKRNL (OVMX/NetBSD, where PID 1 == ovmx_init is
# init) NOTHING auto-creates the node, so PID 1 legitimately mknods it -- but
# ONLY for the REAL registered driver. This check ENCODES that authenticity
# condition; it is NOT an allowlist for the node-creation line (a fabricated
# major or an unguarded mknod still FAILS here). A mknod of /dev/vms is honest
# IFF, from the same source:
#   (a) the major is looked up from the loaded driver -- VAR = getdevmajor("vms");
#   (b) the mknod's device number is built from THAT VAR, never a hardcoded /
#       fabricated major; and
#   (c) the node is NOT created when the lookup fails -- a VAR == NODEVMAJOR
#       guard returns/halts before the mknod, so /dev/vms cannot exist without
#       the real executive (and executive_attach() -- check 3b -- then halts on
#       the open). Every property has an evasion recorded in the negctl.
node_files=$(grep -rlE 'mknod[[:space:]]*\([^;]*"/dev/vms"' --include=*.c "$SRC_ROOT/src" 2>/dev/null || true)
node_ok=1
for nf in $node_files; do
    nsc=$(strip_comments < "$nf")
    # (a) the real-driver lookup and the variable VAR it binds.
    nvar=$(printf '%s\n' "$nsc" \
        | sed -nE 's/.*[^A-Za-z0-9_]([A-Za-z_][A-Za-z0-9_]*)[[:space:]]*=[[:space:]]*getdevmajor[[:space:]]*\([[:space:]]*"vms".*/\1/p' \
        | head -1)
    if [ -z "$nvar" ]; then
        echo "FAIL: $nf: mknod(\"/dev/vms\") major is not derived from a getdevmajor(\"vms\") lookup"
        echo "  -> a userspace /dev/vms node must name the REAL registered driver (INV-6 / generalized Rule 9)."
        node_ok=0; status=1; continue
    fi
    # (b) every mknod("/dev/vms") device number must reference VAR -- reject a
    #     hardcoded/fabricated major.
    nbad=$(printf '%s\n' "$nsc" | grep -E 'mknod[[:space:]]*\([^;]*"/dev/vms"' \
           | sed -E 's/.*"\/dev\/vms"//' \
           | grep -vE "(^|[^A-Za-z0-9_])${nvar}([^A-Za-z0-9_]|$)" || true)
    if [ -n "$nbad" ]; then
        echo "FAIL: $nf: mknod(\"/dev/vms\") uses a hardcoded/fabricated major, not the getdevmajor(\"vms\") result ($nvar)"
        echo "  -> the node must carry the driver's REAL major; a fabricated major is a fake executive (INV-6)."
        node_ok=0; status=1; continue
    fi
    # (c) the honest-failure guard on VAR, and it must terminate before the mknod.
    nguard=$(printf '%s\n' "$nsc" | grep -nE "${nvar}[[:space:]]*==[[:space:]]*NODEVMAJOR|NODEVMAJOR[[:space:]]*==[[:space:]]*${nvar}" | head -1)
    if [ -z "$nguard" ]; then
        echo "FAIL: $nf: mknod(\"/dev/vms\") has no honest-failure guard ($nvar == NODEVMAJOR) -- the node could be created without the real driver"
        node_ok=0; status=1; continue
    fi
    if ! printf '%s\n' "$nsc" | grep -A1 -E "${nvar}[[:space:]]*==[[:space:]]*NODEVMAJOR|NODEVMAJOR[[:space:]]*==[[:space:]]*${nvar}" \
         | grep -qE 'return|_exit|exit[[:space:]]*\(|halt|reboot'; then
        echo "FAIL: $nf: the $nvar == NODEVMAJOR guard does not return/halt before the mknod -- the node is reachable on driver absence"
        node_ok=0; status=1; continue
    fi
done
if [ "$node_ok" -eq 1 ]; then
    if [ -n "$node_files" ]; then
        echo "  OK: every mknod(\"/dev/vms\") is guarded by a getdevmajor(\"vms\") lookup with an honest-failure on driver absence"
    else
        echo "  OK: no userspace /dev/vms node creation (Linux SYSKRNL: devtmpfs/module auto-creates it)"
    fi
fi

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
#   (a) it captures the executive descriptor from ovmx_boot_open_executive()
#       (the boot seam's executive-open; that the seam call really opens
#       /dev/vms is check 3b-backend) into a variable;
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

        # (a) the descriptor is captured. Since vms-28f, PID 1's boot syscalls
        # live behind the src/ovmx_init/ovmx_boot.h seam, so executive_attach()
        # captures the descriptor from the seam's ovmx_boot_open_executive()
        # rather than opening /dev/vms inline. That the seam call really opens
        # the executive device (and does not fake a descriptor) is check
        # 3b-backend below; here we only require the POLICY function to capture
        # it and (b)-(h) to prove its failure branch is terminal and pinned.
        openvar=$(printf '%s\n' "$body" \
            | grep -oE '[A-Za-z_][A-Za-z0-9_]*[[:space:]]*=[[:space:]]*ovmx_boot_open_executive[[:space:]]*\(' \
            | head -1 | sed -E 's/[[:space:]]*=.*//')

        if [ -z "$openvar" ]; then
            reason="$reason
    - no ovmx_boot_open_executive() result is captured into a variable"
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
            echo "  OK: executive_attach() captures ovmx_boot_open_executive(); the"
            echo "      open-failure branch is terminal (ends in an unconditional call"
            echo "      to a halt that reaches $PRIMITIVE_TERMINATORS) and the descriptor"
            echo "      is never closed"
        fi
    fi
fi

# --- 3b-backend. The executive-open SEAM CALL must really open /dev/vms -
# Since vms-28f, executive_attach() (check 3b) captures its descriptor from the
# boot seam's ovmx_boot_open_executive() rather than opening /dev/vms inline:
# PID 1's boot syscalls moved behind src/ovmx_init/ovmx_boot.h so ONE
# ovmx_init.c serves every substrate with no #ifdef fork (INV-DRIFT). Check 3b
# proves the POLICY half -- executive_attach()'s open-failure branch is terminal
# and the fd is pinned. THIS check proves the MECHANISM half on the active
# (Linux) backend: ovmx_boot_open_executive() in ovmx_boot_linux.c actually
# opens the executive device "/dev/vms". Without it the seam could satisfy 3b
# with a call that fabricates a descriptor and never touches the executive --
# PID 1 would then halt on nothing and boot straight past a missing executive,
# the exact silent fallback Rule 9 forbids (INV-6 holds on the backend too, not
# only in ovmx_init.c; the seam CONTRACT states this in ovmx_boot.h). The
# NetBSD backend gets its own device-path control here (vms-f2e).
boot_linux_c="$SRC_ROOT/src/ovmx_init/ovmx_boot_linux.c"
if [ ! -f "$boot_linux_c" ]; then
    echo "FAIL: $boot_linux_c is missing -- the Linux executive-open backend lives there"
    echo "  -> ovmx_boot_open_executive() must open /dev/vms (Rule 9 / vms-28f)."
    status=1
else
    open_body=$(func_body "$boot_linux_c" ovmx_boot_open_executive)
    if [ -z "$open_body" ]; then
        echo "FAIL: could not locate ovmx_boot_open_executive() in ovmx_boot_linux.c"
        echo "  -> the executive-open seam call must exist and open /dev/vms."
        status=1
    elif printf '%s\n' "$open_body" | grep -qE 'open[[:space:]]*\([[:space:]]*"/dev/vms"'; then
        echo "  OK: ovmx_boot_open_executive() opens the executive device /dev/vms"
    else
        echo "FAIL: ovmx_boot_open_executive() no longer opens /dev/vms"
        echo "  -> the boot seam's executive-open must open the executive device,"
        echo "     not fabricate a descriptor. executive_attach() halts PID 1 on"
        echo "     its failure (check 3b); a faked open would boot past a missing"
        echo "     executive on this substrate (INV-6 / Rule 9 / vms-28f)."
        status=1
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

# --- 5. The retired per-process fakes (vms-fk1) must not return --------
# PHASE 4 of the executive retrofit (docs/design-executive-retrofit.md) deleted
# five per-process userspace fakes, each of which reported success while
# sharing nothing across processes -- the INV-6 / Rule 11 facade class. Each
# now has an executive-backed replacement, proven against a REAL /dev/vms under
# the kernel-executive QEMU job (init.sh globs tests/qemu/test_syssvc_*):
#   event flags  -> test_syssvc_ef_mproc.c      (A sets a common flag, B sees it)
#   mailbox      -> test_syssvc_mbx_crossproc.c (A writes MBA1:, B reads it)
#   device table -> test_syssvc_getdvi.c        (executive I/O DB, not the host)
#   LNM$SYSTEM   -> test_syssvc_lnm_system.c    (A DEFINE/SYSTEM, B translates)
#   privileges   -> test_syssvc_setprv.c        (executive owns the grant)
# This section forbids their REINTRODUCTION. Every check has a minimal negative
# control in test_runtime_target_negctl.sh that trips it AND NO OTHER.
#
# Comment lines are excluded throughout: these files DOCUMENT what the fakes
# were ("...the old classify_device()+statvfs() host fake..."), and that prose
# must not trip the gate -- only executable code that resurrects the mechanism.

syssvc="$SRC_ROOT/src/libvms/syssvc"

# not_in_code <file> <ERE> -- non-comment matches in one file, empty if none.
not_in_code() {
    [ -f "$1" ] || return 0
    grep -nE "$2" "$1" 2>/dev/null \
        | grep -vE '^[0-9]+:[[:space:]]*(\*|//|/\*)' || true
}

# (a) EVENT FLAGS -- no per-process cluster STATE. The executive owns all 128
# flags (src/kernel/vms_eflag.c), reached via sys_event.c; the deleted fake
# kept a private copy of the four clusters in struct vms_pcb behind ef_lock /
# ef_cond. The LIB$ flag-NUMBER allocator (src/libvms/rtl/lib_eventflags.c:
# ef_bitmap/ef_lock) is per-process on VMS too and is NOT a fake -- so this
# bans the cluster-STATE fields (ef_clusters, ef_cond, PCB_EF_CLUSTERS) and a
# resurrected event_flags.c, never the generic allocator lock.
ef_hits=$(grep -rnE '\bef_clusters\b|\bef_cond\b|\bPCB_EF_CLUSTERS\b' \
            --include=*.c --include=*.h "$SRC_ROOT/src" 2>/dev/null \
          | grep -vE '^[^:]+:[0-9]+:[[:space:]]*(\*|//|/\*)' || true)
ef_file=$(find "$SRC_ROOT/src" -name 'event_flags.c' 2>/dev/null || true)
if [ -n "$ef_hits" ] || [ -n "$ef_file" ]; then
    echo "FAIL: per-process event-flag cluster state has returned:"
    [ -n "$ef_hits" ] && echo "$ef_hits" | sed 's/^/  /'
    [ -n "$ef_file" ] && echo "  resurrected file: $ef_file"
    echo "  -> event flags live in the executive (src/kernel/vms_eflag.c),"
    echo "     reached via sys_event.c. Do not keep a private copy in the PCB."
    status=1
else
    echo "  OK: no per-process event-flag cluster state (executive owns the flags)"
fi

# (b) MAILBOX -- no AF_UNIX socketpair mailbox. A mailbox is shared system
# state (vms.ko, src/kernel/vms_mbx.c); the deleted fake built a socketpair
# private to the creating process and named it MBA1:, so no unrelated process
# could ever open it. Scoped to sys_mailbox.c, the enumerated fake's home.
mbx_hits=$(not_in_code "$syssvc/sys_mailbox.c" 'socketpair[[:space:]]*\(')
if [ -n "$mbx_hits" ]; then
    echo "FAIL: sys_mailbox.c creates a private socketpair mailbox:"
    echo "$mbx_hits" | sed 's/^/  /'
    echo "  -> \$CREMBX is executive-resident (vms_kif_mbx_create); a socketpair"
    echo "     end is not a device an unrelated process can open (INV-6)."
    status=1
else
    echo "  OK: sys_mailbox.c has no private socketpair mailbox"
fi

# (c) DEVICE TABLE -- \$GETDVI / \$DEVICE_SCAN read the EXECUTIVE'S device table
# (src/kernel/vms_devtab.c), never the host. The deleted fake answered from
# classify_device()+statvfs()/termios and a compiled-in scan_devices[] table,
# reporting SS\$_NORMAL for devices the executive never heard of. (DCL's lexical
# F\$GETDVI in dcl_lexical.c legitimately uses statvfs for volume free-space, a
# documented vms-dv1 remainder -- so this is scoped to sys_device.c only.)
dev_hits=$(not_in_code "$syssvc/sys_device.c" '\bstatvfs\b|\bclassify_device\b|\bscan_devices\b')
if [ -n "$dev_hits" ]; then
    echo "FAIL: sys_device.c fabricates device state from the host:"
    echo "$dev_hits" | sed 's/^/  /'
    echo "  -> \$GETDVI/\$DEVICE_SCAN must read the executive device table"
    echo "     (vms_kif_getdvi_*/vms_kif_devscan), not statvfs()/a static list."
    status=1
else
    echo "  OK: sys_device.c reads the executive device table, not the host"
fi

# (d) LNM\$SYSTEM -- the public logical-name services must route the SYSTEM
# table through the executive (vms_kif_lnm_define/_translate), so a name one
# process defines is visible node-wide. The deleted fake served LNM\$SYSTEM from
# a process-private array (logical_table[]). PRESENCE check: a reintroduction
# replaces the executive call with local storage, removing the token.
# (LNM\$PROCESS staying local is VMS-correct and is deliberately not touched.)
log_c="$syssvc/sys_logical.c"
if [ -f "$log_c" ]; then
    have_def=$(not_in_code "$log_c" 'vms_kif_lnm_define')
    have_trn=$(not_in_code "$log_c" 'vms_kif_lnm_translate')
    if [ -z "$have_def" ] || [ -z "$have_trn" ]; then
        echo "FAIL: sys_logical.c no longer routes LNM\$SYSTEM through the executive"
        [ -z "$have_def" ] && echo "  -> no vms_kif_lnm_define call (SYSTEM create went local?)"
        [ -z "$have_trn" ] && echo "  -> no vms_kif_lnm_translate call (SYSTEM read went local?)"
        echo "     LNM\$SYSTEM is executive-resident (vms-d37); a process-private"
        echo "     SYSTEM table is exactly the fake this closed."
        status=1
    else
        echo "  OK: sys_logical.c routes LNM\$SYSTEM through the executive"
    fi
else
    echo "FAIL: $log_c is missing -- the logical-name services live there"
    status=1
fi

# (e) PRIVILEGES -- \$SETPRV is the executive's grant (vms_kif_setprv ->
# kernel/vms_access.c), authorized against this process's permanent mask. The
# deleted fake let a process award itself a privilege by writing pcb->cur_privs
# (the vms-b2e LARP). PRESENCE check: a reintroduction replaces the executive
# call with a local grant, removing the token. (The PCB masks are a read-back
# CACHE of the executive's answer, not the grant -- see sys_misc.c.)
misc_c="$syssvc/sys_misc.c"
if [ -f "$misc_c" ]; then
    have_setprv=$(not_in_code "$misc_c" 'vms_kif_setprv')
    if [ -z "$have_setprv" ]; then
        echo "FAIL: sys_misc.c no longer routes \$SETPRV through the executive"
        echo "  -> no vms_kif_setprv call: a process must not grant itself"
        echo "     privilege by writing pcb->cur_privs (INV-6 / vms-b2e)."
        status=1
    else
        echo "  OK: sys_misc.c routes \$SETPRV through the executive"
    fi
else
    echo "FAIL: $misc_c is missing -- \$SETPRV lives there"
    status=1
fi

# --- 4. Rule 9 must still be written down ------------------------------
# The gate enforces the mechanics; docs/runtime-target.md carries the
# reasoning. (It used to live in CLAUDE.md; that internal ops file was removed
# from the public repo, so the canonical statement moved to docs/.) If the rule
# is deleted, the gate is cargo cult -- fail loudly rather than drift.
if grep -q "One runtime model: the real-host-kernel path" "$SRC_ROOT/docs/runtime-target.md" 2>/dev/null; then
    echo "  OK: docs/runtime-target.md still carries Rule 9"
else
    echo "FAIL: docs/runtime-target.md no longer carries Rule 9 (one runtime model)"
    echo "  -> this gate enforces a rule that must stay written down."
    status=1
fi

# --- 6. Executive-boundary AUDIT findings (vms-617, Phase A: REPORTING ONLY) --
# The tracer (src/imgact/imgact_boundary_audit.c, armed behind
# OVMX_BOUNDARY_AUDIT=1) records every raw VMS-semantic syscall an activated
# image issues INSTEAD of routing through the executive -- the exact bypass this
# whole gate exists to make loud. Surface those findings here as a REPORTING
# input so they are visible alongside the Rule 9 checks. Phase A does NOT fail
# the build on findings (that ratchet is a later, deliberate step); it makes
# them visible and it surfaces the tracer's own overflow marker so nothing is
# silently dropped. This section never touches $status.
_ba_report="$(dirname "$0")/boundary_audit_report.sh"
if [ -x "$_ba_report" ]; then
    "$_ba_report" || true      # reporting-only: never affects the gate status
elif [ -f "$_ba_report" ]; then
    sh "$_ba_report" || true
fi

if [ "$status" -eq 0 ]; then
    echo "Rule 9 gate: PASS"
else
    echo "Rule 9 gate: FAIL"
fi
exit "$status"
