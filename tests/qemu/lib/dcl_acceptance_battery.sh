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

# --- assertion helpers (arch-independent; operate on a captured console SEGMENT)
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
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

# run_dcl_acceptance_battery -- the login + basic-command battery + assertions.
# See the caller-provided contract above for the primitives/vars it requires.
run_dcl_acceptance_battery() {
    local BOOT_TIMEOUT="${BOOT_TIMEOUT:-180}"

    # --- Boot the real runtime to the login prompt --------------------------
    if wait_for '%OVMX-I-EXEC' 60; then ok "executive attached (real vms.ko)"; else bad "executive never attached"; fi
    # vms-2213: LOGINOUT on OPA0: waits for the operator's RETURN; feed a CR each
    # second (as a real operator would) until Username: appears.
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
    negctl        "$SEG" 'SHOW USERS' "SHOW USERS"

    # --- SHOW DEVICE DKA0: (vms-e6f: shipped bare "Online", no Mounted/label)
    run_cmd 'SHOW DEVICE DKA0:'
    # The DKA0: DATA line, not the echoed command 'SHOW DEVICE DKA0:' (which also
    # contains 'DKA0'): exclude any line naming the SHOW verb.
    local DKA0_LINE; DKA0_LINE=$(printf '%s\n' "$SEG" | grep -i 'DKA0:' | grep -iv 'SHOW ' | head -1)
    must_have  "$SEG" 'DKA0' "SHOW DEVICE DKA0: [vms-e6f]: names the device DKA0:"
    must_have  "$DKA0_LINE" 'Mounted' "SHOW DEVICE DKA0: [vms-e6f]: device status is 'Mounted' (NOT bare 'Online')"
    must_have  "$DKA0_LINE" "$VOLUME_LABEL" "SHOW DEVICE DKA0: [vms-e6f]: shows the volume label '$VOLUME_LABEL'"
    must_match "$DKA0_LINE" '[1-9][0-9]{3,}' "SHOW DEVICE DKA0: [vms-e6f]: shows a nonzero free-block count (128MB ODS-2 volume has thousands free)"
    must_not_have "$DKA0_LINE" 'Online' "SHOW DEVICE DKA0: [vms-e6f]: DKA0: status is not the bare 'Online' bug"
    negctl     "$SEG" 'SHOW DEVICE' "SHOW DEVICE DKA0:"

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
    run_cmd 'WRITE SYS$OUTPUT "GETDVIEXIST=" + F$GETDVI("DKA0:","EXISTS")'
    must_have     "$SEG" 'GETDVIEXIST=TRUE' "F\$GETDVI EXISTS [vms-050]: the real system disk DKA0: exists -> TRUE, from the executive device table"
    negctl        "$SEG" 'GETDVIEXIST' "F\$GETDVI EXISTS(real)"

    run_cmd 'WRITE SYS$OUTPUT "GETDVIBOGUS=" + F$GETDVI("ZZZ999:","EXISTS")'
    must_have     "$SEG" 'GETDVIBOGUS=FALSE' "F\$GETDVI EXISTS [vms-050]: a nonexistent device (ZZZ999:) -> honest FALSE (NOT the old unconditional TRUE)"
    must_not_have "$SEG" 'GETDVIBOGUS=TRUE' "F\$GETDVI EXISTS [vms-050]: bogus device is NOT fabricated as existing"
    negctl        "$SEG" 'GETDVIBOGUS' "F\$GETDVI EXISTS(bogus)"

    run_cmd 'WRITE SYS$OUTPUT "GETDVIVOL=" + F$GETDVI("DKA0:","VOLNAM")'
    must_have     "$SEG" "GETDVIVOL=$VOLUME_LABEL" "F\$GETDVI VOLNAM [vms-050]: reports the REAL mounted ODS-2 label '$VOLUME_LABEL' (same value SHOW DEVICE read above), not a fabricated constant"
    negctl        "$SEG" 'GETDVIVOL' "F\$GETDVI VOLNAM"

    run_cmd 'WRITE SYS$OUTPUT "GETDVICLS=" + F$GETDVI("DKA0:","DEVCLASS")'
    must_have     "$SEG" 'GETDVICLS=1' "F\$GETDVI DEVCLASS [vms-050]: DKA0: is DC\$_DISK (1) from the executive, not a name-substring guess"
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
    must_have     "$SEG" 'DKA0' "SHOW DEVICES [vms-9344]: plural form is accepted and lists devices"
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
    # VMS-faithful: either the real current UIC ([1,4] for SYSTEM) OR an honest
    # %SYSTEM-F-NODISKQUOTA when no quota is enabled -- NOT a fabricated UIC.
    # Accept [1,4] or the zero-padded [001,004] form OVMX prints elsewhere.
    must_match    "$SEG" '(\[0*1,0*4\]|NODISKQUOTA)' "SHOW QUOTA [vms-73c4]: shows the real SYSTEM UIC [1,4] OR an honest %SYSTEM-F-NODISKQUOTA"
    must_not_have "$SEG" '[200,1]' "SHOW QUOTA [vms-73c4]: does NOT print the fabricated UIC '[200,1]' (the shipped bug)"
    negctl        "$SEG" 'SHOW QUOTA' "SHOW QUOTA"

    # --- SHOW SYSTEM (real processes + distinct-pid set + honest accounting) ---
    # The golden the SHOW-SYSTEM-accounting work (vms-f62/#887) defers to, grounded
    # on the OpenVMS VAX V7.3 oracle (docs/oracle/vax73-show-system-process.md).
    # This asserts the executive-sourced facts that are real: distinct pids (#883),
    # real Uptime (#887), and -- now that vms-6cac wired the VAX host-task accounting
    # -- real CPU time (calcru) and Page faults (rulwps live-LWP aggregation), the
    # columns #887 had honestly blanked on VAX. Those two are ARCH-COMMON: x86_64/
    # Alpha sourced them all along, VAX now does too. It does NOT assert the "Pages"
    # (rss) column -- real on x86_64/Alpha, still honestly omitted on VAX until the
    # uvm-TU bind (vms-601), so asserting it either way would fail one arch. It
    # asserts NO State/Pri column on ANY arch: the executive holds no VMS scheduler
    # state, and fabricating one is the exact tell INV-6 forbids.
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
    # faults = the integer immediately AFTER the CPU time. Anchor on the CPU field,
    # NOT the trailing number: on x86_64/Alpha a Pages number follows faults, so the
    # trailing number there is Pages (VAX omits Pages, so faults IS its trailing one).
    SYS_FLTS=$(printf '%s' "$SYS_ROW" | grep -oE '[0-9]{2}:[0-9]{2}:[0-9]{2}\.[0-9]{2}[[:space:]]+[0-9]+' | grep -oE '[0-9]+$')
    if [ -n "$SYS_FLTS" ] && [ "$SYS_FLTS" -gt 0 ] 2>/dev/null; then
        ok "SHOW SYSTEM [vms-6cac]: SYSTEM shows a NON-ZERO Page-fault count ($SYS_FLTS) -- rulwps live-LWP aggregation (not p_ru's dead-LWP ~0)"
    else
        bad "SHOW SYSTEM [vms-6cac]: Page-flts zero/blank for SYSTEM [row='$SYS_ROW'] -- faults not live-aggregated"
    fi
    # vms-f62 honest omission (INV-6): the executive has no VMS scheduler state, so
    # SHOW SYSTEM prints NO State/Pri column on ANY arch -- fabricating one is the
    # same tell the accounting de-fab kills. (State/Pri are NOT in vms-6cac's scope:
    # it wired accounting, not a scheduler; they stay omitted, not "coming later".)
    must_not_have "$SEG" 'State' "SHOW SYSTEM [vms-f62]: no fabricated State column (executive holds no VMS scheduler state -- permanent honest omission)"
    negctl     "$SEG" 'SHOW SYSTEM' "SHOW SYSTEM"

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

    return 0
}
