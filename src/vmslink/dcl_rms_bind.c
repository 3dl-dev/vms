/*
 * dcl_rms_bind.c - force LIBVMSRMS$SHR into DCL.EXE's loaded producer closure so
 * F$IDENTIFIER resolves against the RIGHTSLIST engine (vms-5f0, epic vms-208).
 *
 * WHY THIS OBJECT EXISTS.
 *
 * DCL's F$IDENTIFIER lexical (src/vmsdcl/dcl_lexical.c) resolves an identifier
 * name <-> value by calling rightslist_name_to_value / rightslist_value_to_name
 * in LIBVMS$SHR. Since the atomic flip (vms-f15a) those functions no longer scan
 * an ASCII colon file -- they `#pragma weak`-reference ovmx_rightslist_asctoid /
 * ovmx_rightslist_idtoasc (LIBVMSRMS$SHR universals, from rightslist_live.o) and,
 * when the weak cell is NULL, return the "no such identifier" miss. Those weak
 * references live in LIBVMS$SHR; IMGACT's resolve_weak_imports() binds them BY
 * NAME at activation, but only against the set of producers it has LOADED.
 *
 * THE DEFECT THIS CLOSES. IMGACT loads a producer only when the activated image
 * names it in a STRONG .vms$imp entry (bind_imports walks .vms$imp). DCL.EXE
 * --use's LIBVMSRMS$SHR at link, but -- unlike LOGINOUT -- makes NO strong
 * reference to ANY LIBVMSRMS$SHR universal: its ONLY path to RMS is through
 * LIBVMS$SHR's weak seam (no sys$open/$get call anywhere in src/vmsdcl). So
 * LINK.EXE records no strong .vms$imp import naming LIBVMSRMS$SHR, IMGACT never
 * loads it into DCL's producer closure, resolve_weak_imports() cannot find
 * ovmx_rightslist_asctoid anywhere, LIBVMS$SHR's weak cell stays 0, and every
 * F$IDENTIFIER goes dead ("no such identifier"). DCL activates as its OWN process
 * (execl'd by LOGINOUT), so LOGINOUT having loaded LIBVMSRMS$SHR in a different
 * activation does not help: DCL must anchor the producer itself. This is the same
 * root cause the LOGINOUT anchor (loginout_rms_bind.c) and the static-link test
 * anchor (tests/qemu/rms_acp_bind.c) close for their surfaces.
 *
 * THE FIX. This translation unit makes a STRONG reference to the RIGHTSLIST
 * engine entry points F$IDENTIFIER ultimately depends on. Compiled into DCL.EXE,
 * LINK.EXE's cross-image-import scan resolves each against the --use'd
 * LIBVMSRMS$SHR and records a strong .vms$imp import naming that producer. IMGACT
 * then loads LIBVMSRMS$SHR into DCL's producer closure, and resolve_weak_imports()
 * binds LIBVMS$SHR's whole weak RMS seam by name (ovmx_rightslist_* AND, once the
 * producer is loaded, ovmx_sysuaf_* for any SHOW that reads SYSUAF). It changes NO
 * behaviour of its own: rms_bind_never() is guarded by a volatile-false flag the
 * compiler cannot fold, so the calls are EMITTED (keeping the strong references
 * LINK.EXE needs) but NEVER executed at run time. Exports nothing; it only anchors
 * the producer dependency, exactly like loginout_rms_bind.c and DCL's other
 * executable-local TUs.
 */

/* The engine entry points DCL reaches through LIBVMS$SHR's weak seam, declared
 * only to take their address -- the reference is by NAME, so the exact prototype
 * is immaterial (no vmsrms header needed; this is a link-anchor TU, not part of
 * any library's include graph).
 *
 * F$IDENTIFIER (dcl_lexical.c) resolves BOTH a general identifier -- via
 * ovmx_rightslist_asctoid/_idtoasc (rightslist_live.o) -- AND a UIC identifier:
 * rightslist_name_to_value() falls through to sysuaf_lookup(), which weak-calls
 * ovmx_sysuaf_read_user/_uic (sysuaf_live.o), for names like SYSTEM/DEFAULT whose
 * value is that account's UIC. So BOTH engine leaf objects must be anchored.
 *
 * This matters differently on the two DCL link paths:
 *   - LINK.EXE shareable DCL.EXE (mk_dcl.sh): ANY one strong LIBVMSRMS$SHR import
 *     loads the producer, after which IMGACT's resolve_weak_imports() binds the
 *     WHOLE weak seam by name -- rightslist alone would suffice, sysuaf is free.
 *   - static-musl cmake DCL.EXE (target vmsdcl, the KE test harness's /bin/DCL.EXE):
 *     archive member-pull is per-object -- a strong ref to rightslist_live.o does
 *     NOT pull sysuaf_live.o. Anchoring BOTH is REQUIRED there, or the UIC-side
 *     F$IDENTIFIER answers go dead (test_syssvc_ident, vms-586). */
extern unsigned int ovmx_rightslist_asctoid(const char *name, unsigned int *value);
extern unsigned int ovmx_rightslist_idtoasc(unsigned int value, char *name,
                                            unsigned long bufsz);
extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

__attribute__((used, noinline))
unsigned int dcl_rms_bind_never(void)
{
    /* volatile: the compiler cannot prove this is always zero, so it keeps the
     * calls below (and their strong PLT relocations). At run time it IS zero, so
     * the function returns immediately and no RMS engine is ever entered. */
    static volatile int never = 0;
    if (!never)
        return 0;
    return ovmx_rightslist_asctoid(0, 0) | ovmx_rightslist_idtoasc(0, 0, 0)
         | ovmx_sysuaf_read_user(0, 0)   | ovmx_sysuaf_read_uic(0, 0);
}
