/*
 * fal_rms_bind.c - force the RMS record services into FAL.EXE's static link
 * closure so the FAL network server process (rd vms-d85) moves the transferred
 * file through RMS over the Files-11 ODS-2 ACP -- checked against its OWN
 * (the authenticated user's) identity. Same pattern and reason as
 * src/vmsdecnet/engine/decnetd_rms_bind.c (rd vms-8c2), which this mirrors.
 *
 * WHY THIS OBJECT EXISTS.
 *
 * FAL.EXE is a static musl image (distro/Dockerfile.bootable build-static,
 * OVMX_STATIC=ON). Its FAL half authenticates through sysuaf_lookup +
 * sysuaf_authenticate (LIBVMS) and does real file I/O through rms_textfile_*
 * (LIBVMS) -- but BOTH of those reach the executive-backed producers through a
 * `#pragma weak` seam:
 *   - rms_textfile.c weak-references sys$open/$close/$connect/$disconnect/$get/
 *     $put/$create (defined in vmsrms, rms_core.c).
 *   - sysuaf.c weak-references ovmx_sysuaf_read_user/_uic (defined in vmsrms,
 *     sysuaf_live.o).
 * LIBVMS sits BELOW RMS in the layering, so it cannot hard-reference those
 * producers without inverting the layering -- hence the weak seam.
 *
 * THE DEFECT THIS CLOSES. Under a static link a WEAK undefined reference does
 * NOT extract the defining archive member. FAL linked neither vmsrms nor any
 * STRONG reference into it, so sys$open and ovmx_sysuaf_read_user stayed NULL,
 * rms_services_present() read FALSE, sysuaf_lookup() returned "miss" before any
 * ACP call, and --fal-accept-test's GUEST/GUEST auth + source-file creation both
 * FAILED honestly (INV-6) on the booted image even though LOGINOUT -- which
 * carries the analogous anchor (src/vmslink/loginout_rms_bind.c) -- authenticates
 * GUEST fine. This is the SAME weak-seam trap provision_rms_bind.c and
 * tests/qemu/rms_acp_bind.c close for their images.
 *
 * THE FIX. A table of STRONG references to the RMS record services rms_textfile.c
 * needs AND the two binary-SYSUAF engine readers sysuaf.c needs. Compiled into
 * FAL.EXE, it forces the static linker to extract rms_core.o + sysuaf_live.o
 * from the vmsrms archive FAL now links, so rms_services_present() is TRUE and
 * the genuine ACP $CREATE/$PUT + the genuine $UAFDEF Purdy read of SYSUAF run. It
 * changes NO behaviour of its own (never called) -- `used` keeps the compiler from
 * discarding the table and the address-taken `volatile const` array keeps the
 * linker from folding the references away, so each stays a genuine strong
 * undefined reference that pulls its producer in.
 */

/* The seven RMS three-argument services rms_textfile.c reaches through the weak
 * seam, declared with their real (void *, callback, callback) shape so the
 * reference is to the exact symbols the seam names. No header needed; this is a
 * link-anchor TU, not part of any library's include graph. */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$put(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$create(void *fab, void (*err)(void *), void (*suc)(void *));

/* The two engine entry points sysuaf.c weak-references. Declared only to take
 * their address -- the reference is by NAME, so the exact prototype is
 * immaterial (no vmsrms header needed here). Pulling these extracts
 * sysuaf_live.o so the ovmx_sysuaf_* weak cells in LIBVMS bind. */
extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

/* A table of the addresses. `used` keeps the compiler from discarding it and
 * `volatile` keeps the linker from folding the references away, so each symbol
 * stays a genuine strong undefined reference that pulls its RMS producer in. */
void *const volatile fal_rms_bind_anchor[] __attribute__((used)) = {
    (void *)&sys$open,
    (void *)&sys$close,
    (void *)&sys$connect,
    (void *)&sys$disconnect,
    (void *)&sys$get,
    (void *)&sys$put,
    (void *)&sys$create,
    /* the flip's engine seam -- pull the leaf reader object too */
    (void *)&ovmx_sysuaf_read_user,
    (void *)&ovmx_sysuaf_read_uic,
};
