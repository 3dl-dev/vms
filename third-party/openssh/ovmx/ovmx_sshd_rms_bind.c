/* SPDX-License-Identifier: BSD-2-Clause */
/*
 * ovmx_sshd_rms_bind.c - force-bind the RMS-over-ACP service seam into the
 * wrapped OpenSSH sshd (rd vms-9cc; the sshd counterpart of
 * tests/qemu/rms_acp_bind.c).
 *
 * WHY THIS OBJECT EXISTS. The wrapped sshd authenticates against the binary
 * SYSUAF: __wrap_getpwnam -> ovmx_sshd_fill_passwd and sys_auth_passwd ->
 * ovmx_sshd_sysuaf_auth (src/vmsssh/sshd_auth.c) both call sysuaf_lookup()
 * (src/libvms/rtl/sysuaf.c). Since the vms-5f0 flip, that reader reaches SYSUAF
 * the VMS way -- RMS $OPEN/$GET over the Files-11 ODS-2 ACP -- through a
 * `#pragma weak` seam: it weak-references the RMS record services (sys$open/...)
 * and the engine entry points (ovmx_sysuaf_read_user/_uic), and returns "miss"
 * unconditionally when those cells are NULL (`if (!ovmx_sysuaf_read_user) return
 * -1;`). LIBVMS sits BELOW RMS, so the seam is weak by design (a hard reference
 * would invert the layering and force every LIBVMS consumer to link RMS).
 *
 * THE DEFECT THIS CLOSES. A WEAK undefined reference does not make the linker
 * pull the defining member (sysuaf_live.o / the RMS record services) out of the
 * OVMX static archives the sshd already lists on its link line. So the wrapped
 * sshd got sys$open == NULL and ovmx_sysuaf_read_user == NULL, and EVERY
 * sysuaf_lookup bailed to "not found" BEFORE any ACP call -- surfacing as
 * "Invalid user SYSTEM" for a real account, and (vacuously) as a correct refusal
 * for an unknown one. The SYSUAF was never actually read. Proven on the rail:
 * with SYS$SYSDEVICE correctly pointed at the mounted system volume, an
 * in-process sysuaf_lookup_st("SYSTEM") still returned rc=-1 with rms_st=0 (no
 * RMS status = the read never happened).
 *
 * THE FIX. This translation unit makes STRONG references to the seam symbols.
 * Force-linked as a primary object (before the --start-group archive cycle) into
 * the wrapped sshd, it forces the linker to resolve them from the OVMX archives
 * the harness already links -- extracting sysuaf_live.o + the RMS record
 * services -- so the libvms weak cells bind and the genuine ACP read runs. It
 * changes NO behaviour of its own (never called); it only anchors the symbols.
 *
 * This is the static-link counterpart of the production force-binds
 * (mk_loginout.sh via loginout_rms_bind.c, mk_dcl.sh via dcl_rms_bind.c) and the
 * test-side tests/qemu/rms_acp_bind.c.
 */

/* The RMS three-argument record services the SYSUAF reader (rms_textfile.c /
 * sysuaf_live.c) references, in their real (void *, callback, callback) shape --
 * the exact symbols the weak seam names, no header needed. */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));

/* The engine entry points the flip's libvms SYSUAF reader weak-references.
 * Pulling these extracts sysuaf_live.o so the ovmx_* weak cells in libvms bind. */
extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

/* A table of the addresses. `used` keeps the compiler from discarding it and
 * `volatile` keeps the linker from folding the references away, so each symbol
 * stays a genuine strong undefined reference that pulls its RMS producer in. */
void *const volatile ovmx_sshd_rms_bind_anchor[] __attribute__((used)) = {
    (void *)&sys$open,
    (void *)&sys$close,
    (void *)&sys$connect,
    (void *)&sys$disconnect,
    (void *)&sys$get,
    (void *)&ovmx_sysuaf_read_user,
    (void *)&ovmx_sysuaf_read_uic,
};
