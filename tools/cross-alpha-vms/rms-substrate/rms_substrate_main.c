/* rms_substrate_main.c (vms-a7a, rung 1) — a tiny alpha-dec-vms LP64 image that
 * REFERENCES the RMS substrate universals sys$create / sys$open / sys$connect /
 * sys$put / sys$get / sys$close exported by the alpha LIBVMSRMS$SHR, so the
 * STRICT link (--use LIBVMSRMS$SHR --use DECC$SHR --use LIBOTS_SHR, no
 * --allow-undefined) proves every one of them binds as a real cross-image
 * import with ZERO deferred/undefined. It is compiled by the SAME real
 * alpha-dec-vms cross cc1 as the joint-e2e proofs, linked with the SAME real
 * port crt0 (../joint-e2e/crt0.s).
 *
 * This host has no /dev/vms and no qemu-system-alpha, so the image is NOT run
 * here — the LINK is the rung-1 proof (build + strict-link + EM_ALPHA/ET_DYN).
 * The un-fakeable RUNTIME proof (the create/put/close actually reaching the
 * executive ACP, an independent reader seeing the ODS-2 File ID) is rung 4
 * (vms-f49), on the real executive.
 *
 * Header-free extern style (like crtl_rms_test.c): no libc/RMS headers are set
 * up for this bare cross-compile; the six RMS system-service entries are
 * declared exactly as src/vmsrms/include/rms/rms.h declares them
 *   uint32_t sys$create(void *fab, void (*err)(void*), void (*suc)(void*));
 * ($ in identifiers is a VMS C convention the alpha-dec-vms cc1 accepts).
 * The FAB/RAB are opaque here (void*): the point is that each CALL references
 * the universal so the linker must resolve it; a runtime-meaningful FAB is
 * built by the rung-4 executive test.
 */
typedef unsigned int  u32;
typedef unsigned long u64;

extern u32 sys$create (void *fab, void (*err)(void *), void (*suc)(void *));
extern u32 sys$open   (void *fab, void (*err)(void *), void (*suc)(void *));
extern u32 sys$close  (void *fab, void (*err)(void *), void (*suc)(void *));
extern u32 sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern u32 sys$put    (void *rab, void (*err)(void *), void (*suc)(void *));
extern u32 sys$get    (void *rab, void (*err)(void *), void (*suc)(void *));

/* A small opaque control-block backing store. Real RMS reads a FAB/RAB out of
 * this; for the link proof we only need the addresses to flow into the calls. */
static u64 fab_store[64];
static u64 rab_store[64];

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    void *fab = (void *)fab_store;
    void *rab = (void *)rab_store;

    /* Reference all six universals. The return values are folded into a status
     * so nothing is dead-code-eliminated. */
    u32 st = 0;
    st |= sys$create (fab, 0, 0);
    st |= sys$open   (fab, 0, 0);
    st |= sys$connect(rab, 0, 0);
    st |= sys$put    (rab, 0, 0);
    st |= sys$get    (rab, 0, 0);
    st |= sys$close  (fab, 0, 0);
    return (int)st;
}
