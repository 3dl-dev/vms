/* crtl_rms3_test.c — the alpha-dec-vms CRTL/RMS FILE-OP port program (vms-3320),
 * wired as a REPRODUCIBLE joint-e2e VARIANT (build-joint-image.sh JOINT_MAIN).
 *
 * It advances the vms-b4f / vms-da0 ladder past crtl_rms_test.c (stdio family)
 * and crtl_rms2_test.c (line-I/O + fmt + sort): this program drives the FILE-OP
 * decc$ surface a real GCC port leans on but which was still musl-POSIX before
 * vms-3320 — temp-file minting, cleanup, atomic finalization, dir enumeration:
 *
 *   - open / creat  : mint a real ODS-2 file (sys$create) and reopen it (sys$open)
 *   - unlink        : delete a file (sys$erase)  — temp cleanup
 *   - rename        : ATOMIC re-link (sys$rename -> ACP MODIFY!M_MOVE) — the
 *                     compiler's "write NAME.tmp, rename over the final" finalize
 *   - opendir/readdir/closedir : enumerate the REAL ODS-2 directory entries
 *
 * Every reference is a GENUINE decc$ call: the alpha-dec-vms cross cc1 decorates
 * each name to the decc$ surface at codegen, and mk_decc_shr.sh's ALPHA/EVAX
 * branch (ALPHA_CRTL_RMS_USE) VECTOR-SUBSTITUTES decc$open/creat/unlink/remove/
 * rename/opendir/readdir/closedir -> src/vmsrms/crtl_rms_stdio.c's ovmx_crtl_*
 * (which drive sys$create/$open/$erase/$rename/$parse/$search over the real
 * Files-11 ODS-2 executive ACP). Under JOINT_CRTL_RMS_VENEER=1 this is the path
 * that runs — NOT musl-POSIX. Fail-honest: any RMS failure returns a sentinel.
 *
 * This program is its OWN first (same-CRTL) check — it returns sentinel 7 only
 * if every op succeeded AND its own decc$opendir/readdir enumeration agrees the
 * created + renamed files are present and the deleted + old-name files are gone.
 * The UN-FAKEABLE proof is the INDEPENDENT reader the boot runs afterward
 * (SYS$MANAGER:SYSTARTUP_VMS -> DCL DIRECTORY over the ACP, a DIFFERENT accessor):
 * it must see FOPCRE.DAT + FOPDST.DAT with genuine ODS-2 File IDs and NOT see
 * FOPDEL.DAT / FOPSRC.DAT. The program deliberately LEAVES FOPCRE.DAT + FOPDST.DAT
 * on the volume for that reader; a ramfs/POSIX write can never appear in the ACP
 * directory (see tools/cross-alpha/SYSTARTUP_VMS_FILEOP_PROOF.COM).
 *
 * SENTINEL-RETURN CONVENTION (deterministic). crt0 maps the return N through
 * C$_EXIT1, so $STATUS decodes to C$_EXIT1 + (N-1)*8:
 *   7  = FULL SUCCESS (all 8 ops + self-enumeration agreement)
 *   1  = creat(FOPCRE.DAT) failed
 *   2  = open(FOPCRE.DAT, O_RDONLY) failed
 *   3  = creat(FOPDEL.DAT) failed
 *   4  = unlink(FOPDEL.DAT) failed
 *   5  = creat(FOPSRC.DAT) failed
 *   6  = rename(FOPSRC.DAT -> FOPDST.DAT) failed
 *   8  = opendir failed, OR self-enumeration disagreed (created/renamed missing,
 *        or deleted/old-name still present)
 */

/* alpha-dec-vms is LP64 (-mpointer-size=64). No libc headers in the cross image;
 * declare the CRTL surface directly — the NAMES matter for the link and the
 * cross cc1 decorates them to decc$ at codegen (matching crtl_rms_test.c). */
typedef unsigned long ovmx_size_t;

/* Alpha (OSF/1) open() flag ABI — arch/alpha uapi/asm/fcntl.h, matching the
 * OVMX alpha musl bits/fcntl.h the veneer is compiled against. */
#define O_RDONLY   0x0000
#define O_WRONLY   0x0001
#define O_CREAT    0x0200      /* 01000 */
#define O_TRUNC    0x0400      /* 02000 */

extern int   open(const char *, int);   /* non-variadic: match the veneer ABI (vms-3320) */
extern int   creat(const char *, int);
extern int   close(int);
extern int   unlink(const char *);
extern int   rename(const char *, const char *);
extern void *opendir(const char *);
extern int   closedir(void *);
extern int   printf(const char *, ...);
extern int   fprintf(void *, const char *, ...);
extern char *strstr(const char *, const char *);
extern void *stderr;

/* decc$readdir returns a struct dirent*; ovmx_crtl_readdir fills a struct whose
 * FIRST member is d_name[256] (offset 0), so this layout reads the name back. */
struct portdirent {
    char           d_name[256];
    unsigned short d_namlen;
    unsigned short d_fileid;
};
extern struct portdirent *readdir(void *);

#define DIRSPEC   "VDA0:[SYSTMP]"
#define FOPCRE    DIRSPEC "FOPCRE.DAT"     /* created, LEFT for the reader     */
#define FOPDEL    DIRSPEC "FOPDEL.DAT"     /* created then unlinked (gone)     */
#define FOPSRC    DIRSPEC "FOPSRC.DAT"     /* created then renamed away (gone) */
#define FOPDST    DIRSPEC "FOPDST.DAT"     /* rename target, LEFT for the reader */

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;

    /* 1. creat mints a real ODS-2 file (sys$create); CLOSE it so sys$close
     *    finalizes the ODS-2 header/FH2 and its File ID becomes visible to the
     *    independent reader. Without an explicit close the FID only finalizes at
     *    clean image exit -- which the post-op vms-c5d crash preempts on alpha
     *    (that is the x86_64-green / alpha-red split this closes, vms-3320). */
    int cfd = creat(FOPCRE, 0);
    if (cfd < 0)
        return 1;
    close(cfd);                          /* finalize FOPCRE's File ID */

    /* 2. open the just-created file read-only (sys$open), then close. */
    int ofd = open(FOPCRE, O_RDONLY);
    if (ofd < 0)
        return 2;
    close(ofd);

    /* 3. create a doomed temp, close it, then unlink it (sys$erase). */
    int dfd = creat(FOPDEL, 0);
    if (dfd < 0)
        return 3;
    close(dfd);
    if (unlink(FOPDEL) != 0)
        return 4;

    /* 4. create a source, CLOSE it (finalize its FID), then ATOMICALLY rename
     *    it (sys$rename -> ACP MODIFY!M_MOVE) -- the re-link carries that SAME
     *    File ID into FOPDST, which the independent reader then confirms. */
    int sfd = creat(FOPSRC, 0);
    if (sfd < 0)
        return 5;
    close(sfd);                          /* finalize FOPSRC's File ID BEFORE the rename */
    if (rename(FOPSRC, FOPDST) != 0)
        return 6;

    /* 5. SELF-CHECK: enumerate the directory via decc$opendir/readdir and
     *    confirm the created + renamed files are present, the deleted + old
     *    names gone. (The un-fakeable proof is the independent DIRECTORY reader
     *    the boot runs next — this is the same-CRTL corroboration.) */
    void *dir = opendir(DIRSPEC);
    if (!dir)
        return 8;
    int saw_cre = 0, saw_dst = 0, saw_del = 0, saw_src = 0, nent = 0;
    struct portdirent *e;
    while ((e = readdir(dir)) != 0) {
        nent++;
        if (strstr(e->d_name, "FOPCRE.DAT")) saw_cre = 1;
        if (strstr(e->d_name, "FOPDST.DAT")) saw_dst = 1;
        if (strstr(e->d_name, "FOPDEL.DAT")) saw_del = 1;
        if (strstr(e->d_name, "FOPSRC.DAT")) saw_src = 1;
    }
    closedir(dir);

    fprintf(stderr, "OVMX CRTL/RMS3 file-op test: enumerated %d entries; "
            "cre=%d dst=%d del=%d src=%d\n", nent, saw_cre, saw_dst, saw_del, saw_src);

    if (!saw_cre || !saw_dst || saw_del || saw_src)
        return 8;

    printf("OVMX CRTL/RMS3 file-op test: OK "
           "(open+creat+unlink+rename+opendir+readdir+closedir over RMS) argc=%d\n", argc);

    return 7;   /* distinctive success -> $STATUS = C$_EXIT1 + (7-1)*8 */
}
