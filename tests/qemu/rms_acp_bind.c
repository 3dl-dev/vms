/*
 * rms_acp_bind.c - force-bind the RMS-over-ACP service seam into every QEMU
 * syssvc test binary (vms-058, epic vms-208).
 *
 * WHY THIS OBJECT EXISTS.
 *
 * The SYSUAF / RIGHTSLIST / $GETUAI readers in LIBVMS reach their files the VMS
 * way -- RMS $OPEN/$GET over the Files-11 ODS-2 ACP -- through
 * src/libvms/rtl/rms_textfile.c. That file references sys$open/$get/... with
 * `#pragma weak` on purpose: LIBVMS sits BELOW RMS (LIBVMSRMS links LIBVMS, not
 * the reverse), so a HARD reference from LIBVMS into RMS would invert the
 * layering and force every LIBVMS consumer to also link RMS. The weak seam lets
 * a bare-LIBVMS image build and fail-honest (rms_services_present() == 0 ->
 * rms_textfile_open() returns NULL) when RMS is genuinely absent (Rule 9/INV-6).
 *
 * THE DEFECT THIS CLOSES. A WEAK UNDEFINED reference does NOT make the linker
 * pull the defining member out of the vmsrms archive (static link) and does NOT
 * make it record LIBVMSRMS$SHR as DT_NEEDED (shared link, --as-needed). So an
 * image that lists `vmsrms` on its link line but only reaches RMS through the
 * weak seam got sys$open == NULL anyway: rms_services_present() returned FALSE
 * and every SYSUAF/RIGHTSLIST read bailed to NULL BEFORE any ACP call --
 * surfacing as "record not found" (a login/identifier miss), NOT as an honest
 * read error. Proven directly: `nm test_syssvc_sysuaf_uic_base` carried no
 * sys$open at all, while test_syssvc_loginout_acp -- which STRONGLY calls
 * sys$create -- did, and read SYSUAF fine. The ODS-2 codec, the fixture and the
 * octal-UIC parse were all correct; the reader simply never opened the file.
 *
 * THE FIX. This translation unit makes a STRONG reference to the RMS entry
 * points rms_textfile.c needs. Linked as a primary object (before the libraries)
 * into every syssvc suite, it forces the linker to resolve those symbols from
 * the `vmsrms` the recipe already links -- extracting the archive member
 * (static) / keeping LIBVMSRMS$SHR needed (shared) -- so rms_services_present()
 * is TRUE and the genuine ACP read runs. It changes NO behaviour of its own
 * (never called); it only anchors the symbols. It is applied OPT-IN (vms-5f0),
 * via target_sources() in tests/qemu/CMakeLists.txt, to exactly the suites that
 * read an identity file IN-PROCESS through the libvms reader and carry no strong
 * sys$ RMS call of their own (sysuaf_uic_base, rightslist, setuai). A strong ref
 * here EXTRACTS the whole vmsrms archive + ODS-2 codec into the static binary,
 * so applying it to ALL syssvc suites bloated the shared kernel-test initramfs
 * 31M->36M and overflowed the QEMU boot -- hence opt-in, not the blanket recipe.
 *
 * NOTE: this binds the TEST surface. Production OVMX images that read SYSUAF via
 * the ACP after IMGACT activation (LOGINOUT.EXE, the spawned DCL that answers
 * F$IDENTIFIER, MMK.EXE) hit the SAME weak seam through a DIFFERENT mechanism --
 * symbol-vector import binding of a `--use`'d LIBVMSRMS$SHR at activation -- and
 * need the analogous force-bind in their link scripts (mk_loginout.sh, mk_dcl.sh,
 * ...). That is tracked follow-on work; see the vms-058 report.
 */

/* The seven RMS three-argument services rms_textfile.c references. Declared with
 * their real (void *, callback, callback) shape so the reference is to the exact
 * symbols the weak seam names -- no header needed, and no dependency inversion in
 * the include graph (this is a TEST-side object, not part of LIBVMS). */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$put(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$create(void *fab, void (*err)(void *), void (*suc)(void *));

/* A table of the addresses. `used` keeps the compiler from discarding it and
 * `volatile` keeps the linker from folding the references away, so each symbol
 * stays a genuine strong undefined reference that pulls its RMS producer in. */
void *const rms_acp_bind_anchor[] __attribute__((used)) = {
    (void *)&sys$open,
    (void *)&sys$close,
    (void *)&sys$connect,
    (void *)&sys$disconnect,
    (void *)&sys$get,
    (void *)&sys$put,
    (void *)&sys$create,
};
