/*
 * ncp_rms_bind.c - force the RMS record services into NCP.EXE's static link
 * closure so its DECnet databases persist at SYS$SYSTEM:NETNODE_LOCAL.DAT /
 * NETNODE_REMOTE.DAT / NETOBJECT.DAT through RMS over the Files-11 ODS-2 ACP
 * (rd vms-1f69, dnet_ncpstore.c).
 *
 * WHY THIS OBJECT EXISTS. dnet_ncpstore.c reads and writes through
 * rms_textfile_* (LIBVMS), which reaches sys$open/$close/$connect/$disconnect/
 * $get/$put/$create through a `#pragma weak` seam -- LIBVMS sits BELOW RMS in
 * the layering and must not hard-reference it. Under the static booted link
 * (OVMX_STATIC, distro/Dockerfile.bootable) a WEAK undefined reference does not
 * extract the defining archive member, so with nothing else naming them every
 * service stays NULL, rms_services_present() reads FALSE, and NCP SET EXECUTOR
 * would fail %NCP-E-CFGWRERR before any ACP call even on a healthy executive.
 * The same trap decnetd_rms_bind.c / loginout_rms_bind.c /
 * tests/qemu/rms_acp_bind.c close for their images.
 *
 * THE FIX. A table of STRONG references to the seven services. It is never
 * called; `used` keeps the compiler from discarding it and the address-taken
 * `volatile const` array keeps the linker from folding the references away, so
 * each stays a genuine strong undefined reference that pulls rms_core.o in.
 */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$put(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$create(void *fab, void (*err)(void *), void (*suc)(void *));

void *const volatile ncp_rms_bind_anchor[] __attribute__((used)) = {
    (void *)&sys$open,
    (void *)&sys$close,
    (void *)&sys$connect,
    (void *)&sys$disconnect,
    (void *)&sys$get,
    (void *)&sys$put,
    (void *)&sys$create,
};
