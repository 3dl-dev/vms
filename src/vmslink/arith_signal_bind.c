/*
 * arith_signal_bind.c - static-link anchor for the Alpha arithmetic-trap ->
 * SS$_HPARITH condition bridge (rd vms-db3, epic vms-8954).
 *
 * WHY. The bridge lives in src/libvms/rtl/arith_signal.c and installs its
 * image-start SIGFPE handler via a glibc .init_array constructor (OVMX's
 * LIB$INITIALIZE equivalent). Under -static (OVMX_STATIC / musl or static-glibc
 * images) archive member-pull is PER-OBJECT: nothing in a normal image
 * references arith_signal.o, so the linker never extracts it and the
 * constructor never runs -- the arith bridge would silently not install (the
 * exact false-negative -static weak-seam class documented in
 * src/vmslink/dcl_rms_bind.c and tests/qemu/rms_acp_bind.c).
 *
 * This TU makes a STRONG reference to ovmx$arith_signal_anchor, forcing the
 * linker to extract arith_signal.o (and thus its constructor) into any image
 * that compiles this file as a PRIMARY object (via target_sources()). Same
 * established pattern as dcl_rms_bind.c -- a `used`, volatile-false-guarded
 * reference that is emitted but never executed at run time.
 */

extern int ovmx$arith_signal_anchor;

__attribute__((used, noinline))
int ovmx_arith_signal_bind_never(void)
{
    /* volatile: the compiler cannot prove this is always zero, so it keeps the
     * reference below (and its strong relocation). At run time it IS zero, so
     * the function returns immediately -- the anchor is a link-time device only. */
    static volatile int never = 0;
    if (!never)
        return 0;
    return ovmx$arith_signal_anchor;
}
