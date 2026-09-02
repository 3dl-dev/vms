/*
 * provision_rms_bind.c - force the RMS record services into PROVISION.EXE's
 * static link closure so it can seed SYS$MANAGER:OPERATOR.LOG over the Files-11
 * ODS-2 ACP (vms-aac, epic vms-208 atomic flip).
 *
 * WHY THIS OBJECT EXISTS.
 *
 * PID 1 (STARTUP.EXE) runs the /dev/kmsg -> OPERATOR.LOG bridge but is static
 * and carries no RMS (force-binding RMS into the static PID-1 image bloats the
 * boot initramfs -- see src/ovmx_init/opcom_kmsg.c's top comment). So the boot-
 * time kmsg records are written into the genuine on-volume OPERATOR.LOG by
 * PROVISION.EXE, which links RMS and runs AFTER the SYS$DISK $MOUNT: main()
 * calls opcom_kmsg_seed_operator_log() with an emit that does
 * rms_textfile_append_line(SYS$MANAGER:OPERATOR.LOG, line).
 *
 * THE DEFECT THIS CLOSES. rms_textfile.c (in LIBVMS) reaches the RMS services
 * through a `#pragma weak` seam -- LIBVMS sits below RMS in the layering, so a
 * hard reference would invert it. PROVISION already links vmsrms, but only
 * STRONGLY references the binary $UAFDEF engine (ovmx_sysuaf_enum, sysuaf_live.o),
 * which pulls neither sys$open/$create/$put nor the seq-record engine. A WEAK
 * undefined reference does NOT extract the defining archive member (static link),
 * so rms_textfile.c's sys$* cells stayed NULL, rms_services_present() read FALSE,
 * and rms_textfile_append_line() returned -1 before any ACP call -- OPERATOR.LOG
 * was never created. This is the SAME weak-seam trap tests/qemu/rms_acp_bind.c
 * and src/vmslink/loginout_rms_bind.c close for their images.
 *
 * THE FIX. A table of STRONG references to the RMS record services
 * rms_textfile.c needs. Compiled into PROVISION.EXE, it forces the static linker
 * to extract those members from the vmsrms archive PROVISION already links, so
 * rms_services_present() is TRUE and the genuine ACP $CREATE/$PUT runs. It
 * changes NO behaviour of its own (never called) -- `used` keeps the compiler
 * from discarding the table and `volatile` keeps the linker from folding the
 * references away, so each stays a genuine strong undefined reference that pulls
 * its RMS producer in.
 */

/* The RMS services rms_textfile.c reaches through the weak seam, declared with
 * their real (void *, callback, callback) shape so the reference is to the exact
 * symbols the seam names. No header needed; this is a link-anchor TU. */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$put(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$create(void *fab, void (*err)(void *), void (*suc)(void *));

void *const provision_rms_bind_anchor[] __attribute__((used)) = {
    (void *)&sys$open,
    (void *)&sys$close,
    (void *)&sys$connect,
    (void *)&sys$disconnect,
    (void *)&sys$get,
    (void *)&sys$put,
    (void *)&sys$create,
};
