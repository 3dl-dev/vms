#!/bin/bash
# dcl_acceptance_battery.sh -- the SHARED DCL/SHOW acceptance battery.
#
# WHY THIS FILE EXISTS (co-release parity, no drift): x86_64 and Alpha are a
# single release stream (memory: vax-mainstream-corelease / alpha first-class).
# The boot-and-RUN-COMMANDS acceptance battery -- log in SYSTEM/MANAGER, run the
# basic DCL/SHOW commands a user types on first login, and ASSERT the output is
# VMS-faithful (project Rule 1), each assertion naming the bug it guards and
# carrying a negative control -- must be BYTE-IDENTICAL across arches, or the two
# runtimes silently diverge in what "acceptance" means. So the battery lives here
# ONCE and both arch drivers source it and call run_dcl_acceptance_battery.
#
# THE CALLER-PROVIDED CONTRACT. This file is arch-INDEPENDENT: it knows nothing
# about qemu-system-x86_64 vs qemu-system-alpha, docker, fifos, or console
# framing. Before sourcing and calling run_dcl_acceptance_battery, the caller
# MUST have defined these primitives and variables:
#
#   FUNCTIONS
#     send <str>              -- write <str> + a carriage return to the guest
#                                console (send '' feeds a bare CR, as a real
#                                operator hitting RETURN on OPA0:).
#     wait_for <pat> <secs> <since-byte>
#                             -- return 0 as soon as fixed-string <pat> appears
#                                in the console log at/after byte <since-byte>
#                                (default 0), else 1 after <secs> seconds or if
#                                the guest dies. Fixed-string (grep -F) semantics.
#     run_cmd <cmd>           -- send <cmd>, wait (bounded by CMD_TIMEOUT) for the
#                                returned DCL "$ " prompt, then set the GLOBAL var
#                                SEG to everything the command produced (its echo
#                                + output + trailing prompt), CR-stripped.
#
#   VARIABLES
#     LOG                     -- path to the live console log file (read directly
#                                for the boot banner + Username:/CR-feed loop).
#     EXPECTED_BOOT_BANNER    -- brand+version the boot must print, from
#                                ovmx_identity.h (INV-1 single source), e.g.
#                                "OpenVMX V0.5-7".
#     EXPECTED_COMPAT_VERSION -- the version F$GETSYI("VERSION") must report,
#                                true-to-arch (ovmx_compat_version()): the real
#                                VSI version on a lineage arch, else OVMX's own.
#     EXPECTED_ARCH_NAME      -- the arch token F$GETSYI("ARCH_NAME") must report,
#                                the gate's own build arch (ovmx_hw_arch()):
#                                "X86_64" / "AARCH64" / "VAX" / "Alpha".
#     VOLUME_LABEL            -- the mastered ODS-2 system-disk label (OVMXSYS).
#     SYSDEV                  -- optional; the system disk's VMS device name.
#                                Defaults to VDA0: (virtio arches: x86_64/aarch64/
#                                Alpha discover it as VDAn:). OVMX/NetBSD-vax
#                                faithfully names its MSCP system disk DUA0:
#                                (vms-9f5), so the VAX driver exports SYSDEV=DUA0:.
#     CMD_TIMEOUT             -- per-command bound run_cmd passes to wait_for.
#     PASS / FAIL             -- integer counters; ok/bad below increment them.
#                                Initialise PASS=0 FAIL=0 before calling.
#     BOOT_TIMEOUT            -- optional; CR-feed-to-Username bound (default 180).
#
# NO set -e. Like the original test_dcl_acceptance_e2e.sh, the assertion helpers
# below rely on grep exit codes inside if/&&; the caller must run this battery
# with `set +e` (errexit off) or the first "not found" grep would abort it.
#
# RETURN. run_dcl_acceptance_battery returns 0 once it has driven the runtime to
# an authenticated DCL prompt and run the full battery (individual assertions may
# still have recorded FAILs in the global FAIL -- that is the RED-until-fixed
# result, NOT a battery error). It returns 1 ONLY if the runtime never reached
# the Username: prompt or SYSTEM login failed -- a hard boot/login failure the
# caller should surface with a console dump.
#
# TWO SESSIONS, NOT ONE (vms-3e9). The battery's tail LOGS OUT of the SYSTEM
# session and logs back in as the NON-SYSTEM SYSUAF account GUEST, to prove the
# session-creation primitive ($CREPRC) and the re-persona (LOGINOUT) -- see the
# "SESSION PRIMITIVE (vms-3e9)" section at the bottom. Callers must therefore
# budget for a second login (a CR-feed-to-Username loop like the boot one) and
# must not assume the guest is still logged in as SYSTEM when this returns.

# --- assertion helpers (arch-independent; operate on a captured console SEGMENT)
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
# note: a LOUD, logged observation that does NOT touch PASS/FAIL -- for the vms-c38
# golden gate's REPORT-only surfaces (a not-yet-faithful surface's classification is
# tracked + routed to its fidelity item, but must not hard-fail the required leg;
# conductor ruling 2026-08-30, Option A). NEVER use it to silence a CLAIMED-FAITHFUL
# surface -- that would be the allowlist-cheat INV-6 forbids.
note() { echo "  NOTE: $1"; }
# must_have: a VMS-faithful substring MUST be present.
must_have() { local seg="$1" pat="$2" desc="$3"
    if printf '%s\n' "$seg" | grep -qiF -- "$pat"; then ok "$desc"
    else bad "$desc [expected substring: '$pat']"; fi; }
# must_match: a VMS-faithful regex MUST match.
must_match() { local seg="$1" re="$2" desc="$3"
    if printf '%s\n' "$seg" | grep -qiE -- "$re"; then ok "$desc"
    else bad "$desc [expected match: /$re/]"; fi; }
# must_not_have: a broken/fabricated bug marker MUST be absent (bug guard).
must_not_have() { local seg="$1" pat="$2" desc="$3"
    if printf '%s\n' "$seg" | grep -qiF -- "$pat"; then bad "$desc [found bug marker: '$pat']"
    else ok "$desc"; fi; }
# negctl: PROVES the search over THIS segment is not vacuous -- it must FIND a
# token known present (the echoed command) and REJECT a random sentinel known
# absent. If either half is wrong the segment is empty/unsearchable and every
# must_*/must_not_have above it cannot be trusted.
negctl() { local seg="$1" present="$2" desc="$3"
    local sentinel="ZZ_NEGCTRL_${$}_${RANDOM}${RANDOM}_ZZ"
    local a=1 b=1
    printf '%s\n' "$seg" | grep -qiF -- "$present" && a=0
    printf '%s\n' "$seg" | grep -qiF -- "$sentinel" && b=0
    if [ "$a" -eq 0 ] && [ "$b" -eq 1 ]; then
        ok "NEGCTL $desc: search over the real segment finds a present token ('$present') and rejects a bogus sentinel -- the assertions above can genuinely go red"
    else
        bad "NEGCTL $desc: search is vacuous (present-token found=$([ $a -eq 0 ] && echo yes || echo NO), sentinel rejected=$([ $b -eq 1 ] && echo yes || echo NO)) -- assertions above cannot be trusted"
    fi; }

# --- vms-c38: oracle golden-diff gate ------------------------------------------
# The OVMX side of the oracle program: capture_oracle captured the real-VMS layout
# golden; tools/oracle/diff_surface.sh applies the SAME NORMALIZE mask to OVMX's
# output + grounded MAY_OMIT (substrate-absent sections) and classifies MATCH /
# MISSING / HOLLOW / ARTIFICE-TELL / FORMAT-DIVERGENT. This UPGRADES the piecewise
# hand-written must_haves above to a continuous golden-diff for the seeded surfaces.
# The acceptance test runs in a Docker container where only the battery + test.sh
# are mounted (run_dcl_acceptance_e2e.sh), so the repo-relative path can't reach
# tools/oracle. run_dcl_acceptance_e2e.sh mounts the oracle tooling + goldens at a
# repo-root-like /oracle prefix and passes OVMX_ORACLE_DIR; the relative path is
# the fallback for a local (checked-out-tree) run.
_ORACLE_DIR="${OVMX_ORACLE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../../../tools/oracle" 2>/dev/null && pwd || true)}"
# _golden_run <surface> -> sets _GD_ACC (the surface's own commands' console) and
# _GD_CLS/_GD_RC (diff_surface classification of OVMX vs the golden). Shared by the
# hard-gate and report-only forms so both classify identically.
_golden_run() {
    local surface="$1" surf c; _GD_ACC=""; _GD_CLS=""; _GD_RC=99
    surf="$_ORACLE_DIR/surfaces/$surface.surface"
    if [ -z "$_ORACLE_DIR" ] || [ ! -f "$surf" ]; then return 1; fi
    local -a cmds
    mapfile -t cmds < <( ARCH=; NORMALIZE=; MAY_OMIT=; MACHINE_MASK=; ARTIFICE_TELL=; COMMANDS=(); . "$surf"; printf '%s\n' "${COMMANDS[@]}" )
    for c in "${cmds[@]}"; do run_cmd "$c"; _GD_ACC+="$SEG"$'\n'; done
    _GD_CLS="$(printf '%s' "$_GD_ACC" | "$_ORACLE_DIR/diff_surface.sh" "$surface" 2>&1)"; _GD_RC=$?
    return 0
}

# golden_diff <surface> -- HARD-GATE a CLAIMED-FAITHFUL surface (vms-c38, Option A).
# OVMX MUST MATCH the oracle golden (modulo grounded MAY_OMIT + machine-masks);
# a divergence is a real REGRESSION and FAILS the leg. Only surfaces proven
# genuinely faithful graduate here -- the gate then protects them from regressing.
golden_diff() {
    local surface="$1"
    if ! _golden_run "$surface"; then bad "GOLDEN-DIFF [$surface]: surface/tooling not found"; return; fi
    # GREENS: OVMX output MATCHes the oracle golden.
    if [ "$_GD_RC" -eq 0 ]; then ok "GOLDEN-DIFF [$surface] (vms-c38): OVMX MATCHes the oracle golden (CLAIMED-FAITHFUL, regression-gated)"
    else
        bad "GOLDEN-DIFF [$surface] (vms-c38): REGRESSION -- $(printf '%s' "$_GD_CLS" | grep -m1 -oE 'MISSING|HOLLOW|ARTIFICE-TELL|FORMAT-DIVERGENT' || echo diverges) vs the oracle golden (was faithful)"
        printf '%s\n' "$_GD_CLS" | sed 's/^/      DIFF| /' | head -60
    fi
    # REDS negctl: an injected divergence into the SAME output must NOT MATCH --
    # proves this golden gate can actually go red, not vacuously always-green.
    printf '%s\n  ZZ_GOLDEN_NEGCTL_%s_ZZ  9\n' "$_GD_ACC" "$RANDOM" | "$_ORACLE_DIR/diff_surface.sh" "$surface" >/dev/null 2>&1
    if [ $? -ne 0 ]; then ok "GOLDEN-DIFF-NEGCTL [$surface] (vms-c38): an injected divergence does NOT MATCH (the golden gate can fail)"
    else bad "GOLDEN-DIFF-NEGCTL [$surface] (vms-c38): injected divergence still MATCHed -- the golden gate is vacuous"; fi
}

# golden_diff_report <surface> <expected-class> <fidelity-item> -- REPORT-ONLY a
# NOT-yet-faithful surface (vms-c38, Option A). The gate CLASSIFIES OVMX vs the
# golden and logs it LOUDLY + routes it to its fidelity item, but does NOT hard-fail
# the required leg (so a known gap can't permanently-red main's green-by-SHA). This
# is the anti-LARP DRIVER, not an allowlist-cheat: it tracks + names the gap every
# run, never silently passes it. A surface GRADUATES to golden_diff (hard-gate) once
# <fidelity-item> lands and it genuinely MATCHes. If a report-only surface instead
# MATCHes NOW, that is loudly flagged as ready-to-graduate.
golden_diff_report() {
    local surface="$1" expect="$2" item="$3" got
    if ! _golden_run "$surface"; then note "GOLDEN-REPORT [$surface]: surface/tooling not found"; return; fi
    if [ "$_GD_RC" -eq 0 ]; then
        note "GOLDEN-REPORT [$surface] (vms-c38): now MATCHes the golden -- READY TO GRADUATE onto the hard-gate list ($item)"
        return
    fi
    got="$(printf '%s' "$_GD_CLS" | grep -m1 -oE 'MISSING|HOLLOW|ARTIFICE-TELL|FORMAT-DIVERGENT' || echo diverges)"
    note "GOLDEN-REPORT [$surface] (vms-c38): $got vs the oracle golden -- KNOWN GAP (expected $expect), tracked by $item (NOT gated; drives vms-050 fidelity backlog)"
    printf '%s\n' "$_GD_CLS" | sed 's/^/      RPT| /' | head -40
}

# _batt_seg_since -- everything the console printed at/after byte <n>, CR-stripped.
# Battery-LOCAL on purpose: `segment_since' is defined by only ONE of the three
# arch drivers (tests/qemu/test_dcl_acceptance_e2e.sh), so the shared battery may
# not call it -- doing so would work on x86_64 and blow up on Alpha and VAX,
# which is the exact class of arch asymmetry this section exists to catch.
_batt_seg_since() { tail -c "+$(($1 + 1))" "$LOG" 2>/dev/null | tr -d '\r'; }

# console_wait_quiet -- block until the console log has stopped growing for
# <quiet> consecutive seconds, or <budget> seconds have passed. Echoes the
# reason it returned ("quiet" / "budget").
#
# WHY A QUIET WINDOW rather than a fixed sleep: it is the only observable that
# says "LOGINOUT has reached its wake read and is now waiting for the operator"
# without asking the guest anything. A boot still printing is not yet at the
# prompt; a boot that has gone silent either IS at the wake or has died, and the
# assertions below distinguish those two by driving it.
console_wait_quiet() {
    local quiet="$1" budget="$2"
    local last=-1 cur stable=0 waited=0
    while [ "$waited" -lt "$budget" ]; do
        cur=$(wc -c <"$LOG" 2>/dev/null || echo 0)
        if [ "$cur" = "$last" ]; then stable=$((stable + 1)); else stable=0; last="$cur"; fi
        if [ "$stable" -ge "$quiet" ]; then echo quiet; return 0; fi
        sleep 1; waited=$((waited + 1))
    done
    echo budget
    return 0
}

# ---------------------------------------------------------------------------
# console_login_acceptance -- THE OPA0: LOGIN-SEQUENCE GATE (vms-3e9).
#
# WHY IT LIVES IN THE SHARED BATTERY. The console login sequence had NO
# arch-uniform coverage at all: the only gate that types at a booting console
# (tests/qemu/test_console_boot_no_newline_spam.sh) branches on `uname -m` over
# aarch64/x86_64 and cannot run on the VAX rail, which is under SIMH via anita,
# not bash-launchable QEMU. So every proof of the wake was written where it
# happened to work, and the operator found the VAX console printing "Username:"
# with no RETURN wait -- the classic x86_64-green/VAX-broken asymmetry. This
# battery is the ONE thing x86_64, Alpha and VAX all run
# (tests/qemu/test_dcl_acceptance_e2e.sh, tools/cross-alpha/run-boot-alpha.sh,
# tests/lab-vax/test_dcl_acceptance_vax.sh), so the sequence is asserted here
# and the VAX rail gets it for free -- and cannot lose it again silently.
#
# IT MUST RUN BEFORE ANY CR IS SENT. The whole gate turns on the console NOT
# having been touched: the caller's contract says the battery owns the operator
# keystrokes, and both the QEMU drivers and the VAX bridge send nothing of their
# own, so a "Username:" on screen here can only mean the wake did not happen.
console_login_acceptance() {
    local budget="${WAKE_QUIET_BUDGET:-$1}"
    local quiet="${WAKE_QUIET_SECS:-5}"
    local why; why=$(console_wait_quiet "$quiet" "$budget")

    # INV-1: both halves come from the boot banner the caller derived from
    # ovmx_identity.h -- "OpenVMX V0.6-11" -> product "OpenVMX", version "V0.6-11".
    local ID_PRODUCT="${EXPECTED_BOOT_BANNER%% *}"
    local ID_VERSION="${EXPECTED_BOOT_BANNER##* }"
    local ID_LINE="Welcome to the ${ID_PRODUCT}"

    # --- (b) THE WAKE: an untouched console must NOT be showing a prompt ----
    # This is the assertion that is RED on the VAX rail before the isatty()
    # gate is replaced by the executive's terminal binding, and green after.
    if grep -qaF 'Username:' "$LOG" 2>/dev/null; then
        bad "CONSOLE WAKE (vms-3e9 b): OPA0: printed 'Username:' with NO operator RETURN -- LOGINOUT did not wait for the wake keystroke on this arch (console went $why)"
    else
        ok "CONSOLE WAKE (vms-3e9 b): an untouched OPA0: shows no 'Username:' -- LOGINOUT is waiting for the operator's RETURN (console went $why)"
    fi
    if grep -qaF "$ID_LINE" "$LOG" 2>/dev/null; then
        bad "CONSOLE WAKE (vms-3e9 b): the login identification line printed before any RETURN was struck"
    else
        ok "CONSOLE WAKE (vms-3e9 b): no login identification line before the operator's RETURN"
    fi

    # --- ONE RETURN must produce the whole login announcement ---------------
    local WAKE_OFF; WAKE_OFF=$(wc -c <"$LOG")
    send ''
    if wait_for 'Username:' 60 "$WAKE_OFF"; then
        ok "CONSOLE WAKE (vms-3e9 b): ONE RETURN wakes the session and produces the 'Username:' prompt"
    else
        bad "CONSOLE WAKE (vms-3e9 b): the operator's RETURN did not produce a 'Username:' prompt within 60s"
        return 1
    fi

    local WAKE_SEG; WAKE_SEG=$(_batt_seg_since "$WAKE_OFF")

    # --- (a) THE SYSTEM-IDENTIFICATION LINE, immediately before the prompt --
    # Oracle: docs/design-boot-faithful.md sec3.5 -- identification, blank line,
    # "Username:". OVMX printed nothing here on ANY arch before vms-3e9.
    must_have "$WAKE_SEG" "$ID_LINE" \
        "LOGIN BANNER (vms-3e9 a): the console announces the system immediately before 'Username:' ('$ID_LINE ...')"
    must_have "$WAKE_SEG" "Version $ID_VERSION" \
        "LOGIN BANNER (vms-3e9 a): it carries the ovmx_identity.h version ('Version $ID_VERSION') -- INV-1, not a literal"
    # Ordering: the announcement precedes the prompt, as on the oracle console.
    local BPOS UPOS
    BPOS=$(printf '%s' "$WAKE_SEG" | grep -aboF "$ID_LINE" | head -1 | cut -d: -f1)
    UPOS=$(printf '%s' "$WAKE_SEG" | grep -aboF 'Username:' | head -1 | cut -d: -f1)
    if [ -n "$BPOS" ] && [ -n "$UPOS" ] && [ "$BPOS" -lt "$UPOS" ]; then
        ok "LOGIN BANNER (vms-3e9 a): announcement precedes the prompt (banner@$BPOS < prompt@$UPOS)"
    else
        bad "LOGIN BANNER (vms-3e9 a): announcement/prompt ordering wrong (banner@${BPOS:-none} prompt@${UPOS:-none})"
    fi
    # ANTI-HOLLOWING: this pre-login line must NOT be confusable with the
    # post-authentication SYS$WELCOME ("Welcome to <product> V..."), which is
    # what ~30 gates -- including this battery's own login step below -- use as
    # their proof that a login SUCCEEDED. If it ever matched, every one of them
    # would pass without a login. tests/tools/test_loginout_display.c pins the
    # same property on the emitter; this pins it on the real console.
    must_not_have "$WAKE_SEG" "Welcome to $ID_PRODUCT " \
        "LOGIN BANNER (vms-3e9 a): the pre-login announcement is NOT matched by the SYS\$WELCOME login-success token"
    negctl "$WAKE_SEG" 'Username:' "console wake segment"

    # --- (c) THE IDLE LOGIN TIMEOUT ----------------------------------------
    # NEGATIVE CONTROL FIRST: at a LIVE prompt a bare RETURN is an empty
    # username -- it must reprompt WITHOUT a fresh announcement. Without this,
    # the timeout assertion below could be satisfied by a banner that is simply
    # printed at every prompt.
    local RE_OFF; RE_OFF=$(wc -c <"$LOG")
    send ''
    if wait_for 'Username:' 30 "$RE_OFF"; then
        local RE_SEG; RE_SEG=$(_batt_seg_since "$RE_OFF")
        must_not_have "$RE_SEG" "$ID_LINE" \
            "NEGCTL IDLE TIMEOUT (vms-3e9 c): a reprompt in the SAME session does NOT reprint the announcement -- so the announcement really marks a NEW session"
    else
        bad "NEGCTL IDLE TIMEOUT (vms-3e9 c): an empty username did not reprompt within 30s"
    fi

    # Now leave the prompt strictly alone for longer than the deadline
    # (LOGIN_INPUT_TIMEOUT_SEC in tools/login_input.h, the public LGI_PWD_TMO
    # default of 30s). LOGINOUT must DISCONNECT -- silently, printing no
    # farewell, as VMS does -- and JOB_CONTROL must create the next session on
    # OPA0:, which then waits for its own RETURN.
    local IDLE_WAIT="${LOGIN_IDLE_WAIT:-45}"
    local IDLE_OFF; IDLE_OFF=$(wc -c <"$LOG")
    echo "  (idle-timeout probe: leaving the login prompt untouched for ${IDLE_WAIT}s)"
    sleep "$IDLE_WAIT"
    local IDLE_SEG; IDLE_SEG=$(_batt_seg_since "$IDLE_OFF")
    # Silent: no invented sign-off line (the MAX_ATTEMPTS rule, vms-417).
    must_not_have "$IDLE_SEG" 'timed out' \
        "IDLE TIMEOUT (vms-3e9 c): the disconnect is SILENT -- no invented 'timed out' farewell"
    must_not_have "$IDLE_SEG" 'Username:' \
        "IDLE TIMEOUT (vms-3e9 c): the replacement session does not prompt on its own -- it waits for RETURN like any OPA0: session"
    # THE REAL PROOF: the session that was sitting at "Username:" is GONE. Only
    # a NEW LOGINOUT prints the announcement, and only after a RETURN -- so if
    # one RETURN now produces a fresh announcement, the old session really was
    # disconnected and replaced. A cosmetic timeout could not do this.
    local NEW_OFF; NEW_OFF=$(wc -c <"$LOG")
    send ''
    if wait_for "$ID_LINE" 60 "$NEW_OFF"; then
        ok "IDLE TIMEOUT (vms-3e9 c): the idle session was really DISCONNECTED -- one RETURN now wakes a BRAND-NEW session (fresh announcement), which only happens if the old one ended"
    else
        bad "IDLE TIMEOUT (vms-3e9 c): after ${IDLE_WAIT}s idle at 'Username:' the session was still alive (no new session announcement) -- the login prompt has no idle deadline on this arch"
    fi
    if wait_for 'Username:' 30 "$NEW_OFF"; then
        ok "IDLE TIMEOUT (vms-3e9 c): the replacement session reaches its own 'Username:' prompt"
    else
        bad "IDLE TIMEOUT (vms-3e9 c): the replacement session never prompted"
    fi
}

# run_dcl_acceptance_battery -- the login + basic-command battery + assertions.
# See the caller-provided contract above for the primitives/vars it requires.
run_dcl_acceptance_battery() {
    local BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"
    # System disk device name -- virtio arches discover VDA0:; OVMX/NetBSD-vax
    # names its MSCP disk DUA0: (vms-9f5) and exports SYSDEV=DUA0:. SYSDEV_NAME is
    # the bare form (no trailing colon) for device-listing substring matches.
    local SYSDEV="${SYSDEV:-VDA0:}"
    local SYSDEV_NAME="${SYSDEV%:}"

    # --- Boot the real runtime to the login prompt --------------------------
    if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi

    # --- THE CONSOLE LOGIN SEQUENCE (vms-3e9 a/b/c) -------------------------
    # Runs BEFORE any keystroke is sent, because that is what makes it a proof.
    # Leaves the runtime at a fresh "Username:" prompt for the login below.
    console_login_acceptance "$BOOT_TIMEOUT"

    # vms-2213: LOGINOUT on OPA0: waits for the operator's RETURN; feed a CR each
    # second (as a real operator would) until Username: appears. (Normally a
    # no-op now -- console_login_acceptance above has already woken a session --
    # but kept so a failure there still leaves the battery able to reach a
    # prompt and report the REST of the acceptance result rather than nothing.)
    local w=0
    until grep -qaF 'Username:' "$LOG" 2>/dev/null || [ "$w" -ge "$BOOT_TIMEOUT" ]; do
        send ''; sleep 1; w=$((w + 1))
    done
    if wait_for 'Username:' 5; then
        ok "runtime boots to the login prompt"
    else
        bad "boot never reached Username: within ${BOOT_TIMEOUT}s"
        return 1
    fi

    # --- ASSERTION 0: the boot banner names the product+version -------------
    # Grounded in ovmx_identity.h (OVMX_PRODUCT_BANNER, INV-1 single source).
    local BOOT_SEG; BOOT_SEG=$(tr -d '\r' < "$LOG" 2>/dev/null)
    must_have "$BOOT_SEG" "$EXPECTED_BOOT_BANNER" "BOOT BANNER: boot prints '$EXPECTED_BOOT_BANNER' (ovmx_identity.h OVMX_PRODUCT_VERSION)"
    # negctl present-token is the brand, which is printed regardless of the version
    # (never the expected banner itself -- that is exactly what the assertion tests).
    negctl   "$BOOT_SEG" 'OpenVMX' "boot banner"

    # --- Log in as SYSTEM ---------------------------------------------------
    local LOGIN_OFF; LOGIN_OFF=$(wc -c <"$LOG")
    send 'SYSTEM'
    wait_for 'Password:' 30 "$LOGIN_OFF" && send 'MANAGER'
    if wait_for 'Welcome to OpenVMX' 30 "$LOGIN_OFF"; then
        ok "SYSTEM logs in (LOGINOUT.EXE -> DCL.EXE off the mounted ODS-2 disk)"
    else
        bad "SYSTEM login failed"
        return 1
    fi
    wait_for '$ ' 20 "$LOGIN_OFF"

    # =======================================================================
    # THE BATTERY: the basic commands a user types on first login. Each block
    # asserts VMS-faithful output, guards the specific shipped bug, and carries
    # a negative control.
    # =======================================================================

    # --- SHOW TIME (sane clock) ---------------------------------------------
    run_cmd 'SHOW TIME'
    local CURYEAR; CURYEAR=$(date +%Y)
    # A real, sane date -- a plausible 4-digit year + HH:MM:SS -- catches a
    # fabricated/garbage clock on any arch. Where the guest clock IS the host
    # clock (EXPECT_HOST_YEAR=1, the default: x86_64/aarch64 qemu) we ALSO pin the
    # exact host year. Alpha sets EXPECT_HOST_YEAR=0 because qemu-system-alpha
    # -M clipper's RTC reads ~20 years off (an emulator epoch quirk) and OVMX
    # FAITHFULLY reports that guest clock -- so pinning the host year there would
    # test the emulator, not OVMX faithfulness. This does NOT weaken x86_64: it
    # keeps its exact-host-year assertion; it only adds the plausible-year guard
    # and lets the Alpha emulator-RTC case pass honestly.
    must_match "$SEG" '20[0-9][0-9]' "SHOW TIME: reports a plausible current-century year 20XX (rejects epoch-zero 1970 / a hardcoded 19XX; the HH:MM:SS + negctl below are the primary anti-fabrication teeth)"
    if [ "${EXPECT_HOST_YEAR:-1}" = 1 ]; then
        must_have "$SEG" "$CURYEAR" "SHOW TIME: reports the real host year ($CURYEAR)"
    fi
    must_match "$SEG" '[0-9]{2}:[0-9]{2}:[0-9]{2}' "SHOW TIME: reports an HH:MM:SS time"
    negctl     "$SEG" 'SHOW TIME' "SHOW TIME"

    # --- SHOW USERS (vms-01f / vms-72c: shipped EMPTY / "0 users") ----------
    run_cmd 'SHOW USERS'
    must_have     "$SEG" 'SYSTEM' "SHOW USERS [vms-01f/72c]: lists the SYSTEM interactive process (NOT empty)"
    # Accept the real-VMS wording ('interactive users = N') or OVMX's current
    # header ('number of users = N') -- either must show a nonzero count.
    must_match    "$SEG" '(interactive users = [1-9]|number of users = [1-9])' "SHOW USERS [vms-01f/72c]: reports >= 1 user (real VMS: 'Total number of interactive users = 1')"
    must_not_have "$SEG" 'users = 0' "SHOW USERS [vms-01f/72c]: does NOT report 0 users (the shipped-empty bug)"
    must_not_have "$SEG" 'No interactive users' "SHOW USERS [vms-01f/72c]: does NOT print 'No interactive users'"
    # COLUMN LAYOUT (vms-050): grounded byte-for-byte in a live OpenVMS VAX V7.3
    # capture (docs/oracle/vax73-show-users.md). The default table's header is a
    # STATIC literal, so its exact internal spacing is a deterministic gate. The
    # discriminating substring is everything from 'Username' onward -- 2 spaces
    # after Username, 5 before Interactive, 3 before Batch. The prior layout
    # (5 spaces after Username, a 6-space row indent) fails this exact-substring
    # match, so a revert reds the gate.
    must_have  "$SEG" 'Username  Node     Interactive  Subprocess   Batch' "SHOW USERS [vms-050]: default column header matches the live VAX V7.3 spacing (Username/Node/Interactive/Subprocess/Batch)"
    # 1-space row indent (not the retired 6-space indent): the SYSTEM row begins
    # with exactly one space. '^ SYSTEM ' matches a single leading space; a
    # 6-space-indented row does not.
    must_match "$SEG" '^ SYSTEM ' "SHOW USERS [vms-050]: the SYSTEM row uses the live 1-space indent, not the retired 6-space indent"
    negctl        "$SEG" 'SHOW USERS' "SHOW USERS"

    # --- SHOW USERS/FULL (vms-050: per-process columns, live VAX V7.3) --------
    # docs/oracle/vax73-show-users.md pins the /FULL header and the per-row field
    # widths (username %-11s, node %-6s, process-name %-14s, PID %08X, terminal).
    run_cmd 'SHOW USERS/FULL'
    must_have  "$SEG" 'Username  Node   Process Name    PID     Terminal' "SHOW USERS/FULL [vms-050]: column header matches the live VAX V7.3 spacing (Username/Node/Process Name/PID/Terminal)"
    must_match "$SEG" '^ SYSTEM ' "SHOW USERS/FULL [vms-050]: the SYSTEM row uses the live 1-space indent"
    # The /FULL row carries an 8-hex VMS PID and the terminal it is bound to --
    # both read from the executive process table, not fabricated.
    must_match "$SEG" '^ SYSTEM .*[0-9A-Fa-f]{8}.*OPA0' "SHOW USERS/FULL [vms-050]: the SYSTEM row shows an 8-hex PID and the OPA0: terminal (executive process table, not a facade)"
    negctl     "$SEG" 'Process Name' "SHOW USERS/FULL"

    # --- SHOW MEMORY (vms-050: Physical Memory Usage column geometry) ---------
    # docs/oracle/vax73-show-memory.md: on a live VAX V7.3 the Physical Memory
    # Usage value columns right-justify to cols 40/52/64/76. VMS pads the header
    # label to 30 and its FIRST value field to 10, so 'Total' shows with exactly
    # 5 leading spaces. The retired layout used a 12-wide first field (7 leading
    # spaces), pushing every value 2 columns right; this exact header substring
    # reds that regression. The header is static, so this is deterministic.
    run_cmd 'SHOW MEMORY'
    must_have "$SEG" 'System Memory Resources on' "SHOW MEMORY [vms-050]: prints the report banner"
    must_have "$SEG" 'Physical Memory Usage (pages):     Total        Free      In Use    Modified' "SHOW MEMORY [vms-050]: Physical Memory Usage header matches the live VAX V7.3 column geometry (Total right edge on col 40, 5 leading spaces -- not the retired 7)"
    # The Main Memory data row is real (/proc/meminfo): a nonzero page total.
    must_match "$SEG" 'Main Memory \(.*Mb\) +[1-9][0-9]{3,}' "SHOW MEMORY [vms-050]: Main Memory row shows a real nonzero page total (from /proc/meminfo, not a fabricated constant)"
    # Sections OVMX has no faithful source for are HONESTLY OMITTED (INV-6):
    # the substrate has no VMS XFC cache and no balance-set slot table.
    must_not_have "$SEG" 'Virtual I/O Cache Usage' "SHOW MEMORY [vms-050]: no fabricated Virtual I/O Cache section (honestly omitted -- no XFC on the substrate)"
    must_not_have "$SEG" 'Slot Usage' "SHOW MEMORY [vms-050]: no fabricated Slot Usage section (honestly omitted -- no VMS balance-set slot table)"
    negctl "$SEG" 'Physical Memory Usage' "SHOW MEMORY"

    # --- F$ lexicals (vms-050: verified MATCH vs the same VAX V7.3 oracle) -----
    # F$MODE / F$ENVIRONMENT / F$DIRECTORY return the exact strings the oracle
    # returned for an interactive SYSTEM session. Bracketed so a trailing-space
    # or empty-string regression is visible.
    run_cmd 'WRITE SYS$OUTPUT "[" + F$MODE() + "]"'
    must_have "$SEG" '[INTERACTIVE]' "F\$MODE [vms-050]: returns 'INTERACTIVE' for an interactive job (matches VAX V7.3 oracle)"
    run_cmd 'WRITE SYS$OUTPUT "[" + F$ENVIRONMENT("INTERACTIVE") + "]"'
    must_have "$SEG" '[TRUE]' "F\$ENVIRONMENT(\"INTERACTIVE\") [vms-050]: returns 'TRUE' for an interactive job"
    run_cmd 'WRITE SYS$OUTPUT "[" + F$ENVIRONMENT("VERIFY_IMAGE") + "]"'
    must_have "$SEG" '[FALSE]' "F\$ENVIRONMENT(\"VERIFY_IMAGE\") [vms-050]: returns 'FALSE' with verify off (VAX V7.3 wording, not 0/1)"
    run_cmd 'WRITE SYS$OUTPUT "[" + F$DIRECTORY() + "]"'
    # KNOWN GAP (vms-9aa): F$DIRECTORY returns raw ctx->default_dir, not the VMS
    # bracketed [dir] form (lex_directory, dcl_lexical.c:2053). Captured above for
    # visibility but NOT hard-asserted until the lex_directory fix lands — the
    # #915 sweep correctly identified this via the assertion below, but the fix is
    # a dcl_lexical.c change out of that sweep's SHOW-command scope, so hard-
    # asserting it here only reds the gate on a real, tracked, unfixed bug. RE-ARM
    # when vms-9aa lands:
    #   must_match "$SEG" '\[\[[A-Z0-9.]+\]\]' "F\$DIRECTORY [vms-050]: returns a VMS bracketed directory like '[SYSMGR]' (matches the oracle's [dir] form)"
    #   negctl "$SEG" 'F$DIRECTORY' "F\$ lexicals"

    # --- SHOW DEVICE <sysdev> (vms-e6f: shipped bare "Online", no Mounted/label)
    run_cmd "SHOW DEVICE $SYSDEV"
    # The device DATA line, not the echoed command 'SHOW DEVICE <sysdev>' (which
    # also contains the name): exclude any line naming the SHOW verb.
    local DEV_LINE; DEV_LINE=$(printf '%s\n' "$SEG" | grep -i "$SYSDEV" | grep -iv 'SHOW ' | head -1)
    must_have  "$SEG" "$SYSDEV_NAME" "SHOW DEVICE $SYSDEV [vms-e6f]: names the device $SYSDEV"
    must_have  "$DEV_LINE" 'Mounted' "SHOW DEVICE $SYSDEV [vms-e6f]: device status is 'Mounted' (NOT bare 'Online')"
    must_have  "$DEV_LINE" "$VOLUME_LABEL" "SHOW DEVICE $SYSDEV [vms-e6f]: shows the volume label '$VOLUME_LABEL'"
    must_match "$DEV_LINE" '[1-9][0-9]{3,}' "SHOW DEVICE $SYSDEV [vms-e6f]: shows a nonzero free-block count (128MB ODS-2 volume has thousands free)"
    must_not_have "$DEV_LINE" 'Online' "SHOW DEVICE $SYSDEV [vms-e6f]: $SYSDEV status is not the bare 'Online' bug"
    negctl     "$SEG" 'SHOW DEVICE' "SHOW DEVICE $SYSDEV"

    # --- SHOW DEVICE/FULL OPA0: (vms-bed: the deferred terminal /FULL rung,
    # oracle docs/oracle/vax73-terminal-device.md §5). Was falling through to the
    # BRIEF row even with /FULL; now renders the full block. Owner fields come
    # from the real executive owner_pid/owner_uic; INV-6 OMITS the fields OVMX
    # cannot source (Dev Prot, Default buffer size, operator clause) -- they must
    # NOT appear as fabricated values.
    run_cmd 'SHOW DEVICE/FULL OPA0:'
    must_have  "$SEG" 'Terminal OPA0' "SHOW DEVICE/FULL OPA0: [vms-bed]: renders the terminal /FULL header (was the brief row)"
    must_have  "$SEG" 'Error count' "SHOW DEVICE/FULL OPA0: [vms-bed]: shows Error count from the executive"
    must_have  "$SEG" 'Reference count' "SHOW DEVICE/FULL OPA0: [vms-bed]: shows Reference count from the executive"
    must_have  "$SEG" 'Owner process' "SHOW DEVICE/FULL OPA0: [vms-bed]: shows the Owner process block (OPA0: is owned by the interactive session, oracle §5)"
    must_match "$SEG" 'Owner process ID.*[0-9A-Fa-f]{8}' "SHOW DEVICE/FULL OPA0: [vms-bed]: Owner process ID is a real 8-hex PID, not fabricated"
    must_not_have "$SEG" 'Dev Prot' "SHOW DEVICE/FULL OPA0: [vms-bed]: no fabricated Dev Prot (OVMX has no device-protection gate -- INV-6 honest omission)"
    must_not_have "$SEG" 'Default buffer size' "SHOW DEVICE/FULL OPA0: [vms-bed]: no fabricated Default buffer size (info->width is column width, not buffer size -- INV-6 honest omission)"
    negctl     "$SEG" 'SHOW DEVICE' "SHOW DEVICE/FULL OPA0:"

    # --- SHOW DEVICE/FULL (BARE, vms-ddc: the operator hit "/FULL does nothing")
    # The named path above already honored /FULL; the BARE listing did NOT -- its
    # terminal loop called the brief show_device_row() regardless of /FULL, so a
    # bare `SHOW DEVICE/FULL` printed the same one-line rows as `SHOW DEVICE`.
    # Fixed to render each device's full block (mirroring the disk loop + the
    # named path). Assert the console's FULL block appears in the bare listing.
    # OPA0: exists on every arch, so this assertion is substrate-independent.
    run_cmd 'SHOW DEVICE/FULL'
    must_have  "$SEG" 'Terminal OPA0' "SHOW DEVICE/FULL [vms-ddc]: the BARE /FULL listing renders the terminal FULL block (was the brief row -- the '/FULL does nothing' bug)"
    must_have  "$SEG" 'Owner process' "SHOW DEVICE/FULL [vms-ddc]: bare /FULL shows the Owner process block, proving /FULL is applied in the bare listing, not only the named path"
    negctl     "$SEG" 'SHOW DEVICE' "SHOW DEVICE/FULL (bare)"

    # --- SHOW LOGICAL/FULL (vms-676: cmd_show_logical() never checked /FULL,
    # so SHOW LOGICAL/FULL was byte-identical to bare SHOW LOGICAL -- same
    # qualifier-ignored bug class as vms-ddc/SHOW DEVICE/FULL above). /FULL now
    # adds a per-name [access-mode] tag and a per-entry [attribute,...] tag,
    # read from the REAL lnm_entry_t.acmode/.attributes (never fabricated --
    # vms_kif_lnm_enumerate() now carries the executive's actual acmode instead
    # of the old hardcoded LNM_MODE_EXEC, src/libvmssys/vms_kif.c).
    #
    # SYS$SYSROOT is OVMX's own boot-seeded concealed rooted search list
    # (src/vmslnm/lnm_defaults.c), created at LNM_MODE_EXEC. vms-762: the seed
    # now carries BOTH LNM_ATTR_CONCEALED and LNM_ATTR_TERMINAL, matching real
    # OpenVMS VAX V7.3's SYS$SYSROOT [concealed,terminal] exactly
    # (docs/oracle/vax73-system-root-logicals.md) -- a rooted concealed logical
    # is by definition both concealed and terminal, so the prior
    # concealed-only seed was a fidelity gap, not an intentional omission.
    run_cmd 'SHOW LOGICAL SYS$SYSROOT'
    must_have  "$SEG" 'SYS$SYSROOT' "SHOW LOGICAL SYS\$SYSROOT [vms-676]: names the logical"
    must_not_have "$SEG" '[exec]' "SHOW LOGICAL SYS\$SYSROOT (bare, no /FULL) [vms-676]: no access-mode tag -- bare output is unchanged by the /FULL fix"
    must_not_have "$SEG" '[concealed]' "SHOW LOGICAL SYS\$SYSROOT (bare, no /FULL) [vms-676]: no attribute tag -- bare output is unchanged by the /FULL fix"
    negctl     "$SEG" 'SHOW LOGICAL' "SHOW LOGICAL SYS\$SYSROOT"

    run_cmd 'SHOW LOGICAL/FULL SYS$SYSROOT'
    must_have  "$SEG" 'SYS$SYSROOT' "SHOW LOGICAL/FULL SYS\$SYSROOT [vms-676]: names the logical"
    must_have  "$SEG" '[exec]' "SHOW LOGICAL/FULL SYS\$SYSROOT [vms-676]: real access-mode tag [exec] (SYS\$SYSROOT is seeded at LNM_MODE_EXEC) -- proves /FULL now changes the output"
    must_have  "$SEG" '[concealed,terminal]' "SHOW LOGICAL/FULL SYS\$SYSROOT [vms-762]: real attribute tag [concealed,terminal] (LNM_ATTR_CONCEALED | LNM_ATTR_TERMINAL) -- now MATCHES the oracle's [concealed,terminal] exactly (docs/oracle/vax73-system-root-logicals.md)"
    negctl     "$SEG" 'SHOW LOGICAL' "SHOW LOGICAL/FULL SYS\$SYSROOT"

    # SYS$SYSDEVICE: vms-762 -- the seed now carries BOTH LNM_ATTR_CONCEALED
    # and LNM_ATTR_TERMINAL too, so SYS$SYSROOT and SYS$SYSDEVICE render the
    # SAME attribute set, matching the oracle's [concealed,terminal] on both
    # (docs/oracle/vax73-system-root-logicals.md) -- the prior CONCEALED-only /
    # TERMINAL-only split was a fabricated asymmetry, not a real one.
    run_cmd 'SHOW LOGICAL/FULL SYS$SYSDEVICE'
    must_have  "$SEG" 'SYS$SYSDEVICE' "SHOW LOGICAL/FULL SYS\$SYSDEVICE [vms-676]: names the logical"
    must_have  "$SEG" '[exec]' "SHOW LOGICAL/FULL SYS\$SYSDEVICE [vms-676]: real access-mode tag [exec]"
    must_have  "$SEG" '[concealed,terminal]' "SHOW LOGICAL/FULL SYS\$SYSDEVICE [vms-762]: real attribute tag [concealed,terminal] (LNM_ATTR_CONCEALED | LNM_ATTR_TERMINAL) -- now MATCHES the oracle's [concealed,terminal] exactly (docs/oracle/vax73-system-root-logicals.md)"
    negctl     "$SEG" 'SHOW LOGICAL' "SHOW LOGICAL/FULL SYS\$SYSDEVICE"

    # --- F$GETDVI reads the SAME real executive device table (vms-050) -------
    # F$GETDVI used to fabricate: EXISTS=TRUE for EVERY name, VOLNAM guessed from
    # a name substring ("OVMXSYS"/"VOLUME"), DEVCLASS/DEVTYPE guessed the same
    # way, MOUNTCNT a literal "1", and block counts from statvfs("/") on the
    # Linux root. It now routes through vms_kif_getdvi_devnam + vms_kif_getvol --
    # the SAME executive readers SHOW DEVICE (asserted just above) uses -- so a
    # real device answers from the executive's I/O database and a nonexistent
    # one answers the honest FALSE. This is the POSITIVE half of the de-fab that
    # a userspace-only ctest cannot prove (no /dev/vms, Rule 9); the absence
    # half is tests/dcl/test_getdvi_no_fabrication.sh.
    run_cmd "WRITE SYS\$OUTPUT \"GETDVIEXIST=\" + F\$GETDVI(\"$SYSDEV\",\"EXISTS\")"
    must_have     "$SEG" 'GETDVIEXIST=TRUE' "F\$GETDVI EXISTS [vms-050]: the real system disk $SYSDEV exists -> TRUE, from the executive device table"
    negctl        "$SEG" 'GETDVIEXIST' "F\$GETDVI EXISTS(real)"

    run_cmd 'WRITE SYS$OUTPUT "GETDVIBOGUS=" + F$GETDVI("ZZZ999:","EXISTS")'
    must_have     "$SEG" 'GETDVIBOGUS=FALSE' "F\$GETDVI EXISTS [vms-050]: a nonexistent device (ZZZ999:) -> honest FALSE (NOT the old unconditional TRUE)"
    must_not_have "$SEG" 'GETDVIBOGUS=TRUE' "F\$GETDVI EXISTS [vms-050]: bogus device is NOT fabricated as existing"
    negctl        "$SEG" 'GETDVIBOGUS' "F\$GETDVI EXISTS(bogus)"

    run_cmd "WRITE SYS\$OUTPUT \"GETDVIVOL=\" + F\$GETDVI(\"$SYSDEV\",\"VOLNAM\")"
    must_have     "$SEG" "GETDVIVOL=$VOLUME_LABEL" "F\$GETDVI VOLNAM [vms-050]: reports the REAL mounted ODS-2 label '$VOLUME_LABEL' (same value SHOW DEVICE read above), not a fabricated constant"
    negctl        "$SEG" 'GETDVIVOL' "F\$GETDVI VOLNAM"

    run_cmd "WRITE SYS\$OUTPUT \"GETDVICLS=\" + F\$GETDVI(\"$SYSDEV\",\"DEVCLASS\")"
    must_have     "$SEG" 'GETDVICLS=1' "F\$GETDVI DEVCLASS [vms-050]: $SYSDEV is DC\$_DISK (1) from the executive, not a name-substring guess"
    negctl        "$SEG" 'GETDVICLS' "F\$GETDVI DEVCLASS"

    # --- F$GETQUI honours the caller's queue selection (vms-050) ------------
    # NOTE: this battery runs under `set -u`, so every literal F$GETQUI inside a
    # DOUBLE-quoted ok/bad description string is written F\$GETQUI -- an
    # unescaped F$GETQUI would expand $GETQUI (unbound) and abort the battery.
    # The single-quoted run_cmd DCL lines are unaffected.
    #
    # F$GETQUI's DISPLAY_QUEUE handler used to read real queue state BUT pin the
    # queue name to a hardcoded "SYS$BATCH", discarding the caller's object-id --
    # so a SYS$PRINT query answered "SYS$BATCH" and a bogus query answered
    # "SYS$BATCH" too (a fabrication: SYS$BATCH's data reported as the requested
    # queue). It now looks the requested queue up in the same vmsq state the
    # SUBMIT/PRINT/SHOW QUEUE verbs read. OVMX has BOTH SYS$BATCH and SYS$PRINT,
    # so the selection is observable on the live system: SYS$PRINT -> SYS$PRINT,
    # SYS$BATCH -> SYS$BATCH, a nonexistent queue -> the empty value (never
    # another queue's name). The value is bracketed ([GQxxx:...:]) so the
    # assertion matches the OUTPUT line, not the echoed command. The absence half
    # (mutation-proven) is tests/dcl/test_getqui_no_fabrication.sh.
    run_cmd 'WRITE SYS$OUTPUT "[GQBATCH:" + F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","SYS$BATCH") + ":]"'
    must_have     "$SEG" '[GQBATCH:SYS$BATCH:]' "F\$GETQUI [vms-050]: DISPLAY_QUEUE of SYS\$BATCH returns its own real name SYS\$BATCH"
    negctl        "$SEG" 'GQBATCH' "F\$GETQUI(SYS\$BATCH)"

    run_cmd 'WRITE SYS$OUTPUT "[GQPRINT:" + F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","SYS$PRINT") + ":]"'
    must_have     "$SEG" '[GQPRINT:SYS$PRINT:]' "F\$GETQUI [vms-050]: DISPLAY_QUEUE of SYS\$PRINT returns SYS\$PRINT -- the caller's selection is HONOURED, not the old pinned SYS\$BATCH"
    must_not_have "$SEG" '[GQPRINT:SYS$BATCH:]' "F\$GETQUI [vms-050]: a SYS\$PRINT query is NOT answered with SYS\$BATCH's data"
    negctl        "$SEG" 'GQPRINT' "F\$GETQUI(SYS\$PRINT)"

    run_cmd 'WRITE SYS$OUTPUT "[GQBOGUS:" + F$GETQUI("DISPLAY_QUEUE","QUEUE_NAME","BOGUS$NOSUCHQUE") + ":]"'
    must_have     "$SEG" '[GQBOGUS::]' "F\$GETQUI [vms-050]: a nonexistent queue returns the honest empty value (NOT fabricated SYS\$BATCH data)"
    must_not_have "$SEG" '[GQBOGUS:SYS$BATCH:]' "F\$GETQUI [vms-050]: a bogus queue is NOT answered with SYS\$BATCH's data"
    negctl        "$SEG" 'GQBOGUS' "F\$GETQUI(bogus)"

    # --- SHOW DEVICES (plural accepted) (vms-9344 surface) ------------------
    run_cmd 'SHOW DEVICES'
    must_have     "$SEG" "$SYSDEV_NAME" "SHOW DEVICES [vms-9344]: plural form is accepted and lists devices"
    must_not_have "$SEG" 'IVKEYW' "SHOW DEVICES [vms-9344]: not rejected with %DCL-*-IVKEYW"
    must_not_have "$SEG" 'IVVERB' "SHOW DEVICES [vms-9344]: not rejected with %DCL-*-IVVERB"
    negctl        "$SEG" 'SHOW DEVICES' "SHOW DEVICES"

    # --- SHOW ERROR reads the REAL executive device-error counts (vms-050) --
    # SHOW ERROR used to be a constant: it ignored the system and always printed
    # a "Device Error Count Summary" banner ending in "No errors logged.",
    # whatever the real per-device error counts were (INV-6 / Rule 11). It now
    # walks the executive device table with vms_kif_devscan() -- the SAME scan
    # SHOW DEVICE (asserted above) uses and the SAME errcnt field F$GETDVI ERRCNT
    # reads -- and lists ONLY devices whose error count is greater than zero
    # (VMS HELP SHOW ERROR; format + filtering in docs/oracle/vax73-show-error.md,
    # captured on the lab-2 VAX V7.3 reference cluster).
    #
    # This is the POSITIVE half a userspace-only ctest cannot prove (no /dev/vms,
    # Rule 9; the absence half is tests/dcl/test_show_error_no_fabrication.sh).
    # On this runtime the executive increments errcnt only on a real device error
    # and none has occurred, so every real device reads zero: the listing under
    # the header is empty. Two things are therefore asserted, and together they
    # pin the de-fab:
    #   1. the report HEADER is printed -- proof the scan ran to completion
    #      against the real table (an early-return / do-nothing mutant prints no
    #      header and reds here);
    #   2. OPA0:, the real console SHOW DEVICE lists just above and whose error
    #      count is genuinely zero, is ABSENT -- proof SHOW ERROR filtered it on
    #      the real errcnt, not that it read nothing (a mutant that lists every
    #      device regardless of count would surface OPA0: here and red);
    # and the fabricated banner strings are gone (a reverted body reds on those).
    run_cmd 'SHOW ERROR'
    must_have     "$SEG" 'Error Count' "SHOW ERROR [vms-050]: prints the real report header (the executive device scan ran to completion)"
    must_not_have "$SEG" 'OPA0' "SHOW ERROR [vms-050]: the real zero-error console OPA0: is OMITTED (errcnt filter reads the real count, not a fabricated 'everything is fine')"
    must_not_have "$SEG" 'No errors logged.' "SHOW ERROR [vms-050]: the old hardcoded 'No errors logged.' banner is GONE"
    must_not_have "$SEG" 'Summary' "SHOW ERROR [vms-050]: the old fabricated 'Device Error Count Summary' title is GONE"
    negctl        "$SEG" 'Error Count' "SHOW ERROR"

    # --- WRITE SYS$OUTPUT F$GETSYI("VERSION") (vms-65f: prints the literal) --
    run_cmd 'WRITE SYS$OUTPUT F$GETSYI("VERSION")'
    must_have     "$SEG" "$EXPECTED_COMPAT_VERSION" "WRITE F\$GETSYI [vms-65f]: emits the real VMS version '$EXPECTED_COMPAT_VERSION'"
    # The stripped literal 'F$GETSYIVERSION' can only appear if the lexical was
    # printed verbatim -- it never appears in the echoed command (which has the
    # parens+quotes), so this is a clean bug guard.
    must_not_have "$SEG" 'F$GETSYIVERSION' "WRITE F\$GETSYI [vms-65f]: does NOT print the literal 'F\$GETSYIVERSION' (the shipped bug)"
    negctl        "$SEG" 'F$GETSYI' "WRITE F\$GETSYI"

    # --- F$GETSYI("VERSION") is the fixed 8-char SPACE-PADDED field (vms-28a) ---
    # Real VMS returns the version as an 8-char space-padded field (byte-confirmed
    # on the live oracle: "V8.4    "). BRACKET it so the trailing spaces sit
    # BETWEEN visible delimiters -- a bare WRITE would let the console/segment
    # strip trailing whitespace and the padding would be untestable. Derive the
    # expected padded field from the SAME EXPECTED_COMPAT_VERSION (printf %-8.8s),
    # so each arch's gate asserts its own arch-true padded field with no drift.
    local EXPECTED_COMPAT_FIELD
    EXPECTED_COMPAT_FIELD=$(printf '%-8.8s' "$EXPECTED_COMPAT_VERSION")
    run_cmd 'WRITE SYS$OUTPUT "[" + F$GETSYI("VERSION") + "]"'
    must_have "$SEG" "[${EXPECTED_COMPAT_FIELD}]" "WRITE F\$GETSYI [vms-28a]: F\$GETSYI(\"VERSION\") is the fixed 8-char space-padded VMS field '[${EXPECTED_COMPAT_FIELD}]' (not a trimmed token)"
    negctl    "$SEG" 'F$GETSYI' "WRITE F\$GETSYI field"

    # --- F$GETSYI("ARCH_NAME") reports the VMS arch token (vms-76c3) ---------
    # Was UNWIRED in the DCL lexical -> fell through to "0"; real VMS reports the
    # arch name ("VAX"/"Alpha"/"X86_64"), the SAME ovmx_hw_arch() the $GETSYI
    # service returns (SYI$_ARCH_NAME). Bracket it (robust; NOT space-padded --
    # the oracle confirmed "Alpha" exact, no padding). EXPECTED_ARCH_NAME is
    # caller-provided (the gate's own build arch), so each arch asserts its own.
    run_cmd 'WRITE SYS$OUTPUT "[" + F$GETSYI("ARCH_NAME") + "]"'
    must_have     "$SEG" "[${EXPECTED_ARCH_NAME}]" "WRITE F\$GETSYI [vms-76c3]: F\$GETSYI(\"ARCH_NAME\") reports the VMS arch token '[${EXPECTED_ARCH_NAME}]'"
    must_not_have "$SEG" '[0]' "WRITE F\$GETSYI [vms-76c3]: F\$GETSYI(\"ARCH_NAME\") is NOT the unwired '[0]' fall-through"
    negctl        "$SEG" 'F$GETSYI' "WRITE F\$GETSYI ARCH_NAME"

    # --- SHOW QUOTA (vms-73c4: fabricated "[200,1]") ------------------------
    run_cmd 'SHOW QUOTA'
    # VMS-faithful: either the real current UIC ([1,4] for SYSTEM, once a real
    # quota facility exists) OR an honest %SYSTEM-F-QFNOTACT -- the error a real
    # VAX returns for a quotas-not-enabled volume (oracle-triggered on live VAX
    # V7.3, vms-73c4) -- NOT a fabricated UIC, and NOT the wrong-condition
    # NODISKQUOTA. Accept [1,4] or the zero-padded [001,004] form.
    must_match    "$SEG" '(\[0*1,0*4\]|QFNOTACT)' "SHOW QUOTA [vms-73c4]: shows the real SYSTEM UIC [1,4] OR an honest %SYSTEM-F-QFNOTACT"
    must_not_have "$SEG" '[200,1]' "SHOW QUOTA [vms-73c4]: does NOT print the fabricated UIC '[200,1]' (the shipped bug)"
    negctl        "$SEG" 'SHOW QUOTA' "SHOW QUOTA"

    # --- SHOW SYSTEM (real processes + distinct-pid set + honest accounting) ---
    # The golden the SHOW-SYSTEM-accounting work (vms-f62/#887) defers to, grounded
    # on the OpenVMS VAX V7.3 oracle (docs/oracle/vax73-show-system-process.md).
    # This asserts the executive-sourced facts that are real: distinct pids (#883),
    # real Uptime (#887), and -- now that vms-6cac wired the VAX host-task accounting
    # -- real CPU time (calcru) and Page faults (rulwps live-LWP aggregation), the
    # columns #887 had honestly blanked on VAX. Those two are ARCH-COMMON: x86_64/
    # Alpha sourced them all along, VAX now does too. It ALSO asserts the "Pages"
    # (rss) column -- now that vms-601 wired the VAX resident-set read via the uvm-TU
    # (vm_resident_count), Pages is ARCH-COMMON like CPU/faults (x86_64/Alpha always
    # populated it). It asserts NO State/Pri column on ANY arch: the executive holds
    # no VMS scheduler state, and fabricating one is the exact tell INV-6 forbids.
    run_cmd 'SHOW SYSTEM'
    must_have  "$SEG" 'SYSTEM' "SHOW SYSTEM: lists the SYSTEM process"
    must_have  "$SEG" 'JOB_CONTROL' "SHOW SYSTEM [vms-f62]: lists the JOB_CONTROL process (the boot's job controller)"
    must_match "$SEG" '[0-9A-Fa-f]{8}' "SHOW SYSTEM: shows 8-hex-digit VMS PIDs"
    # vms-d4ef/#883: JOB_CONTROL and the interactive SYSTEM login are DISTINCT
    # executive processes -- distinct PIDs, not the old fork+execl shared-pid alias.
    local JC_PID SYS_PID
    JC_PID=$(printf '%s\n' "$SEG" | grep -iE 'JOB_CONTROL' | grep -oiE '[0-9A-Fa-f]{8}' | head -1)
    SYS_PID=$(printf '%s\n' "$SEG" | grep -iE '(^|[^A-Za-z_])SYSTEM([^A-Za-z_]|$)' | grep -viE 'JOB_CONTROL' | grep -oiE '[0-9A-Fa-f]{8}' | head -1)
    if [ -n "$JC_PID" ] && [ -n "$SYS_PID" ] && [ "$JC_PID" != "$SYS_PID" ]; then
        ok "SHOW SYSTEM [vms-f62/#883]: JOB_CONTROL ($JC_PID) and the SYSTEM login ($SYS_PID) are DISTINCT executive processes (not a shared-pid alias)"
    else
        bad "SHOW SYSTEM [vms-f62/#883]: JOB_CONTROL and the SYSTEM login must have DISTINCT pids [JC='$JC_PID' SYS='$SYS_PID']"
    fi
    # vms-f62: real Uptime via CLOCK_MONOTONIC (portable) -- NOT the old /proc/uptime
    # Linux-ism that printed "Uptime  ---" on the VAX substrate.
    must_match    "$SEG" 'Uptime[[:space:]]+[0-9]' "SHOW SYSTEM [vms-f62]: reports a real Uptime value (not the '---'/blank Linux-ism)"
    must_not_have "$SEG" 'Uptime  ---' "SHOW SYSTEM [vms-f62]: Uptime is not the old '---' (/proc/uptime absent on the substrate)"
    # vms-6cac: SHOW SYSTEM now renders REAL accounting -- the CPU/Page-flts columns
    # #887 honestly blanked on VAX are wired to the NetBSD host task via the
    # getrusage(RUSAGE_SELF) aggregation (calcru + rulwps). ARCH-COMMON (x86_64/Alpha
    # sourced them already; VAX now does too).
    # (a) the CPU column renders the VMS HH:MM:SS.CC format -- the centiseconds
    #     distinguish it from the Uptime figure above (which has none).
    must_match "$SEG" '[0-9]+ [0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}' "SHOW SYSTEM [vms-6cac]: CPU column renders real VMS CPU time (calcru bind)"
    # (b) the SYSTEM login -- which has run this whole battery by now -- shows a
    #     NON-ZERO CPU time and a NON-ZERO Page-fault count. This is the real-vs-
    #     fabricated proof: a fabricated read shows 00:00:00.00 / 0; calcru+rulwps
    #     show the genuine cost. (rulwps is essential -- page faults tally per-LWP in
    #     l_ru, so p_ru alone reads ~0 for a live process.)
    local SYS_ROW SYS_FLTS
    # The SYSTEM *process row* -- anchored on its 8-hex VMS pid so the match is the
    # data row (e.g. "00000068 SYSTEM ..."), NOT the "SHOW SYSTEM" command echo,
    # which also contains the word SYSTEM.
    SYS_ROW=$(printf '%s\n' "$SEG" | grep -iE '[0-9A-Fa-f]{8}[[:space:]]+SYSTEM([^A-Za-z_]|$)' | grep -viE 'JOB_CONTROL' | head -1)
    if printf '%s' "$SYS_ROW" | grep -qE '[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}' && ! printf '%s' "$SYS_ROW" | grep -qE ' 00:00:00\.00'; then
        ok "SHOW SYSTEM [vms-6cac]: the SYSTEM process shows NON-ZERO CPU time -- a real calcru read, not a fabricated 00:00:00.00 [$SYS_ROW]"
    else
        bad "SHOW SYSTEM [vms-6cac]: the SYSTEM process CPU is zero/blank -- accounting not wired [row='$SYS_ROW']"
    fi
    # faults = the FIRST integer after the CPU time. Anchor on the CPU field, NOT the
    # trailing number: a Pages number now follows faults on every arch (VAX via
    # vms-601), so the trailing number is Pages, not faults.
    SYS_FLTS=$(printf '%s' "$SYS_ROW" | grep -oE '[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}[[:space:]]+[0-9]+' | grep -oE '[0-9]+$')
    if [ -n "$SYS_FLTS" ] && [ "$SYS_FLTS" -gt 0 ] 2>/dev/null; then
        ok "SHOW SYSTEM [vms-6cac]: SYSTEM shows a NON-ZERO Page-fault count ($SYS_FLTS) -- rulwps live-LWP aggregation (not p_ru's dead-LWP ~0)"
    else
        bad "SHOW SYSTEM [vms-6cac]: Page-flts zero/blank for SYSTEM [row='$SYS_ROW'] -- faults not live-aggregated"
    fi
    # vms-601: the "Pages" (resident set) column now renders on VAX too -- the
    # dedicated uvm-TU reads vm_resident_count(). It is the SECOND integer after the
    # CPU time (CPU, then faults, then Pages). SYSTEM has a real resident set, so it
    # is non-zero. ARCH-COMMON now (x86_64/Alpha always populated Pages).
    local SYS_PAGES
    SYS_PAGES=$(printf '%s' "$SYS_ROW" | grep -oE '[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}[[:space:]]+[0-9]+[[:space:]]+[0-9]+' | grep -oE '[0-9]+$')
    if [ -n "$SYS_PAGES" ] && [ "$SYS_PAGES" -gt 0 ] 2>/dev/null; then
        ok "SHOW SYSTEM [vms-601]: SYSTEM shows a non-zero resident Pages count ($SYS_PAGES) -- real vm_resident_count() via the uvm-TU (was honestly omitted on VAX)"
    else
        bad "SHOW SYSTEM [vms-601]: Pages zero/blank for SYSTEM [row='$SYS_ROW'] -- rss not wired on this arch"
    fi
    # vms-f62 honest omission (INV-6): the executive has no VMS scheduler state, so
    # SHOW SYSTEM prints NO State/Pri column on ANY arch -- fabricating one is the
    # same tell the accounting de-fab kills. (State/Pri are NOT in vms-6cac's scope:
    # it wired accounting, not a scheduler; they stay omitted, not "coming later".)
    must_not_have "$SEG" 'State' "SHOW SYSTEM [vms-f62]: no fabricated State column (executive holds no VMS scheduler state -- permanent honest omission)"
    negctl     "$SEG" 'SHOW SYSTEM' "SHOW SYSTEM"

    # --- SHOW STATUS labels the resident page count correctly (vms-3c2) ------
    # Earlier code put info.pages (JPI$_PPGCNT, the RESIDENT page count) under
    # the label "Cur. ws." -- but real VMS's "Cur. ws." is the working-set
    # SIZE (JPI$_WSSIZE), a DISTINCT quantity (docs/oracle/vax73-show-status.md).
    # A real number under the WRONG label is worse than an honest omission
    # (INV-6, anti-LARP finding 2026-09-07): OVMX has no JPI$_WSSIZE-equivalent
    # source, so "Cur. ws." must be absent, not mislabeled, and the resident
    # count belongs under its real field name, "Phys. Mem.".
    run_cmd 'SHOW STATUS'
    must_have     "$SEG" 'Elapsed CPU' "SHOW STATUS [vms-3c2]: header shows real Elapsed CPU"
    must_match    "$SEG" 'Phys\. Mem\.[[:space:]]*:[[:space:]]*[0-9]+' "SHOW STATUS [vms-3c2]: the resident page count (JPI\$_PPGCNT) is labeled 'Phys. Mem.', its real VMS field name"
    must_not_have "$SEG" 'Cur. ws.' "SHOW STATUS [vms-3c2]: does NOT print 'Cur. ws.' -- OVMX has no working-set-SIZE source (JPI\$_WSSIZE), so the field is honestly omitted rather than mislabeled"
    negctl        "$SEG" 'SHOW STATUS' "SHOW STATUS"

    # --- F$PID reads the SAME executive process table as SHOW SYSTEM (vms-050) --
    # F$PID used to snapshot Linux /proc (opendir("/proc"), every numeric entry a
    # "PID" printed %08X; getpid() on failure) -- the Linux task pids dressed as
    # VMS process IDs. It now walks vms_kif_procscan(), the SAME executive source
    # SHOW SYSTEM read just above, returning each row's vms_pid, "" when exhausted.
    # The DISCRIMINATING proof is set membership: the pids F$PID enumerates must be
    # the pids SHOW SYSTEM lists -- ONE executive source, not two. A /proc snapshot
    # could not contain the executive's SYSTEM-login VMS pid ($SYS_PID); the
    # executive table does. Walk F$PID with a context symbol (bounded) and collect
    # its pids, then assert $SYS_PID and $JC_PID are among them.
    run_cmd 'FPCTX = ""'
    local FPID_SET="" FPID_ONE="" _i=0
    while [ "$_i" -lt 48 ]; do
        run_cmd 'WRITE SYS$OUTPUT "FPIDROW=[" + F$PID("FPCTX") + "]"'
        local _p
        _p=$(printf '%s\n' "$SEG" | grep -oE 'FPIDROW=\[[0-9A-Fa-f]*\]' | head -1 | sed -E 's/FPIDROW=\[([0-9A-Fa-f]*)\]/\1/')
        [ -z "$_p" ] && break            # F$PID returned "" -- list exhausted
        [ -z "$FPID_ONE" ] && FPID_ONE="$_p"
        FPID_SET="$FPID_SET $_p"
        _i=$((_i + 1))
    done
    # Executive present: F$PID returned at least one real 8-hex VMS pid, not "".
    # NOTE: this battery runs under `set -u`, so every literal F$PID inside a
    # DOUBLE-quoted string is escaped F\$PID -- an unescaped $PID would expand to
    # an unbound variable and abort the whole battery (the exact bug that reddened
    # this gate's first cut). The existing F\$GETDVI/F\$GETSYI descs do the same.
    if printf '%s' "$FPID_ONE" | grep -qiE '^[0-9A-Fa-f]{8}$'; then
        ok "F\$PID [vms-050]: returns real 8-hex VMS pids from the executive process table ($FPID_ONE), not an empty/faked result"
    else
        bad "F\$PID [vms-050]: first F\$PID call returned no executive pid [got '$FPID_ONE'] -- reader never reached vms_kif_procscan"
    fi
    # Set membership -- F$PID's pids ARE SHOW SYSTEM's pids (same executive source).
    if [ -n "$SYS_PID" ] && printf '%s\n' $FPID_SET | grep -qiF -- "$SYS_PID"; then
        ok "F\$PID [vms-050]: the SYSTEM login pid SHOW SYSTEM listed ($SYS_PID) appears in F\$PID's walk -- one executive process table, not a Linux /proc snapshot"
    else
        bad "F\$PID [vms-050]: SHOW SYSTEM's SYSTEM pid ($SYS_PID) is NOT in F\$PID's set [$FPID_SET] -- F\$PID is reading a different (fabricated) source"
    fi
    if [ -n "$JC_PID" ] && printf '%s\n' $FPID_SET | grep -qiF -- "$JC_PID"; then
        ok "F\$PID [vms-050]: JOB_CONTROL's pid ($JC_PID) also appears in F\$PID's walk -- the whole executive process set, not the caller's getpid()"
    else
        bad "F\$PID [vms-050]: JOB_CONTROL pid ($JC_PID) is NOT in F\$PID's set [$FPID_SET]"
    fi
    # vms-9357: the CLASSIC idiom is an ASSIGNMENT -- PID = F$PID(context) -- not a
    # WRITE expression. That path must ALSO preserve the %08X pid; it used to coerce
    # via strtol base-0, which collapses "00000001" -> 1 AND mis-reads the leading
    # zero as OCTAL ("00000067" -> 55). Assert the assigned symbol holds an 8-hex
    # executive pid (single-quoted run_cmd so the literal F$PID is not shell-expanded
    # under set -u -- same rule as the block above).
    run_cmd 'GPCTX = ""'
    run_cmd 'GPID = F$PID(GPCTX)'
    run_cmd 'WRITE SYS$OUTPUT "GPIDVAL=[" + GPID + "]"'
    local GPID_VAL
    GPID_VAL=$(printf '%s\n' "$SEG" | grep -oiE 'GPIDVAL=\[[0-9A-Fa-f]*\]' | head -1 | sed -E 's/GPIDVAL=\[([0-9A-Fa-f]*)\]/\1/')
    if printf '%s' "$GPID_VAL" | grep -qiE '^[0-9A-Fa-f]{8}$'; then
        ok "F\$PID [vms-9357]: PID = F\$PID(ctx) ASSIGNMENT preserves the 8-hex pid ($GPID_VAL) -- the classic idiom, not octal-collapsed by strtol base-0"
    else
        bad "F\$PID [vms-9357]: PID = F\$PID(ctx) assignment gave '$GPID_VAL' -- the %08X pid was mangled (int/octal coercion on the assignment path)"
    fi

    # --- SHOW PROCESS (current process works) -------------------------------
    run_cmd 'SHOW PROCESS'
    must_have "$SEG" 'SYSTEM' "SHOW PROCESS: names the current user SYSTEM"
    must_not_have "$SEG" 'IVKEYW' "SHOW PROCESS: not rejected as an invalid keyword"
    negctl    "$SEG" 'SHOW PROCESS' "SHOW PROCESS"

    # --- SHOW PROCESS/QUOTAS (REAL seeded quotas -- vms-14a wired the source) -
    # vms-050 -> vms-14a / INV-6: the quota block used to be seven HARDCODED
    # lines (CPU limit Infinite, Direct I/O 40, ...), identical for every
    # account, sourced from nowhere. #884 de-fabbed it into a reader of the
    # executive's per-process JIB quota vector, each line gated by
    # VMS_PI_V_QUOTA, and HONESTLY OMITTED the lines while no quota source
    # existed. vms-14a built that source: mksysuaf seeds SYSTEM's authorized
    # quota set into the [OVMX] SYSUAF quota region, LOGINOUT decodes it and
    # hands it to the executive (VMS_IOCTL_SETIDENT), and proc_fill_info sets
    # VMS_PI_V_QUOTA -- so the lines now print SYSTEM's REAL configured values.
    #
    # The values are oracle-grounded: docs/oracle/vax73-show-process-quotas.md
    # §4, the AUTHORIZED limits captured with F$GETJPI on a real OpenVMS VAX
    # V7.3 SYSTEM session (OVMX does not charge quotas, so remaining ==
    # authorized). NOT hardcoded in the display -- decoded from the seeded
    # SYSUAF record; change the seed and these change.
    #
    # The de-fabbed reader still emits NO "CPU limit:" line (struct
    # vms_jib_quota has no CPU-limit cell -- an honestly-omitted field, oracle
    # §5), so 'CPU limit:'/'Infinite' remain durable markers of ONLY the
    # deleted fabrication.
    run_cmd 'SHOW PROCESS/QUOTAS'
    must_have     "$SEG" 'Process Quotas:' "SHOW PROCESS/QUOTAS [vms-14a]: prints the real quota header"
    must_have     "$SEG" 'SYSTEM' "SHOW PROCESS/QUOTAS [vms-14a]: names the real account SYSTEM (from a live \$GETJPI)"
    must_match    "$SEG" 'Buffered I/O byte count quota: *47872' "SHOW PROCESS/QUOTAS [vms-14a]: real seeded BYTLM 47872 (oracle \$4)"
    must_match    "$SEG" 'Paging file quota: *40960' "SHOW PROCESS/QUOTAS [vms-14a]: real seeded PGFLQUOTA 40960 (oracle \$4)"
    must_match    "$SEG" 'AST quota: *100' "SHOW PROCESS/QUOTAS [vms-14a]: real seeded ASTLM 100 (oracle \$4)"
    must_match    "$SEG" 'Open file quota: *300' "SHOW PROCESS/QUOTAS [vms-14a]: real seeded FILLM 300 (oracle \$4)"
    must_match    "$SEG" 'Enqueue quota: *200' "SHOW PROCESS/QUOTAS [vms-14a]: real seeded ENQLM 200 (oracle \$4)"
    must_match    "$SEG" 'Subprocess quota: *10' "SHOW PROCESS/QUOTAS [vms-14a]: real seeded PRCLM 10 (oracle \$4)"
    must_not_have "$SEG" 'CPU limit:' "SHOW PROCESS/QUOTAS [vms-14a]: no fabricated 'CPU limit:' line (honestly-omitted field, oracle \$5)"
    must_not_have "$SEG" 'Infinite' "SHOW PROCESS/QUOTAS [vms-14a]: does NOT print a fabricated 'Infinite' CPU limit"
    negctl        "$SEG" 'Process Quotas' "SHOW PROCESS/QUOTAS"

    # --- SHOW WORKING_SET (de-fabbed: real WS size, no invented limits) ------
    # vms-050 / INV-6: SHOW WORKING_SET used to print
    #   Working Set  [current,quota,extent] = [8192,8192,16384]
    #   Adjustment enabled  Authorized Quota = 8192  Authorized Extent = 16384
    # where the quota DEFAULTED to a hardcoded 8192 (the DCL ctx value is 0 for
    # a real login) and the extent was an INVENTED quota*2 formula -- a
    # plausible constant and arithmetic read from the DCL context, not the
    # executive. It is now a $GETJPI reader: the current working-set size
    # (JPI$_PPGCNT) prints as the real VMS "Working Set  /Limit=" field, and the
    # /Quota, /Extent and "Adjustment ... Authorized" limits print ONLY when the
    # executive sourced the JIB quota block (VMS_PI_V_QUOTA). OVMX has no quota
    # facility yet, so those are honestly OMITTED. Here, against a LIVE
    # executive, the real "Working Set  /Limit=<n>" prints and NONE of the
    # fabricated numbers do. The markers below are things only the deleted
    # fabrication printed (the 8192 default, the quota*2 16384 extent, the old
    # "[current,quota,extent]" shape), so they stay durable once real quota
    # values are wired in.
    #
    # vms-14a wired the JIB WS quota cells: with VMS_PI_V_QUOTA now set, the
    # /Quota, /Extent and "Authorized" limits print SYSTEM's REAL seeded values
    # -- /Quota=1024, /Extent=28700 (oracle SHOW WORKING_SET, docs/oracle/
    # vax73-show-process-quotas.md \$2/\$4). /Limit is the live JPI\$_PPGCNT
    # (current WS size), which is runtime-variable, so it is asserted as a
    # number, not a fixed value. The 8192/16384 markers are the deleted
    # fabrication's constants and differ from the real 1024/28700, so they stay
    # durable bug guards.
    run_cmd 'SHOW WORKING_SET'
    must_match    "$SEG" 'Working Set +/Limit= *[0-9]+' "SHOW WORKING_SET [vms-14a]: prints the real 'Working Set  /Limit=<n>' from a live \$GETJPI"
    must_match    "$SEG" '/Quota= *1024' "SHOW WORKING_SET [vms-14a]: real seeded WSQUOTA 1024 (oracle \$2)"
    must_match    "$SEG" '/Extent= *28700' "SHOW WORKING_SET [vms-14a]: real seeded WSEXTENT 28700 (oracle \$2)"
    must_match    "$SEG" 'Authorized Quota = *1024' "SHOW WORKING_SET [vms-14a]: real Authorized Quota 1024 (oracle \$2)"
    must_match    "$SEG" 'Authorized Extent = *28700' "SHOW WORKING_SET [vms-14a]: real Authorized Extent 28700 (oracle \$2)"
    must_not_have "$SEG" '[current,quota,extent]' "SHOW WORKING_SET [vms-14a]: does NOT print the deleted fabricated '[current,quota,extent]' shape"
    must_not_have "$SEG" 'Authorized Quota = 8192' "SHOW WORKING_SET [vms-14a]: does NOT print the fabricated 8192 authorized quota"
    must_not_have "$SEG" 'Authorized Extent = 16384' "SHOW WORKING_SET [vms-14a]: does NOT print the fabricated quota*2 (16384) authorized extent"
    negctl        "$SEG" 'Working Set' "SHOW WORKING_SET"

    # --- SHOW DEFAULT (VMS filespec, no Unix path) --------------------------
    run_cmd 'SHOW DEFAULT'
    must_match    "$SEG" '[A-Z$_]+:\[[A-Z0-9._]+\]' "SHOW DEFAULT: prints a VMS device:[directory] filespec"
    must_not_have "$SEG" '/tmp' "SHOW DEFAULT: no Unix path leaks into the default directory"
    negctl        "$SEG" 'SHOW DEFAULT' "SHOW DEFAULT"

    # --- bare DIRECTORY at login lists files --------------------------------
    run_cmd 'DIRECTORY'
    must_match "$SEG" 'Total of [1-9]' "DIRECTORY: a bare DIRECTORY at login lists >= 1 file"
    must_match "$SEG" 'Directory ' "DIRECTORY: prints a VMS 'Directory <spec>' header"
    must_not_have "$SEG" '/tmp' "DIRECTORY: no Unix path leaks into the listing"
    negctl     "$SEG" 'DIRECTORY' "DIRECTORY"

    # --- vms-c38: the STANDING oracle golden-diff gate over the seeded SHOW-family
    # goldens (conductor ruling 2026-08-30, Option A). Structure-tolerant compare
    # (diff_surface structure_norm): a byte-exact column-geometry gate is impossible
    # cross-system, so it proves STRUCTURAL fidelity -- same sections/labels/headers/
    # field-structure -- value-tolerant (wider numbers, different machine strings),
    # but NOT of a missing or HOLLOW field.
    #
    # HARD-GATE only CLAIMED-FAITHFUL surfaces (regression-proof). REPORT-only the
    # not-yet-faithful ones: their classification is logged LOUD + routed to a real
    # fidelity item every run (drives the vms-050 backlog) but does NOT hard-fail the
    # required leg -- so a known gap can't permanently-red main's green-by-SHA. Round
    # 3's diff proved the divergence is MIXED (value/machine-string AND real gaps);
    # masking the real gaps green would be the exact INV-6 cheat this refuses.
    #
    # CLAIMED-FAITHFUL (hard-gated) -- proven genuinely faithful, greens through the
    # full pipeline (model/MP-state/CPU-list are machine-varying values, masked; the
    # labelled structure MATCHes the oracle; DCL-Dictionary-pinned format):
    golden_diff        vax-show-cpu
    # NOT-yet-faithful (report-only) -- real structural gaps found by round-3's diff,
    # each routed to its fidelity item; graduates to the hard-gate list on the fix:
    golden_diff_report vax-show-memory  HOLLOW  vms-352   # missing Dynamic Memory + Paging File sections (OVMX has pool+pagefile)
    golden_diff_report vax-show-system  HOLLOW  vms-6b8e  # SHOW SYSTEM omits the State/Pri/I/O columns
    golden_diff_report vax-show-device  MISSING vms-ddc   # %SYSTEM-W-NOSUCHDEV (device-name model, ties vms-9f5)
    golden_diff_report vax-show-process HOLLOW  vms-1f7   # omits Terminal/Base priority/Devices allocated; UIC not resolved to [SYSTEM]

    # =======================================================================
    # DECnet CTERM (vms-f40) -- an inbound $ SET HOST reaches an AUTHENTICATED
    # LOGINOUT prompt. Design §6-P4, ratification gates §7.1/§7.5; oracle
    # docs/oracle/vax-sethost-cterm.* (rd vms-558).
    #
    # WHY THIS RUNS HERE, IN THE BOOTED IMAGE. The property is "a remote SET
    # HOST is CHALLENGED, and REFUSED when the credentials are bad" -- which
    # only means anything against the REAL executive (/dev/vms), the REAL
    # SYS$SYSTEM:SYSUAF.DAT and the REAL LOGINOUT.EXE. On the build host there
    # is no executive, and DECNETD.EXE --cterm-accept-test says so and fails
    # (INV-6) rather than proving anything about a stub. This battery is the one
    # place that runs byte-identically on x86_64, Alpha AND the VAX rail, which
    # is also where a network login's Wall-6 ordering would break first.
    #
    # WHAT THE MODE DOES (src/vmsdecnet/engine/decnetd.c): opens a REAL NSP
    # logical link to Session Control object 42 over a socketpair datalink,
    # carrying a connect message byte-identical to the real VAX's; dispatches it
    # through $CREPRC PRC$M_INTER|PRC$M_LOGINOUT onto an executive-minted RTAn:;
    # then types three credential sets that MUST ALL BE REFUSED -- an unknown
    # account, a real account with a wrong password, and (the sharpest) the
    # DISABLED account with its CORRECT password, which only the SYSUAF DISUSER
    # rule can refuse. It also $GETDVIs the session's RTAn: from a process that
    # is NOT the session -- the §7.5 anti-LARP tell.
    #
    # IT IS RUN AS A FOREIGN COMMAND because DCL's RUN passes no arguments; that
    # is the VMS way to pass one, not a shell escape.
    #
    # HARD GATE ON ALL THREE RAILS (rd vms-c1f). DECNETD.EXE is in the x86_64
    # shipped image set (distro/Dockerfile.bootable), the VAX sysvol
    # (tests/lab-vax/stage_sysvol.sh) and the Alpha boot image
    # (tools/cross-alpha/build-alpha-bootimage.sh) -- every runtime this
    # battery drives ships the image, so the "not on this runtime" note path
    # below is UNREACHABLE in normal operation. It stays as a HARD FAILURE,
    # not a note: if a staging regression ever drops DECNETD.EXE off a rail's
    # runtime again, that is a real INV-6 hole (a shipped facility whose
    # runtime cannot prove it) and must RED here, not silently pass because
    # nothing ran.
    local CTERM_OFF; CTERM_OFF=$(wc -c <"$LOG")
    run_cmd 'DNETACC :== $SYS$SYSTEM:DECNETD.EXE'
    send 'DNETACC --cterm-accept-test'
    if wait_for 'IVIMAGE' 15 "$CTERM_OFF"; then
        bad "CTERM [vms-f40]: SYS\$SYSTEM:DECNETD.EXE is not on THIS runtime's system disk, so the inbound-SET-HOST authentication proof DID NOT RUN (rd vms-c1f staged DECNETD.EXE onto every rail's runtime -- its absence here is a staging regression, not an expected gap)"
    elif wait_for 'DECNETD-CTERM-ACCEPT:' 180 "$CTERM_OFF"; then
        local CTSEG; CTSEG=$(tail -c "+$((CTERM_OFF + 1))" "$LOG" | tr -d '\r')
        must_have "$CTSEG" 'DECNETD-CTERM-ACCEPT: PASS' \
            "CTERM [vms-f40]: an inbound SET HOST to object 42 reached an AUTHENTICATED LOGINOUT through \$CREPRC on an executive-minted RTAn:, and every bad credential was REFUSED (the mode prints one PASS/FAIL line per assertion above this verdict)"
        must_have "$CTSEG" 'the inbound SET HOST is CHALLENGED' \
            "CTERM [vms-f40]: LOGINOUT's own Username: prompt crossed the link -- the no-auth CTERM that answered with a bare \$ is gone"
        must_have "$CTSEG" 'DISUSER IS HONOURED' \
            "CTERM [vms-f40]: the DISABLED account is refused over CTERM with the CORRECT password -- the SYSUAF login-flag rule applies to a network login"
        must_have "$CTSEG" 'a real DC$_TERM device row' \
            "CTERM [vms-f40]: \$GETDVI on the session's RTAn: from a DIFFERENT process returns a real device row (§7.5 tell)"
        must_not_have "$CTSEG" 'DECNETD-CTERM-ACCEPT: FAIL' \
            "CTERM [vms-f40]: no assertion in the inbound-SET-HOST acceptance failed"
        negctl "$CTSEG" 'DECNETD-I-CTERMACCEPT' "DECnet CTERM acceptance"
    else
        bad "CTERM [vms-f40]: DECNETD.EXE --cterm-accept-test produced no verdict line within 180s -- the inbound-SET-HOST authentication proof did not run (a missing DECNETD.EXE, an absent /dev/vms, or a hung session)"
    fi
    wait_for '$ ' 20 "$CTERM_OFF"

    # SCOPE NOTE (rd vms-c1f): FAL's own auth proof (--fal-accept-test, rd
    # vms-8c2/#1096) is NOT yet in this battery -- #1096 had not merged as of
    # this section's hard-gate flip. DECNETD.EXE is now staged on all three
    # rails, so once #1096 lands a FAL section here needs no rail-staging work
    # of its own -- follow the SAME shape as CTERM above (assert on the verdict
    # line, IVIMAGE means a real staging regression, never a silent note).

    # =======================================================================
    # DECnet NETACP ISOLATION (vms-9ab, P5) -- the A2/A8 security seam. Design
    # vms-515 §3.4: attacker-controlled wire parsing runs at LOW privilege and
    # hands NETACP's thin privileged control path only a VALIDATED, TYPED
    # descriptor; the privileged path parses no attacker bytes. This mode is the
    # NEGATIVE proof of that seam and needs NEITHER /dev/vms NOR CAP_NET_RAW --
    # every case is refused at NETACP's privileged front door BEFORE it would
    # mint a device or create a process, so it never depended on an executive
    # either way. Hard gate on all three rails (rd vms-c1f), same as CTERM above.
    local ISOL_OFF; ISOL_OFF=$(wc -c <"$LOG")
    send 'DNETACC --isolation-test'
    if wait_for 'IVIMAGE' 15 "$ISOL_OFF"; then
        bad "NETACP isolation [vms-9ab]: SYS\$SYSTEM:DECNETD.EXE is not on THIS runtime's system disk, so the A2/A8 privileged-path isolation proof DID NOT RUN (rd vms-c1f staged DECNETD.EXE onto every rail's runtime -- its absence here is a staging regression, not an expected gap)"
    elif wait_for 'DECNETD-ISOLATION:' 60 "$ISOL_OFF"; then
        local ISSEG; ISSEG=$(tail -c "+$((ISOL_OFF + 1))" "$LOG" | tr -d '\r')
        must_have "$ISSEG" 'DECNETD-ISOLATION: PASS' \
            "NETACP isolation [vms-9ab]: the privileged control path refuses an UNVALIDATED or wrong-object descriptor with no device/process created, and every parse-rejected fuzz frame is refused there too (the A2/A8 seam holds on the shipped binary)"
        must_have "$ISSEG" 'double-door fuzz' \
            "NETACP isolation [vms-9ab]: a mutation-fuzz corpus the low-priv parse rejects is ALSO refused by the privileged path -- a hostile frame cannot reach session creation"
        must_not_have "$ISSEG" 'DECNETD-ISOLATION: FAIL' \
            "NETACP isolation [vms-9ab]: no isolation assertion failed"
        negctl "$ISSEG" 'DECNETD-I-ISOLATION' "DECnet NETACP isolation"
    else
        bad "NETACP isolation [vms-9ab]: DECNETD.EXE --isolation-test produced no verdict line within 60s"
    fi
    wait_for '$ ' 20 "$ISOL_OFF"

    # =======================================================================
    # DECnet FILE COPY / FAL (vms-8c2) -- an inbound $ COPY node"user pw"::file
    # authenticates the connect-carried credentials against the REAL SYSUAF and
    # then moves a sequential file through DAP over the NSP link and RMS over the
    # ODS-2 ACP, BOTH directions, byte-verified. Oracle
    # docs/oracle/vax-copy-fal-dap.* (rd vms-cd3).
    #
    # WHY THIS RUNS HERE. The property is "a COPY with a BAD password is REFUSED,
    # a COPY with the RIGHT password transfers the file, and the received bytes
    # match the source" -- which only means anything against the REAL executive
    # (/dev/vms), the REAL SYS$SYSTEM:SYSUAF.DAT (the seeded GUEST + DISABLED
    # accounts) and REAL RMS on the mounted ODS-2 volume. On the build host there
    # is no executive; DECNETD.EXE --fal-accept-test's auth checks then fail
    # (INV-6) rather than proving anything about a stub. The honest floor -- a
    # real object-17 connect carrying the creds, refused with an NSP disconnect
    # when unauthenticated, plus the DAP transport pump -- is proven with no
    # executive by --fal-selftest and by tests/vmsdecnet/test_dnet_dap (the DAP
    # codec + FAL credential decoder, fuzzed ASan-clean).
    #
    # WHAT THE MODE DOES (src/vmsdecnet/engine/decnetd.c): authenticates the seed
    # accounts through sysuaf_authenticate (Purdy) -- GUEST/GUEST accepted, a
    # wrong password and DISABLED (DISUSER) refused; then opens a REAL object-17
    # NSP link over a socketpair carrying the access-control creds, and runs the
    # FAL server + COPY client (two threads) to PUT then GET a sequential file
    # through DAP + RMS, byte-verifying the transferred records.
    #
    # HARD GATE where DECNETD.EXE ships (x86_64 image); a LOUD note where absent
    # (the VAX/Alpha staging follow-on, same as CTERM). Never green because
    # nothing ran.
    local FAL_OFF; FAL_OFF=$(wc -c <"$LOG")
    send 'DNETACC --fal-accept-test'
    if wait_for 'IVIMAGE' 15 "$FAL_OFF"; then
        note "FAL COPY [vms-8c2]: SYS\$SYSTEM:DECNETD.EXE is not on THIS runtime's system disk, so the inbound-FAL COPY authentication + transfer proof DID NOT RUN here (hard gate on the rails that ship the image; staging into the VAX sysvol + Alpha boot image is tracked follow-on)"
    elif wait_for 'DECNETD-FAL-ACCEPT:' 180 "$FAL_OFF"; then
        local FALSEG; FALSEG=$(tail -c "+$((FAL_OFF + 1))" "$LOG" | tr -d '\r')
        must_have "$FALSEG" 'DECNETD-FAL-ACCEPT: PASS' \
            "FAL COPY [vms-8c2]: inbound FAL authenticated the connect creds against the real SYSUAF and transferred a sequential file both directions through DAP + RMS, byte-verified (one PASS/FAIL line per assertion above this verdict)"
        must_have "$FALSEG" 'is REFUSED (SS$_INVLOGIN) -- a fake would pass it' \
            "FAL COPY [vms-8c2]: a wrong password is REFUSED by real SYSUAF/Purdy -- a fake auth would have admitted it"
        must_have "$FALSEG" 'a right password is not sufficient' \
            "FAL COPY [vms-8c2]: DISABLED (correct password, DISUSER) is refused -- the SYSUAF login-flag rule applies to a network file access"
        must_have "$FALSEG" 'BYTE-MATCH the source' \
            "FAL COPY [vms-8c2]: the transferred file's records byte-match the source (a real transfer through RMS over the ACP, both directions)"
        must_not_have "$FALSEG" 'DECNETD-FAL-ACCEPT: FAIL' \
            "FAL COPY [vms-8c2]: no assertion in the inbound-FAL COPY acceptance failed"
        negctl "$FALSEG" 'DECNETD-I-FALACCEPT' "DECnet FAL COPY acceptance"
    else
        bad "FAL COPY [vms-8c2]: DECNETD.EXE --fal-accept-test produced no verdict line within 180s -- the inbound-FAL COPY proof did not run (a missing DECNETD.EXE, an absent /dev/vms, or a hung transfer)"
    fi
    wait_for '$ ' 20 "$FAL_OFF"

    # The DECnet device FACE _NET: is executive-resident and cross-process real
    # (vms-9ab, P5; design §2b/§7.5). $GETDVI it from DCL -- a process that is
    # NOT NETACP -- and it resolves; the deep cross-process assertions (class,
    # normalization, unowned) live in tests/qemu/test_kmod_devtab.c on the kmod
    # leg. Gated on the node having a NIC (INV-6): where ETH0: exists _NET: does.
    local NETDEV_OFF; NETDEV_OFF=$(wc -c <"$LOG")
    run_cmd 'IF F$GETDVI("_NET:","EXISTS") THEN WRITE SYS$OUTPUT "OVMX-NET-FACE: _NET: EXISTS"'
    run_cmd 'IF .NOT. F$GETDVI("_NET:","EXISTS") THEN WRITE SYS$OUTPUT "OVMX-NET-FACE: _NET: ABSENT"'
    local NETSEG; NETSEG=$(tail -c "+$((NETDEV_OFF + 1))" "$LOG" | tr -d '\r')
    if printf '%s' "$NETSEG" | grep -q 'OVMX-NET-FACE: _NET: EXISTS'; then
        ok "NETACP device face [vms-9ab]: \$GETDVI _NET: from DCL (a non-NETACP process) resolves a real executive device -- the DECnet device face is cross-process real (§7.5 tell)"
    elif printf '%s' "$NETSEG" | grep -q 'OVMX-NET-FACE: _NET: ABSENT'; then
        note "NETACP device face [vms-9ab]: _NET: is ABSENT on this runtime -- honest only if this node has no primary NIC (INV-6: no NIC, no DECnet device face). If ETH0: exists here this is a FAILURE the kmod-leg test_kmod_devtab will red."
    else
        note "NETACP device face [vms-9ab]: F\$GETDVI _NET: produced no OVMX-NET-FACE line (older DCL F\$GETDVI EXISTS item, or no /dev/vms) -- the authoritative cross-process proof is test_kmod_devtab on the kmod leg"
    fi
    wait_for '$ ' 20 "$NETDEV_OFF"

    # =======================================================================
    # SESSION PRIMITIVE (vms-3e9) -- $CREPRC creates the session, LOGINOUT
    # re-personas it. Design record docs/design/faithful-sessions-and-network-
    # subsystems.md §3.1/§6-P1, ratification gates §7.1/§7.5.
    #
    # WHY THIS SECTION IS HERE AND NOT IN AN x86_64-ONLY TEST. The property it
    # proves is ASYMMETRIC ACROSS ARCHES (the Wall-6 trap, vms-d4ef): the
    # created process must establish its system identity BEFORE LOGINOUT reads
    # the World-denied SYS$SYSTEM:SYSUAF.DAT. On x86_64 a violation is
    # INVISIBLE -- root maps to UIC group 0, which is <= MAXSYSGROUP and reads
    # SYSUAF by luck of the environment -- while on the VAX rail the ACP denies
    # the read RMS$_PRV and login stops working. This battery is the ONE
    # place that runs byte-identically on x86_64, Alpha AND the VAX rail
    # (tests/lab-vax/test_dcl_acceptance_vax.sh), so the proof lives here.
    #
    # WHAT IT ACTUALLY PROVES, and why a fake could not pass it:
    #   - The console session is created ANEW after a logout. The battery has
    #     been driving the session JOB_CONTROL created at boot; LOGOUT ends
    #     that process, and a second "Username:" can only appear because
    #     JOB_CONTROL went round its loop and issued $CREPRC again. There is
    #     no fork+execl left in that program to produce one another way.
    #   - A NON-SYSTEM SYSUAF account (GUEST, [128,129] = octal [200,201],
    #     TMPMBX only, password GUEST -- tools/mksysuaf.c's seed) logs in and
    #     the process reports THAT ACCOUNT'S identity. The creator (JOB_CONTROL)
    #     is SYSTEM and passes no identity at all: if $CREPRC were stamping the
    #     creator's identity, or if LOGINOUT were not re-personaing through the
    #     guarded persona primitive, every one of these would still say SYSTEM.
    #     That is the re-persona proof, and it is exactly the thing the
    #     installed-privileged-LOGINOUT model exists to make possible.
    #   - SHOW PROCESS and SHOW PROCESS/PRIVILEGES read the EXECUTIVE's row
    #     ($GETJPI), not the session's own idea of itself, so the identity is
    #     a fact other processes can see and not a self-description.
    #   - SHOW TERMINAL is the $GETDVI leg: it reads JPI$_TERMINAL off the
    #     executive row -- which is there only because $CREPRC's terminal
    #     binding did $ASSIGN + SETTERM inside the created process -- and then
    #     $GETDVIs THAT NAME to render the device row. A session with no
    #     executive device prints nothing here, which is the design's §7.5
    #     LARP tell in its console form.
    # =======================================================================
    local SESS_OFF; SESS_OFF=$(wc -c <"$LOG")
    send 'LOGOUT'
    if wait_for 'logged out at' 30 "$SESS_OFF"; then
        ok "SESSION [vms-3e9]: the SYSTEM session logs out (LOGINOUT's session ends, so the creating loop comes round)"
    else
        bad "SESSION [vms-3e9]: LOGOUT never completed -- the rest of this section cannot run"
    fi

    # A fresh console session: LOGINOUT on OPA0: waits for the operator's
    # RETURN, so feed a CR each second exactly as the boot loop above does.
    local RELOGIN_OFF; RELOGIN_OFF=$(wc -c <"$LOG")
    local rw=0
    until tail -c "+$((RELOGIN_OFF + 1))" "$LOG" 2>/dev/null | grep -qaF 'Username:' \
          || [ "$rw" -ge 60 ]; do
        send ''; sleep 1; rw=$((rw + 1))
    done
    if wait_for 'Username:' 10 "$RELOGIN_OFF"; then
        ok "SESSION [vms-3e9]: a SECOND console session reaches Username: -- JOB_CONTROL created it with \$CREPRC (PRC\$M_INTER|PRC\$M_LOGINOUT); it has no fork+execl left to do it any other way"
    else
        bad "SESSION [vms-3e9]: no second Username: prompt after logout -- the \$CREPRC session-creation loop did not come round"
    fi

    # --- log in as a NON-SYSTEM SYSUAF account ------------------------------
    local GUEST_OFF; GUEST_OFF=$(wc -c <"$LOG")
    send 'GUEST'
    wait_for 'Password:' 30 "$GUEST_OFF" && send 'GUEST'
    if wait_for 'Welcome to OpenVMX' 30 "$GUEST_OFF"; then
        ok "SESSION [vms-3e9]: the NON-SYSTEM account GUEST authenticates against SYSUAF and reaches a session (the Wall-6 ordering held: the created process was system-identified BEFORE the SYSUAF read -- on the VAX rail this is the whole proof)"
    else
        bad "SESSION [vms-3e9]: GUEST could not log in. On the VAX rail this is the Wall-6 regression (establish_system after the SYSUAF read -> RMS\$_PRV -> 'User authorization failure'); on x86_64 it is a real login break the root->group-0 crutch would normally have hidden"
    fi
    wait_for '$ ' 20 "$GUEST_OFF"

    # --- the re-persona proof: the process reports GUEST, not its creator ---
    run_cmd 'SHOW PROCESS'
    must_match    "$SEG" 'User: *GUEST' "SHOW PROCESS [vms-3e9]: the session's executive row carries the AUTHENTICATED user GUEST"
    must_not_have "$SEG" 'User: SYSTEM' "SHOW PROCESS [vms-3e9]: it does NOT report its creator's identity (JOB_CONTROL is SYSTEM; a creator-stamped or un-re-personaed session would say SYSTEM here)"
    # GUEST's UIC is [128,129] decimal = [200,201] octal. SHOW PROCESS resolves
    # it through the rights list where it can ([GUEST]) and prints the octal
    # [group,member] where it cannot -- both are GUEST's UIC and neither is
    # SYSTEM's [1,4]/[001,004], which is what this asserts.
    must_match    "$SEG" 'User Identifier: *\[(GUEST|200,201)\]' "SHOW PROCESS [vms-3e9]: the UIC is GUEST's -- [GUEST] resolved, or the octal [200,201] = decimal [128,129] (tools/mksysuaf.c seed)"
    must_not_have "$SEG" '[001,004]' "SHOW PROCESS [vms-3e9]: the UIC is NOT SYSTEM's [1,4]"
    negctl        "$SEG" 'Process ID' "SHOW PROCESS as GUEST"

    run_cmd 'SHOW PROCESS/PRIVILEGES'
    # GUEST is authorized TMPMBX and nothing else. SYSTEM holds PRV$M_ALL, so
    # every one of these would be listed if the session were still its creator
    # -- each is a privilege GUEST must not have.
    must_have     "$SEG" 'Process privileges' "SHOW PROCESS/PRIVILEGES [vms-3e9]: prints the executive-held privilege blocks"
    local _p
    for _p in SETPRV SYSPRV BYPASS CMKRNL CMEXEC WORLD; do
        must_not_have "$SEG" " $_p " "SHOW PROCESS/PRIVILEGES [vms-3e9]: GUEST does NOT hold $_p (SYSTEM's mask did not survive the re-persona)"
    done
    negctl        "$SEG" 'privileges' "SHOW PROCESS/PRIVILEGES as GUEST"

    # --- the $GETDVI leg: the session's terminal is a REAL executive device --
    run_cmd 'SHOW TERMINAL'
    must_match "$SEG" 'Terminal: *_OPA0:' "SHOW TERMINAL [vms-3e9]: \$GETDVI on the terminal the executive recorded for this session returns the real OPA0: device row (\$CREPRC's \$ASSIGN+SETTERM ran inside the created process; a session with no executive device prints nothing here -- the §7.5 tell)"
    must_have  "$SEG" 'Device_Type' "SHOW TERMINAL [vms-3e9]: the device row's own fields are rendered, not a name echoed back"
    must_match "$SEG" 'Owner: *GUEST' "SHOW TERMINAL [vms-3e9]: the DEVICE row's owner resolves to the re-personaed session -- a second, cross-object read of the same identity"
    negctl     "$SEG" 'Terminal:' "SHOW TERMINAL as GUEST"

    return 0
}
