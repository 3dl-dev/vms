/*
 * accvio_signal_bind.c - static-link anchor for the Alpha access-violation ->
 * SS$_ACCVIO condition bridge (rd vms-cc8, CHF rung-4, epic vms-2e72).
 *
 * WHY. The bridge lives in src/libvms/rtl/accvio_signal.c and installs its
 * image-start SIGSEGV/SIGBUS/SIGILL handler via a .init_array constructor
 * (OVMX's LIB$INITIALIZE equivalent). Under -static archive member-pull is
 * PER-OBJECT: nothing in a normal image references accvio_signal.o, so the
 * linker never extracts it and the constructor never runs -- the bridge would
 * silently not install (the false-negative -static weak-seam class documented in
 * src/vmslink/arith_signal_bind.c / dcl_rms_bind.c).
 *
 * This TU makes a STRONG reference to ovmx$accvio_signal_anchor, forcing the
 * linker to extract accvio_signal.o (and thus its constructor) into any image
 * that compiles this file as a PRIMARY object (via target_sources()). Same
 * established pattern as arith_signal_bind.c -- a `used`, volatile-false-guarded
 * reference that is emitted but never executed at run time.
 */

extern int ovmx$accvio_signal_anchor;

__attribute__((used, noinline))
int ovmx_accvio_signal_bind_never(void)
{
    /* volatile: the compiler cannot prove this is always zero, so it keeps the
     * reference below (and its strong relocation). At run time it IS zero, so
     * the function returns immediately -- the anchor is a link-time device only. */
    static volatile int never = 0;
    if (!never)
        return 0;
    return ovmx$accvio_signal_anchor;
}
