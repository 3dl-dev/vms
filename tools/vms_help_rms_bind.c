/*
 * vms_help_rms_bind.c - force the RMS record services into HELP.EXE's static
 * link closure so it can read SYS$HELP:HELPLIB.HLP over the Files-11 ODS-2 ACP
 * (vms-4ac).
 *
 * WHY THIS OBJECT EXISTS. dcl_help.c (shared with the DCL HELP built-in) now
 * reads the HELP library over the executive ACP via rms_textfile_* -- the /vms
 * POSIX passthrough it used to fopen() was retired by the atomic flip (epic
 * vms-208). rms_textfile.c (in LIBVMS) reaches the RMS services through a
 * `#pragma weak` seam (LIBVMS sits below RMS in the layering, so a hard
 * reference would invert it). HELP.EXE is STATIC (build-static/musl), and a
 * WEAK undefined reference does not extract the defining member from the vmsrms
 * archive, so rms_textfile.c's sys$* cells would stay NULL, rms_services_present()
 * would read FALSE, and rms_textfile_open() would return NULL before any ACP
 * call -- exactly the OPENIN this item fixes. This is the SAME weak-seam trap
 * src/ovmx_provision/provision_rms_bind.c and src/vmslink/loginout_rms_bind.c
 * close for their images.
 *
 * THE FIX. A table of STRONG references to the RMS record services rms_textfile.c
 * needs, compiled into HELP.EXE so the static linker extracts those members from
 * the vmsrms archive HELP.EXE now links. It changes NO behaviour of its own
 * (never called): `used` keeps the compiler from discarding the table and
 * `volatile` is unnecessary because the array of function-address initialisers
 * is itself a set of genuine strong undefined references the linker must satisfy.
 */

/* The RMS services rms_textfile.c reaches through the weak seam, declared with
 * their real (void *, callback, callback) shape so the reference names the exact
 * symbols the seam does. No header needed; this is a link-anchor TU. */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$put(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$create(void *fab, void (*err)(void *), void (*suc)(void *));

void *const vms_help_rms_bind_anchor[] __attribute__((used)) = {
    (void *)&sys$open,
    (void *)&sys$close,
    (void *)&sys$connect,
    (void *)&sys$disconnect,
    (void *)&sys$get,
    (void *)&sys$put,
    (void *)&sys$create,
};
