#!/bin/sh
#
# test_creprc_session_primitive.sh -- $CREPRC is the SOLE session-creation
# primitive, and no Linux process/terminal mechanics survive ABOVE the VMS
# layer (rd vms-3e9; design record docs/design/faithful-sessions-and-network-
# subsystems.md §3.1 / §6-P1, ratification gate §7.1).
#
# WHAT THIS GATE IS FOR. The design's first ratification gate is a NEGATIVE
# property of the CALLERS, and negative properties rot silently: the fastest
# way to "fix" a session bug is to fork+execl the login image right there in
# the daemon that has the connection, which is exactly how OVMX ended up with
# three independent login paths (console fork+execl, vmssshd's inline
# reimplementation, decnetd's unauthenticated execvp). One of them is now
# retired; this gate is what stops it coming back, and what will hold the SSH
# (P3) and DECnet CTERM (P4) callers to the same rule when they land.
#
# IT IS A SOURCE SCAN, AND THAT IS ALL IT IS. It cannot prove the runtime
# property -- that a real boot creates a real session through $CREPRC, that
# LOGINOUT re-personas it, and that the terminal is a real executive device.
# Those are proven end-to-end against the REAL booted image, on all three
# release arches including the VAX rail, by the session-primitive section of
# the shared acceptance battery (tests/qemu/lib/dcl_acceptance_battery.sh,
# "SESSION PRIMITIVE (vms-3e9)"), which logs a NON-SYSTEM SYSUAF account in at
# the console and reads its UIC, privileges and terminal back out of the
# executive from a different process. This file is the cheap, always-on half.
#
# THE FOUR CHECKS:
#   1. JOB_CONTROL (the console session creator) contains NO fork/execl/
#      execv*/openpty/forkpty/dup2/setsid/posix_spawn, and no direct exec of
#      LOGINOUT.EXE. It creates sessions with sys$creprc + PRC$M_INTER |
#      PRC$M_LOGINOUT and nothing else.
#   2. PID 1 (STARTUP.EXE) still contains no login loop of its own (the
#      vms-8d2 ownership property, restated here for the caller set).
#   3. The mechanics DID move rather than merely vanish: $CREPRC's
#      implementation carries the terminal-device binding (open + dup2 +
#      $ASSIGN + SETTERM) and the exec, under PRC$M_INTER / PRC$M_LOGINOUT.
#   4. THE WALL-6 ORDERING (vms-d4ef, design §A7): inside $CREPRC's
#      PRC$M_LOGINOUT path, vms_kif_establish_system() is issued BEFORE the
#      image is activated -- i.e. before the execl() that runs LOGINOUT and
#      therefore before LOGINOUT's SYSUAF read. On x86_64 a violation is
#      INVISIBLE (root maps to UIC group 0, which reads the World-denied
#      SYSUAF by luck of the environment); on the VAX rail it denies the read
#      RMS$_PRV and login stops working. A source-order check cannot replace
#      the VAX-rail run, but it fails the moment the ordering is edited, which
#      is faster than waiting for the rail to say so.
#
# Usage: test_creprc_session_primitive.sh [source-root]
# Exit 0 = all checks pass.

set -u

SRC_ROOT="${1:-$(cd "$(dirname "$0")/../.." && pwd)}"
status=0

JC_C="$SRC_ROOT/src/ovmx_job_control/ovmx_job_control.c"
INIT_C="$SRC_ROOT/src/ovmx_init/ovmx_init.c"
CREPRC_C="$SRC_ROOT/src/libvms/syssvc/sys_process.c"

echo "\$CREPRC session-primitive gate (vms-3e9): scanning the session callers"

for f in "$JC_C" "$INIT_C" "$CREPRC_C"; do
    if [ ! -f "$f" ]; then
        echo "FAIL: $f not found"
        status=1
    fi
done
[ "$status" -eq 0 ] || { echo "\$CREPRC session-primitive gate: FAILED"; exit 1; }

# --- 1. No Linux process/terminal mechanics in the console session creator ---
# Matched as CALLS ("name(") so a mention inside a comment -- and this file's
# own callers carry long comments explaining WHY these are gone -- is not a
# false positive. That is deliberate: the rule is about code, and a comment
# that explains the rule must not trip it.
for sym in fork execl execlp execv execvp execve openpty forkpty posix_spawn \
           dup2 setsid login_tty; do
    if grep -nE "(^|[^A-Za-z0-9_])${sym}[[:space:]]*\(" "$JC_C" \
        | grep -vE '^[0-9]+:[[:space:]]*\*' > /dev/null; then
        echo "FAIL: $JC_C calls ${sym}() -- Linux session mechanics are back"
        echo "      ABOVE the VMS layer. A session is created by \$CREPRC with"
        echo "      PRC\$M_INTER|PRC\$M_LOGINOUT; the ${sym}() belongs INSIDE"
        echo "      that service (src/libvms/syssvc/sys_process.c), never here."
        grep -nE "(^|[^A-Za-z0-9_])${sym}[[:space:]]*\(" "$JC_C" | sed 's/^/        /'
        status=1
    fi
done
if grep -qF 'LOGINOUT.EXE"' "$JC_C" && grep -qE 'exec[lv][a-z]*\(' "$JC_C"; then
    echo "FAIL: $JC_C execs LOGINOUT.EXE directly"
    status=1
fi
if grep -q 'sys\$creprc' "$JC_C" && grep -q 'PRC\$M_INTER' "$JC_C" \
   && grep -q 'PRC\$M_LOGINOUT' "$JC_C"; then
    echo "PASS: JOB_CONTROL creates its session with \$CREPRC PRC\$M_INTER|PRC\$M_LOGINOUT"
else
    echo "FAIL: $JC_C does not call sys\$creprc with PRC\$M_INTER|PRC\$M_LOGINOUT"
    status=1
fi
if [ "$status" -eq 0 ]; then
    echo "PASS: JOB_CONTROL carries no fork/exec*/openpty/dup2/setsid of its own"
fi

# --- 2. PID 1 still owns no login loop -------------------------------------
for sig in "consecutive_failures" "OVMX-E-NOLOGIN" "PRC\$M_INTER"; do
    if grep -qF -- "$sig" "$INIT_C"; then
        echo "FAIL: $INIT_C carries '$sig' -- the console login loop is back in PID 1"
        status=1
    fi
done

# --- 3. The mechanics moved INTO $CREPRC, they did not vanish --------------
# If they had merely been deleted, JOB_CONTROL would scan clean above and the
# product would have no way to start a session at all -- a gate that passes on
# a broken system is worse than no gate.
for need in 'PRC\$M_INTER' 'PRC\$M_LOGINOUT' 'creprc_bind_terminal' \
            'ovmx_console_terminal_path' 'vms_kif_assign' 'vms_kif_setterm' \
            'vms_kif_establish_system' 'execl(img_path'; do
    if grep -q -- "$need" "$CREPRC_C"; then
        :
    else
        echo "FAIL: $CREPRC_C is missing '$need' -- the session-creation"
        echo "      primitive did not receive the mechanics JOB_CONTROL gave up."
        status=1
    fi
done
if [ "$status" -eq 0 ]; then
    echo "PASS: \$CREPRC carries the terminal binding, the identity establishment and the exec"
fi

# --- 4. WALL-6: establish_system precedes image activation in $CREPRC ------
# The CALL, not the prose: this file documents the primitive at length, so a
# line whose first non-blank character is a comment '*' is skipped -- taking a
# doc mention as the call site would make the ordering check meaningless.
EST_LINE=$(grep -n 'vms_kif_establish_system()' "$CREPRC_C" \
    | grep -vE '^[0-9]+:[[:space:]]*\*' | head -1 | cut -d: -f1)
EXEC_LINE=$(grep -n 'execl(img_path' "$CREPRC_C" \
    | grep -vE '^[0-9]+:[[:space:]]*\*' | head -1 | cut -d: -f1)
if [ -n "$EST_LINE" ] && [ -n "$EXEC_LINE" ] && [ "$EST_LINE" -lt "$EXEC_LINE" ]; then
    echo "PASS: WALL-6 ordering holds -- vms_kif_establish_system() at line $EST_LINE precedes the image activation at line $EXEC_LINE (vms-d4ef; the VAX rail is the runtime proof)"
else
    echo "FAIL: WALL-6 ordering violated in $CREPRC_C"
    echo "      establish_system=${EST_LINE:-absent} image activation=${EXEC_LINE:-absent}"
    echo "  -> the identity must be on the created process's fresh PCB BEFORE"
    echo "     LOGINOUT reads the World-denied SYS\$SYSTEM:SYSUAF.DAT. x86_64"
    echo "     hides this (root -> UIC group 0 reads SYSUAF anyway); the VAX"
    echo "     rail denies the read RMS\$_PRV and console login stops working."
    status=1
fi

# --- 5. THE DECnet CTERM CALLER (P4, rd vms-f40) ----------------------------
# THIS IS THE HALF THAT WOULD HAVE CAUGHT THE MERGED HOLE. The first CTERM host
# cut answered an inbound connect to Session Control object 42 by openpty()ing a
# pty and fork()+execvp()ing `vmsdcl --login`: a remote $ SET HOST reached a BARE
# DCL PROMPT WITH NO AUTHENTICATION, and nothing in the tree said no. The rule
# above ("nothing above the VMS layer forks, execs, opens a pty, or dup2s") is
# what forbids it, and this is that rule applied to the DECnet caller.
#
# Two directions, because either alone can be satisfied by a broken system:
#   (a) NEGATIVE -- the daemon and the CTERM host carry no session mechanics and
#       no exec of a shell/DCL. In particular NO `vmsdcl` string with an exec.
#   (b) POSITIVE -- the CTERM host DOES create its session the one legal way:
#       $CREPRC with PRC$M_INTER|PRC$M_LOGINOUT, on a terminal it minted through
#       the executive. If (a) passed because the code was deleted rather than
#       moved, (b) fails and the gate still goes red.
DECNETD_C="$SRC_ROOT/src/vmsdecnet/engine/decnetd.c"
CTERM_HOST_C="$SRC_ROOT/src/vmsdecnet/cterm/dnet_cterm_host.c"
CTERM_C="$SRC_ROOT/src/vmsdecnet/cterm/dnet_cterm.c"

for f in "$DECNETD_C" "$CTERM_HOST_C" "$CTERM_C"; do
    if [ ! -f "$f" ]; then
        echo "FAIL: $f not found -- the DECnet CTERM caller scan cannot run"
        status=1
    fi
done

if [ "$status" -eq 0 ]; then
    for f in "$DECNETD_C" "$CTERM_HOST_C" "$CTERM_C"; do
        for sym in fork execl execlp execv execvp execve openpty forkpty \
                   posix_spawn login_tty setsid dup2; do
            if grep -nE "(^|[^A-Za-z0-9_])${sym}[[:space:]]*\(" "$f" \
                | grep -vE '^[0-9]+:[[:space:]]*\*' > /dev/null; then
                echo "FAIL: $f calls ${sym}() -- the DECnet CTERM no-auth login"
                echo "      path is back. An inbound SET HOST creates its session"
                echo "      with \$CREPRC PRC\$M_INTER|PRC\$M_LOGINOUT on an"
                echo "      executive-minted RTAn: (src/vmsdecnet/cterm/"
                echo "      dnet_cterm_host.c); the pty lives in"
                echo "      src/libvms/syssvc/sys_vterm.c, below the VMS layer."
                grep -nE "(^|[^A-Za-z0-9_])${sym}[[:space:]]*\(" "$f" | sed 's/^/        /'
                status=1
            fi
        done
        # The specific shape of the merged hole: an exec of DCL from the daemon.
        if grep -qE '"vmsdcl"|vmsdcl --login|--cterm-server[^"]*--login-command' "$f" \
           && grep -qE 'exec[lv][a-z]*\(' "$f"; then
            echo "FAIL: $f execs vmsdcl -- a remote SET HOST must reach LOGINOUT,"
            echo "      never a bare \$ prompt"
            status=1
        fi
    done
    if [ "$status" -eq 0 ]; then
        echo "PASS: the DECnet CTERM caller carries no fork/exec*/openpty of its own"
    fi
fi

# (b) the POSITIVE half -- the mechanics moved, they did not vanish.
#
# MATCHED AS CODE, NOT AS PROSE. dnet_cterm_host.c documents the hole it closes
# at length, so every token below appears in its comments too; searching the
# whole file would make this half pass on a file that merely TALKS about
# $CREPRC. Lines whose first non-blank character is a comment '*' are dropped
# first -- the same discipline check 4's ordering test uses, and it is
# load-bearing: without it, deleting the login path and leaving the comments
# still scored a PASS (measured while writing this gate).
# Drop whole-line comments, then strip single-line /* ... */ spans and any
# trailing //, so the trailing comment on an #include cannot satisfy a check
# either (also measured: `#include "prcdef.h" /* PRC$M_LOGINOUT */` did).
CTERM_HOST_CODE=$(grep -vE '^[[:space:]]*(\*|/\*|//)' "$CTERM_HOST_C" 2>/dev/null \
    | sed -e 's:/\*[^*]*\*/::g' -e 's://.*::')
for need in 'PRC\$M_INTER' 'PRC\$M_LOGINOUT' 'sys\$creprc(' 'ovmx_vterm_create(' \
            'VMS_LOGINOUT_PATH'; do
    if printf '%s\n' "$CTERM_HOST_CODE" | grep -q -- "$need"; then
        :
    else
        echo "FAIL: $CTERM_HOST_C has no CODE line carrying '$need' -- the inbound"
        echo "      SET HOST does not reach LOGINOUT through \$CREPRC on an"
        echo "      executive-minted RTAn:. A gate that passes because the login"
        echo "      path was deleted is worse than no gate."
        status=1
    fi
done

# (b-cont) The BOUNDED WIRE PARSE deliberately moved OUT of this privileged host
# path into the LOW-PRIVILEGE seam (dnet_cterm.c dnet_conn_descriptor_from_wire),
# per the A2/A8 isolation design (vms-515 §3.4): NETACP's privileged control path
# must NOT parse attacker-controlled bytes. So the parse-before-create invariant
# is now enforced in TWO places, and BOTH must hold or the gate goes red:
#   (i)  the bounded parse still lives in the low-priv seam (dnet_cterm.c), and
#   (ii) the privileged host path refuses an UNVALIDATED descriptor before it
#        creates any device/process (the validated-flag gate = parse-before-create).
# Losing (i) = an inbound connect reaches the privileged path unparsed; losing
# (ii) = a create-before-validate hole. Matched as CODE (comments stripped), the
# same discipline as the check above.
CTERM_CODE=$(grep -vE '^[[:space:]]*(\*|/\*|//)' "$CTERM_C" 2>/dev/null \
    | sed -e 's:/\*[^*]*\*/::g' -e 's://.*::')
if printf '%s\n' "$CTERM_CODE" | grep -q -- 'dnet_cterm_sc_connect_parse('; then
    :
else
    echo "FAIL: $CTERM_C has no CODE line carrying 'dnet_cterm_sc_connect_parse(' --"
    echo "      the low-privilege wire parse (the A2/A8 seam) is gone; an inbound"
    echo "      SET HOST would reach NETACP's privileged path without a bounded parse."
    status=1
fi
if printf '%s\n' "$CTERM_HOST_CODE" | grep -q -- '->validated'; then
    :
else
    echo "FAIL: $CTERM_HOST_C privileged open path does not gate on the descriptor's"
    echo "      validated flag -- parse-before-create is not enforced; an unvalidated"
    echo "      (unparsed) descriptor could create a session."
    status=1
fi

# The pty belongs to the virtual-terminal service, below the VMS layer -- so
# THAT file must have it. Same reasoning as check 3 for $CREPRC's mechanics.
VTERM_C="$SRC_ROOT/src/libvms/syssvc/sys_vterm.c"
if [ -f "$VTERM_C" ] && grep -q 'posix_openpt' "$VTERM_C" \
   && grep -q 'vms_kif_terminal_create' "$VTERM_C"; then
    echo "PASS: the virtual-terminal service owns the pty and mints RTAn: through the executive"
else
    echo "FAIL: $VTERM_C does not open the pty and mint the RTAn: through the"
    echo "      executive -- the mechanics the DECnet caller gave up have no home"
    status=1
fi

# --- NEGCTL: the scan is not vacuous ---------------------------------------
# Prove the search can actually find what it is looking for: JOB_CONTROL DOES
# contain the token 'sys$creprc', and does NOT contain a sentinel. If either
# half is wrong, every check above is searching an unreadable file and its
# PASSes mean nothing (the same discipline the acceptance battery's negctl
# applies to a console segment).
if grep -q 'sys\$creprc' "$JC_C" && ! grep -q 'ZZ_NOT_PRESENT_SENTINEL_ZZ' "$JC_C"; then
    echo "PASS: NEGCTL -- the scan finds a token known present and rejects one known absent"
else
    echo "FAIL: NEGCTL -- the scan over $JC_C is vacuous; the checks above cannot go red"
    status=1
fi

# NEGCTL for check 5, both of its directions, because both were measured wrong
# while this gate was written:
#   - the comment-stripped code view must still SEE a real call and must NOT see
#     a token that only appears in a comment. dnet_cterm_host.c mentions
#     openpty() in its prose (explaining why it does not call it), so if the
#     negative scan were a plain grep it would fire on the honest file, and if
#     the positive scan were a plain grep it would pass on a gutted one.
if printf '%s\n' "$CTERM_HOST_CODE" | grep -q 'sys\$creprc(' \
   && ! printf '%s\n' "$CTERM_HOST_CODE" | grep -q 'ZZ_NOT_PRESENT_SENTINEL_ZZ' \
   && grep -q 'openpty' "$CTERM_HOST_C" \
   && ! printf '%s\n' "$CTERM_HOST_CODE" | grep -qE '(^|[^A-Za-z0-9_])openpty[[:space:]]*\('; then
    echo "PASS: NEGCTL -- the CTERM-host code view finds a real \$CREPRC call, rejects"
    echo "      an absent sentinel, and correctly does NOT read the file's own"
    echo "      prose about openpty() as a call to it"
else
    echo "FAIL: NEGCTL -- the CTERM-host scan is vacuous or comment-blind; check 5"
    echo "      cannot be trusted in either direction"
    status=1
fi

if [ "$status" -eq 0 ]; then
    echo "\$CREPRC session-primitive gate: ALL CHECKS PASSED"
else
    echo "\$CREPRC session-primitive gate: FAILED"
fi
exit "$status"
