# weak-seams.sh - sourced by tools/cross-vax/build-vmsdcl-vax.sh (rd vms-1f7).
#
# DCL.EXE ONLY, on purpose. Forcing the same seams into STARTUP, PROVISION,
# JOB_CONTROL and LOGINOUT bound code paths those images had never run on the
# VAX, and PROVISION.EXE then failed the boot on a 16 MB VAX (ci-vax 16 MB leg,
# run 41383611966: %OVMX-I-EXECINIT, SYS$SYSTEM:PROVISION.EXE failed). Each of
# those images gets its seams back only with a boot proof at 16 MB (rd vms-20c9).
#
# libvms reaches LIBVMSRMS through `#pragma weak' declarations (rightslist.c,
# sysuaf.c, rms_textfile.c, sys_security.c, ...) so a bare LIBVMS unit test links
# without LIBVMSRMS and takes the honest miss. In a STATIC-ARCHIVE link a weak
# undefined reference never pulls its archive member in: unless the image also
# references the symbol strongly it stays NULL. That is how the VAX disk's DCL
# shipped with 20 seams NULL -- SHOW PROCESS printed [001,004], F$IDENTIFIER
# answered 0 (tests/lab/captures/vax-uic-identifier-20261102/).
#
#   seam_flags        sets FORCE_SEAMS (every weak symbol libvms.a leaves
#                     undefined that libvmsrms.a DEFINES, found by nm, never
#                     listed by hand) and SEAM_FLAGS (-Wl,--undefined=<each>).
#                     Needs TARGET, LIBVMS_A, LIBVMSRMS_A, OUT.
#   seam_check <exe>  fails (exit 1) if any of FORCE_SEAMS is not DEFINED in <exe>.

seam_flags() {
    _weak=$("$TARGET-nm" "$LIBVMS_A" 2>/dev/null | awk '$1=="w"{print $2}' | sort -u)
    "$TARGET-nm" --defined-only "$LIBVMSRMS_A" 2>/dev/null | awk 'NF==3{print $3}' \
        | sort -u > "$OUT/.libvmsrms-defs"
    FORCE_SEAMS=$(printf '%s\n' "$_weak" | grep -Fx -f "$OUT/.libvmsrms-defs" || true)
    SEAM_FLAGS=""
    for _s in $FORCE_SEAMS; do SEAM_FLAGS="$SEAM_FLAGS -Wl,--undefined=$_s"; done
    echo "weak LIBVMS seams LIBVMSRMS defines (forced in): $(echo $FORCE_SEAMS)"
    if [ -z "$FORCE_SEAMS" ]; then
        echo "FAIL: found no weak LIBVMS seams -- the nm scan itself is broken"
        exit 1
    fi
}

seam_check() {
    _exe="$1"; _left=""
    echo "=== seam proof: every LIBVMSRMS seam is DEFINED in $(basename "$_exe") (none weak-NULL) ==="
    for _s in $FORCE_SEAMS; do
        "$TARGET-nm" "$_exe" | awk -v s="$_s" \
            '$NF==s && ($(NF-1)=="T"||$(NF-1)=="D"||$(NF-1)=="B"||$(NF-1)=="R"){f=1} END{exit !f}' \
            || _left="$_left $_s"
    done
    if [ -n "$_left" ]; then
        echo "FAIL: $(basename "$_exe") leaves these LIBVMS seams weak-NULL:$_left"
        exit 1
    fi
    echo "OK: all $(echo $FORCE_SEAMS | wc -w) seams are defined"
}

#   seam_exempt <exe> <item>   for an image NOT yet given its seams: computes
#                     the same list and PRINTS every one left weak in <exe>,
#                     tagged with the item that owns closing it. Never fails;
#                     the gap stays visible in every build log.
seam_exempt() {
    _exe="$1"; _item="$2"; _left=""
    seam_flags >/dev/null
    for _s in $FORCE_SEAMS; do
        "$TARGET-nm" "$_exe" | awk -v s="$_s" \
            '$NF==s && ($(NF-1)=="T"||$(NF-1)=="D"||$(NF-1)=="B"||$(NF-1)=="R"){f=1} END{exit !f}' \
            || _left="$_left $_s"
    done
    echo "=== seam proof EXEMPT ($_item): $(basename "$_exe") leaves these LIBVMSRMS seams weak-NULL:${_left:- (none)} ==="
}
