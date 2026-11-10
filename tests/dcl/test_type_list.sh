#!/bin/bash
# TEST: TYPE types EVERY file of a comma / plus list, in order (rd vms-691d).
#
# Observed on OpenVMS VAX V7.3 (tests/lab/captures/vms-691d-type-spawn-20261102):
#   $ TYPE TA.TXT,TB.TXT          -> TA's records bare, then " ", TB's full
#                                    name, " ", then TB's records; $STATUS %X10000001
#   $ TYPE TA.TXT,NOSUCH.TXT,TB.TXT -> TA, %TYPE-W-SEARCHFAIL + -RMS-E-FNF for the
#                                    missing one, then TB with its header;
#                                    $STATUS %X10951238
#   $ TYPE NOSUCH.TXT              -> $STATUS %X10951238
#   $ TYPE TA.TXT,TB               -> TB takes TA's file type (TB.TXT)
#   $ TYPE TA.TXT+TB.TXT           -> as the comma list
# OVMX V0.7-14 typed only the first file of a list (vms-ba2 then typed a comma
# list); this pins the rest of the VAX behaviour.
#
# The stdout sequence is checked line by line below (stdout and stderr reach
# the harness separately buffered, so their interleaving is not checked).
# EXPECT: contains:SEQUENCE-COMMA-OK
# EXPECT: contains:SEQUENCE-MISSING-OK
# EXPECT: contains:SEQUENCE-STICKY-OK
# EXPECT: contains:SEQUENCE-PLUS-OK
# EXPECT: contains:STATUS-LIST-SUCCESS
# EXPECT: contains:STATUS-MISSING=%X10951238
# EXPECT: contains:STATUS-SINGLE=%X10951238
# EXPECT: contains:%TYPE-W-SEARCHFAIL, error searching for
# EXPECT: contains:NOSUCH.TXT;
# EXPECT: contains:-RMS-E-FNF, file not found
#
# TRIPWIRE: drop the '+' split or plist_sticky_type() from exec_plist()
# (src/vmsdcl/dcl_exec.c) and SEQUENCE-PLUS / SEQUENCE-STICKY disappear; print
# empty header lines in type_list_heading() (src/vmsdcl/dcl_cmd_file.c) and
# every SEQUENCE fails on line 2; return SS$_NOSUCHFILE again and the STATUS
# lines read %X00000910.
VMSDCL="${VMSDCL:-vmsdcl}"
# The files live on the volume RMS reaches (TYPE opens them by RMS $OPEN, rd
# vms-2cbe), named by a VMS directory, not a host path; each has its version,
# as every file on a Files-11 volume does (the header shows TB.TXT;1).
TNAME="dcl_tlist_$$"
TDIR="/vms/$TNAME"
VDIR="$(echo "$TNAME" | tr a-z A-Z)"
mkdir -p "$TDIR"
trap 'rm -rf "$TDIR"' EXIT
printf 'alpha one\nalpha two\n' > "$TDIR/ta.txt;1"
printf 'bravo one\n' > "$TDIR/tb.txt;1"

run() {
    printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\n%s\n' "$VDIR" "$1" | $VMSDCL 2>/dev/null
}

# expected stdout: the lines given, where "@TB" / "@TA" is a header name line
# ending in TB.TXT;<ver> / TA.TXT;<ver>
check_seq() {
    local label=$1 got=$2; shift 2
    local i=0 line
    local -a lines
    mapfile -t lines <<< "$got"
    for want in "$@"; do
        line=${lines[$i]-<missing>}
        case "$want" in
        @*) [[ "$line" =~ ${want#@}\.TXT\;[0-9]*$ ]] || { echo "$label: line $i '$line' is not the ${want#@} header"; return; } ;;
        *)  [ "$line" = "$want" ] || { echo "$label: line $i '$line' != '$want'"; return; } ;;
        esac
        i=$((i + 1))
    done
    [ "${#lines[@]}" -eq "$i" ] || { echo "$label: ${#lines[@]} lines, want $i"; return; }
    echo "SEQUENCE-$label-OK"
}

out=$(run 'TYPE TA.TXT,TB.TXT')
echo "$out"
check_seq COMMA "$out" 'alpha one' 'alpha two' ' ' '@TB' ' ' 'bravo one'

out=$(run 'TYPE TA.TXT,NOSUCH.TXT,TB.TXT')
echo "$out"
check_seq MISSING "$out" 'alpha one' 'alpha two' ' ' '@TB' ' ' 'bravo one'

out=$(run 'TYPE TA.TXT,TB')
check_seq STICKY "$out" 'alpha one' 'alpha two' ' ' '@TB' ' ' 'bravo one'

out=$(run 'TYPE TA.TXT+TB.TXT')
check_seq PLUS "$out" 'alpha one' 'alpha two' ' ' '@TB' ' ' 'bravo one'

st=$(run $'TYPE TA.TXT,TB.TXT\nIF $STATUS THEN WRITE SYS$OUTPUT "STATUS-LIST-SUCCESS"' | grep '^STATUS-')
echo "$st"
st=$(run $'TYPE TA.TXT,NOSUCH.TXT,TB.TXT\nWRITE SYS$OUTPUT "STATUS-MISSING=",$STATUS' | grep '^STATUS-')
echo "$st"
st=$(run $'TYPE NOSUCH.TXT\nWRITE SYS$OUTPUT "STATUS-SINGLE=",$STATUS' | grep '^STATUS-')
echo "$st"

# the error lines themselves (stderr)
printf 'SET DEFAULT SYS$SYSDEVICE:[%s]\nTYPE TA.TXT,NOSUCH.TXT\n' "$VDIR" | $VMSDCL 2>&1 >/dev/null
rm -rf "$TDIR"
