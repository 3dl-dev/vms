/*
 * loginout_rms_bind.c - force LIBVMSRMS$SHR into LOGINOUT.EXE's loaded producer
 * closure so LOGINOUT can authenticate against SYSUAF (vms-5f0, epic vms-208).
 *
 * WHY THIS OBJECT EXISTS.
 *
 * LOGINOUT.EXE authenticates by reading SYS$SYSTEM:SYSUAF.DAT through RMS
 * ($OPEN/$CONNECT/$GET) over the Files-11 ODS-2 ACP. Those RMS entry points are
 * exported by LIBVMSRMS$SHR, but they are CALLED from rms_textfile.c inside
 * LIBVMS$SHR, which `#pragma weak`-references them: LIBVMS$SHR sits BELOW RMS in
 * the layering (LIBVMSRMS$SHR --use's LIBVMS$SHR, never the reverse), so
 * LIBVMS$SHR cannot --use LIBVMSRMS$SHR to import sys$open by (producer,index)
 * without a build cycle. LINK.EXE therefore records those weak references in
 * LIBVMS$SHR's .vms$wimp, and IMGACT's resolve_weak_imports() binds them by NAME
 * at activation -- against the WHOLE loaded producer set.
 *
 * THE DEFECT THIS CLOSES. IMGACT loads a producer only when the activated image
 * names it in a STRONG .vms$imp entry (bind_imports walks .vms$imp and loads
 * each producer_off it finds). LOGINOUT.EXE --use's LIBVMSRMS$SHR at link, but
 * makes NO strong reference to any LIBVMSRMS$SHR universal of its own -- all its
 * RMS use is through LIBVMS$SHR's weak seam. So LINK.EXE records no strong
 * .vms$imp import naming LIBVMSRMS$SHR, IMGACT never loads it, and
 * resolve_weak_imports() -- iterating only the loaded producers (DECC$SHR,
 * LIBVMS$SHR, LIBVMSPROCESS$SHR, LIBVMSFS$SHR, LIBVMSLNM$SHR, LIBVMSSYS$SHR) --
 * cannot find sys$open anywhere. LIBVMS$SHR's weak sys$open cell stays 0,
 * rms_services_present() reads FALSE, rms_textfile_open() bails to NULL BEFORE
 * any ACP call, SYSUAF is never read, and every login fails "User authorization
 * failure" -- the ACP never even reached. This is the exact --as-needed / no
 * DT_NEEDED behaviour the test-side fix (tests/qemu/rms_acp_bind.c) closes for
 * the statically-linked syssvc suites, hitting the LINK.EXE shareable-image path
 * through the same root cause.
 *
 * THE FIX. This translation unit makes a STRONG reference to the RMS entry
 * points LOGINOUT ultimately depends on. Compiled into LOGINOUT.EXE, LINK.EXE's
 * cross-image-import scan resolves each against the --use'd LIBVMSRMS$SHR and
 * records a strong .vms$imp import naming that producer. IMGACT then loads
 * LIBVMSRMS$SHR into the producer closure, and resolve_weak_imports() finds
 * sys$open there and binds LIBVMS$SHR's weak cell -- rms_services_present() is
 * TRUE and the genuine ACP read of SYSUAF runs. It changes NO behaviour of its
 * own: rms_bind_never() is guarded by a volatile-false flag the compiler cannot
 * fold, so the calls are EMITTED (keeping the strong references LINK.EXE needs)
 * but NEVER executed at run time.
 *
 * The `volatile` read is what forces both properties: the compiler may not prove
 * the block dead (so the calls, and their relocations, survive), yet the guard
 * is always false at run time (so the function returns 0 without ever entering
 * RMS). A plain address-taken table would work under a static linker but risks a
 * data-relocation form LINK.EXE's strong-import scan is not tuned for; a guarded
 * CALL is the well-trodden PLT/import path every other cross-image call in
 * vms_login.o already takes.
 */

/* The RMS three-argument services rms_textfile.c reaches through the weak seam,
 * declared with their real (void *, callback, callback) shape so the reference
 * is to the exact universals LIBVMSRMS$SHR exports. No header needed; this is a
 * link-anchor TU, not part of any library's include graph. */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$put(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$create(void *fab, void (*err)(void *), void (*suc)(void *));

/* THE ENGINE SEAM (vms-5f0 flip). The atomic flip moved SYSUAF authentication
 * itself onto the binary Prolog-3 engine: sysuaf_lookup() in LIBVMS$SHR no longer
 * scans an ASCII file, it `#pragma weak`-references ovmx_sysuaf_read_user/_uic
 * (LIBVMSRMS$SHR universals, from sysuaf_live.o) and returns "miss" when the cell
 * is NULL. sys$open above already forces LIBVMSRMS$SHR into LOGINOUT's strong
 * .vms$imp closure (so IMGACT loads it and resolve_weak_imports binds the whole
 * weak seam by name), but the engine entries are the universals LOGINOUT's
 * authentication path ACTUALLY depends on -- anchor them directly so the strong
 * import naming LIBVMSRMS$SHR does not hinge on rms_textfile.c's own sys$ use. */
extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

__attribute__((used, noinline))
unsigned int rms_bind_never(void)
{
    /* volatile: the compiler cannot prove this is always zero, so it keeps the
     * calls below (and their strong PLT relocations). At run time it IS zero, so
     * the function returns immediately and no RMS service is ever entered. */
    static volatile int never = 0;
    if (!never)
        return 0;
    return sys$open(0, 0, 0)    | sys$close(0, 0, 0)  | sys$connect(0, 0, 0)
         | sys$get(0, 0, 0)     | sys$put(0, 0, 0)    | sys$create(0, 0, 0)
         | ovmx_sysuaf_read_user(0, 0)                | ovmx_sysuaf_read_uic(0, 0);
}
