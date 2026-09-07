/* test_veneer_main.c (vms-ed1e, rung 2 of vms-b4f) — a tiny alpha-dec-vms LP64
 * image that calls the PLAIN C stdio entry points (fopen/fwrite/fread/fclose),
 * so the alpha-dec-vms cc1's OWN crtlmap auto-decoration turns each call site
 * into a reference to decc$fopen/decc$fwrite/decc$fread/decc$fclose (gcc/
 * config/vms/vms.cc; the same auto-decoration mk_decc_shr.sh's header comment
 * documents for the OVMX bootstrap surface, vms-864).
 *
 * The STRICT link (--use DECC$SHR --use LIBVMSRMS$SHR --use LIBOTS_SHR, no
 * --allow-undefined) proves these four bind as real cross-image imports to
 * the veneer-wired DECC$SHR (rung 2), with ZERO deferred. This mirrors rung
 * 1's rms_substrate_main.c pattern exactly (header-free extern style; $ in
 * identifiers is a VMS C convention the alpha-dec-vms cc1 accepts) but calls
 * through the DEC C RTL stdio surface instead of the raw sys$ RMS services —
 * proving the CONSUMER-FACING veneer path (a real port program's fopen call),
 * not just the RMS substrate underneath it.
 *
 * This host has no /dev/vms and no qemu-system-alpha, so the image is NOT run
 * here — the LINK is the rung-2 proof (build + strict-link + EM_ALPHA/ET_DYN,
 * zero deferred). The un-fakeable RUNTIME proof (fopen/fwrite/fread/fclose
 * actually reaching the executive ACP through the veneer, from a REAL alpha
 * port program) is a later rung, on the real executive.
 */
typedef unsigned long size_t_;

extern void *fopen (const char *path, const char *mode);
extern size_t_ fwrite(const void *ptr, size_t_ size, size_t_ nmemb, void *fh);
extern size_t_ fread (void *ptr, size_t_ size, size_t_ nmemb, void *fh);
extern int   fclose(void *fh);

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    static char buf[16] = "OVMX";
    void *fh = fopen("SYS$SCRATCH:VENEER_TEST.DAT", "w");
    size_t_ st = 0;
    if (fh) {
        st += fwrite(buf, 1, 4, fh);
        st += (size_t_)fclose(fh);
    }
    fh = fopen("SYS$SCRATCH:VENEER_TEST.DAT", "r");
    if (fh) {
        st += fread(buf, 1, 4, fh);
        st += (size_t_)fclose(fh);
    }
    return (int)st;
}
