/*
 * tcpip_inetd_rms_bind.c - force-bind anchor for TCPIP$INETD.EXE (rd vms-8bd).
 *
 * THE WEAK-SEAM TRAP (the SAME one dcl_rms_bind.c / loginout_rms_bind.c /
 * decnetd_rms_bind.c / provision_rms_bind.c close): src/libvms/rtl/sysuaf.c
 * `#pragma weak`-references ovmx_sysuaf_read_user()/_uic() -- the REAL
 * SYSUAF-over-ODS-2-ACP readers, defined in src/vmsrms/sysuaf_live.c. A weak
 * UNDEFINED reference does NOT cause the static linker to extract an archive
 * member, so linking LIBVMSRMS is not enough: with nothing making a STRONG
 * reference to those symbols, sysuaf_live.o is never pulled and the weak cells
 * stay NULL. sysuaf_lookup() then returns -1 with RMS status 0 (the "no
 * LIBVMSRMS in this image" branch), which is EXACTLY how a live TCPIP$INETD
 * failed to resolve its per-service run-as account at boot: the account was on
 * disk, the file opened, but the leaf reader was never linked in.
 *
 * Before R4 G1 the auxiliary server never resolved SYSUAF, so it never needed
 * these readers; the per-service persona drop is the first caller, and it must
 * carry its own anchor -- mirroring every other image that reads SYSUAF over the
 * ACP.
 *
 * Taking the ADDRESS of each reader is a strong undefined reference that extracts
 * sysuaf_live.o (which in turn strong-references the RMS core it calls, pulling
 * rms_core.o), so the weak cells in LIBVMS bind to the real readers. The
 * reference is by NAME only -- the exact prototype is immaterial -- so no vmsrms
 * header is needed here. `used` + `volatile` keep the compiler and linker from
 * discarding or folding the table away, so each stays a genuine strong reference.
 */

extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

void *const volatile tcpip_inetd_rms_bind_anchor[] __attribute__((used)) = {
    (void *)&ovmx_sysuaf_read_user,
    (void *)&ovmx_sysuaf_read_uic,
};
