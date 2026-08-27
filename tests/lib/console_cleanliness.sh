# shellcheck shell=sh
# tests/lib/console_cleanliness.sh -- the shared "faithful boot console" contract
# (rd vms-603, authenticity vms-898). A real VMS boot console shows ONLY the
# VMS-faithful sequence (OpenVMX/SYSBOOT banner -> startup phases -> login banner
# -> Username:); it NEVER shows the host kernel's boot log. Leaking Linux/NetBSD
# dmesg, printk, runs of empty newlines, or Unix mount/autoconf text onto the
# console is a substrate leak (INV-6 / authenticity).
#
# ONE contract, sourced by BOTH gates so they cannot drift:
#   * Linux: tests/qemu/test_boot_conformance.sh   (substrate "linux")
#   * VAX:   tests/lab-vax/run-boot.sh gate mode    (substrate "vax")
# The forbidden-pattern SET and the blank-line-flood rule are shared; each
# substrate contributes only its own host-kernel probe strings.
#
# CRUX (vms-603): grep the FULL RAW console transcript, never a stream a boot
# driver already filtered through milestone-matching -- a gate that reads the
# same skipped stream cannot see the spam it skips.

# Shared, substrate-neutral forbidden regex: a host kernel dmesg timestamp
# "[   41.332690]" -- the single most reliable "this is a Unix kernel log" tell.
CONSOLE_FORBID_SHARED='\[[[:space:]]*[0-9][0-9]*\.[0-9][0-9]*\]'

# More than this many CONSECUTIVE blank lines is an empty-newline flood (the
# operator's "assload of empty newlines"), not the odd separator VMS itself emits.
CONSOLE_MAX_BLANK_RUN=3

# console_forbid_for_substrate <substrate> -> one ERE per line (host-kernel tells)
console_forbid_for_substrate() {
    case "$1" in
    linux)
        # OVMX Linux kmods print via pr_info with these prefixes; they belong in
        # OPERATOR.LOG (opcom_kmsg bridge), never on the console (vms-300).
        printf '%s\n' \
            '^vms: ' \
            '^vmsfs: '
        ;;
    vax)
        # NetBSD/vax boot chatter: the copyright+version+memory banner (now gated
        # in the OVMX kernel under AB_QUIET), autoconf device probes, and the
        # root/swap mount lines. If any of these reach the console the boot is a
        # Unix dmesg, not a VMS boot.
        printf '%s\n' \
            '^NetBSD [0-9]' \
            'The NetBSD Foundation' \
            '^total memory =' \
            '^avail memory =' \
            '^root on ' \
            '^swap on ' \
            ' at mainbus0' \
            ' at uba0' \
            ' at cpu0' \
            'Detecting hardware'
        ;;
    *)
        echo "console_forbid_for_substrate: unknown substrate '$1'" >&2
        return 2
        ;;
    esac
}

# assert_console_clean <raw-transcript-file> <substrate>
#   0 = clean (no substrate leak); 1 = leak (offending lines printed to stderr).
# Reads the FULL raw transcript (CR-normalized), fails on any forbidden host-
# kernel pattern OR a blank-line run over the cap.
assert_console_clean() {
    _cc_log="$1"; _cc_sub="$2"; _cc_rc=0
    [ -r "$_cc_log" ] || { echo "assert_console_clean: cannot read '$_cc_log'" >&2; return 2; }

    _cc_clean="$(tr -d '\r' < "$_cc_log")"

    # Combine the shared + substrate forbidden regexes into one alternation.
    _cc_re="$( { console_forbid_for_substrate "$_cc_sub"; printf '%s\n' "$CONSOLE_FORBID_SHARED"; } \
               | grep -v '^$' | paste -sd'|' - )"
    if printf '%s\n' "$_cc_clean" | grep -qE "$_cc_re"; then
        echo "CONSOLE-LEAK ($_cc_sub): host-substrate text on the boot console --" >&2
        printf '%s\n' "$_cc_clean" | grep -nE "$_cc_re" | head -20 >&2
        _cc_rc=1
    fi

    # Blank-line flood: longest run of blank (whitespace-only) lines.
    _cc_maxblank="$(printf '%s\n' "$_cc_clean" \
        | awk '/^[[:space:]]*$/ { r++; if (r > m) m = r; next } { r = 0 } END { print m + 0 }')"
    if [ "${_cc_maxblank:-0}" -gt "$CONSOLE_MAX_BLANK_RUN" ]; then
        echo "CONSOLE-LEAK ($_cc_sub): ${_cc_maxblank} consecutive blank lines (> ${CONSOLE_MAX_BLANK_RUN}) -- empty-newline flood" >&2
        _cc_rc=1
    fi

    return "$_cc_rc"
}
