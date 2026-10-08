/*
 * mail_rms_bind.c - force the RMS record services and the binary-SYSUAF reader
 * into the static link closure of the two MAIL store writers, MAIL.EXE and
 * MAIL_SERVER.EXE (rd vms-47fd). Same trap, same fix as
 * src/vmsdecnet/fal/fal_rms_bind.c: rms_textfile.c (the store's reader) and
 * sysuaf.c (recipient/home-directory lookup) reach their producers through a
 * `#pragma weak` seam, and a weak undefined reference never extracts the
 * defining archive member -- so without a STRONG reference the mail file reads
 * as absent and every user as unknown (INV-6 holds, but nothing works). Never
 * called; `used` + the address-taken volatile table keep every reference.
 */
extern unsigned int sys$open(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$close(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$connect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$disconnect(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$get(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$put(void *rab, void (*err)(void *), void (*suc)(void *));
extern unsigned int sys$create(void *fab, void (*err)(void *), void (*suc)(void *));
extern unsigned int ovmx_sysuaf_read_user(const char *username, void *out);
extern unsigned int ovmx_sysuaf_read_uic(unsigned int uic, void *out);

void *const volatile mail_rms_bind_anchor[] __attribute__((used)) = {
    (void *)&sys$open,
    (void *)&sys$close,
    (void *)&sys$connect,
    (void *)&sys$disconnect,
    (void *)&sys$get,
    (void *)&sys$put,
    (void *)&sys$create,
    (void *)&ovmx_sysuaf_read_user,
    (void *)&ovmx_sysuaf_read_uic,
};
