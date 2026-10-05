/*
 * test_lib_filectl.c - LIB$FIND_FILE / LIB$FILE_SCAN / LIB$DELETE_FILE /
 * LIB$RENAME_FILE walk the RMS wildcard engine ($PARSE/$SEARCH), not the host
 * filesystem and not a canned list.
 *
 * Ground truth: a temp directory is mounted as a physical device (the system-disk device) in the
 * host device table; files are created through POSIX under it; the LIB$ routines
 * then find, scan, rename and delete them by VMS filespec. Runs on the host with
 * no /dev/vms (the executive-absent RMS POSIX backend), so it exercises the
 * routines' own logic -- context handling, default-spec fill, NMF/FNF
 * discipline, callback contracts -- against files that really exist, and the
 * assertions are about the files that remain on disk afterwards, not about the
 * routines' own return values alone.
 *
 * Clean-room: expected behaviour is the public OpenVMS RTL Library (LIB$) Manual.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>

#include "ssdef.h"
#include "descrip.h"
#include "rmsdef.h"
#include "lib$routines.h"
#include "vmsfs/device.h"
#include "ovmx_layout.h"

/* The temp tree is mounted AS the system disk: RMS confines resolved host paths to
 * the system-disk root, so any other device would be refused as out-of-bounds. */
#define DEV SYSDISK_DEVICE ":"
#include "rms.h"

/* Force-bind anchor: lib$find_file reaches RMS through weak references (the
 * LIBVMS-below-RMS layering seam), so a static link needs a strong reference to
 * pull the RMS parse/search objects in (cf. tests/qemu/rms_acp_bind.c). */
static void *const rms_anchor[] = { (void *)sys$parse, (void *)sys$search,
                                    (void *)rms_search_end };

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char root[256];

static void mkfile(const char *rel)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, rel);
    FILE *f = fopen(p, "w");
    if (f) { fputs("x\n", f); fclose(f); }
}

/* True when directory root/<dir> holds a file named <name>, ignoring case and a
 * ";version" suffix: the host POSIX RMS backend stores renamed files as
 * "name.typ;1" in lower case, which is its own naming convention and not the
 * subject here -- what matters is which FILES exist afterwards. */
static int exists_ci(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", root, dir);
    DIR *d = opendir(p);
    if (!d) return 0;
    int found = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char n[256];
        size_t k = 0;
        for (; e->d_name[k] && e->d_name[k] != ';' && k < 255; k++)
            n[k] = (char)toupper((unsigned char)e->d_name[k]);
        n[k] = '\0';
        if (strcmp(n, name) == 0) { found = 1; break; }
    }
    closedir(d);
    return found;
}

static void dsc(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length = (uint16_t)strlen(s);
    d->dsc$b_dtype = DSC$K_DTYPE_T;
    d->dsc$b_class = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

/* Collect every match of `spec` (default `dflt`) via LIB$FIND_FILE. */
static int find_all(const char *spec, const char *dflt, char out[][256], int max,
                    uint32_t *final_status)
{
    struct dsc$descriptor_s sd, dd, rd;
    char res[256];
    uint32_t ctx = 0, stv = 0, st;
    int n = 0;

    dsc(&sd, spec);
    if (dflt) dsc(&dd, dflt);
    for (;;) {
        rd.dsc$w_length = sizeof res;
        rd.dsc$b_dtype = DSC$K_DTYPE_T;
        rd.dsc$b_class = DSC$K_CLASS_S;
        rd.dsc$a_pointer = res;
        memset(res, ' ', sizeof res);
        st = lib$find_file(&sd, &rd, &ctx, dflt ? &dd : NULL, NULL, &stv, NULL);
        if (!(st & 1))
            break;
        if (n < max) {
            size_t l = sizeof res;
            while (l > 0 && res[l - 1] == ' ') l--;
            memcpy(out[n], res, l);
            out[n][l] = '\0';
            n++;
        }
    }
    if (final_status) *final_status = st;
    (void)lib$find_file_end(&ctx);
    return n;
}

static int has(char out[][256], int n, const char *needle)
{
    for (int i = 0; i < n; i++) {
        /* case-blind substring: the resultant is a full DEV:[DIR]NAME.TYP;n spec */
        char a[256], b[256];
        size_t k;
        for (k = 0; out[i][k] && k < 255; k++) a[k] = (char)toupper((unsigned char)out[i][k]);
        a[k] = '\0';
        for (k = 0; needle[k] && k < 255; k++) b[k] = (char)toupper((unsigned char)needle[k]);
        b[k] = '\0';
        if (strstr(a, b)) return 1;
    }
    return 0;
}

static int scan_count, scan_err_count;
static int ok_cb(struct FAB *fab) { (void)fab; scan_count++; return SS$_NORMAL; }
static int err_cb(struct FAB *fab) { (void)fab; scan_err_count++; return SS$_NORMAL; }

int main(void)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    if (!rms_anchor[0]) return 2;
    printf("=== test_lib_filectl (LIB$FIND_FILE/FILE_SCAN/DELETE_FILE/RENAME_FILE over RMS) ===\n");

    snprintf(root, sizeof root, "/tmp/ovmx_libfilectl_%d", (int)getpid());
    mkdir(root, 0755);
    char sub[300];
    snprintf(sub, sizeof sub, "%s/DATA", root);
    mkdir(sub, 0755);
    mkfile("DATA/ALPHA1.TXT");
    mkfile("DATA/ALPHA2.TXT");
    mkfile("DATA/BETA1.DAT");
    mkfile("DATA/GAMMA.TXT");

    if (!(vmsfs_device_add(SYSDISK_DEVICE ":", root) & 1)) {
        printf("  FAIL: could not mount the temp tree as " DEV "\n");
        return 2;
    }

    char out[16][256];
    uint32_t fst = 0;
    int n;

    /* ---- LIB$FIND_FILE ---- */
    n = find_all(DEV "[DATA]ALPHA*.TXT", NULL, out, 16, &fst);
    CHECK(n == 2, "find_file: ALPHA*.TXT matches exactly the two ALPHA files");
    CHECK(has(out, n, "ALPHA1.TXT") && has(out, n, "ALPHA2.TXT"),
          "find_file: both ALPHA1.TXT and ALPHA2.TXT are returned");
    CHECK(fst == RMS$_NMF, "find_file: the sequence ends with RMS$_NMF");
    CHECK(!has(out, n, "BETA1") && !has(out, n, "GAMMA"),
          "find_file: files the wildcard does not match are not returned");

    n = find_all("*.TXT", DEV "[DATA]", out, 16, &fst);
    printf("  INFO: default-spec find: %d match(es), final status 0x%X\n", n, fst);
    for (int i = 0; i < n; i++) printf("        %s\n", out[i]);
    CHECK(n == 3 && has(out, n, "GAMMA.TXT"),
          "find_file: the default filespec supplies the device and directory");

    n = find_all(DEV "[DATA]NOSUCH*.*", NULL, out, 16, &fst);
    CHECK(n == 0 && (fst == RMS$_NMF || fst == RMS$_FNF),
          "find_file: a wildcard with no match returns NMF/FNF and no names");

    {
        struct dsc$descriptor_s sd, rd, rel;
        char res[256];
        uint32_t ctx = 0, stv = 0;
        dsc(&sd, DEV "[DATA]*.TXT");
        dsc(&rel, DEV "[DATA]X.Y");
        rd.dsc$w_length = sizeof res; rd.dsc$b_dtype = DSC$K_DTYPE_T;
        rd.dsc$b_class = DSC$K_CLASS_S; rd.dsc$a_pointer = res;
        uint32_t st = lib$find_file(&sd, &rd, &ctx, NULL, &rel, &stv, NULL);
        CHECK(!(st & 1) && ctx == 0,
              "find_file: a related filespec is refused, not silently ignored");
    }

    /* ---- LIB$FILE_SCAN ---- */
    {
        struct FAB fab = cc$rms_fab;
        struct NAM nam = cc$rms_nam;
        char rsa[256];                       /* NO ESA: the scan lends its own */
        char spec[] = DEV "[DATA]*.TXT";
        unsigned int context = 0;
        uint32_t st;

        fab.fab$l_fna = spec;
        fab.fab$b_fns = (uint8_t)strlen(spec);
        nam.nam$l_rsa = rsa;
        nam.nam$b_rss = 255;
        fab.fab$l_nam = &nam;

        scan_count = scan_err_count = 0;
        st = lib$file_scan((unsigned int *)&fab, (int (*)())ok_cb, (int (*)())err_cb,
                           &context);
        CHECK(st == RMS$_NMF, "file_scan: the scan ends with RMS$_NMF");
        CHECK(scan_count == 3, "file_scan: the success routine ran once per .TXT file (3)");
        CHECK(scan_err_count == 0, "file_scan: the error routine did not run");
        CHECK(nam.nam$l_esa == NULL, "file_scan: the NAM is left as the caller gave it (no ESA)");
        (void)lib$file_scan_end((unsigned int *)&fab, &context);

        /* A search that fails other than by running out of files must END the
         * scan with that status, not loop on it. */
        struct FAB bf = cc$rms_fab;
        struct NAM bn = cc$rms_nam;
        char bspec[] = DEV "[NOSUCHDIR]*.*";
        char brsa[256];
        unsigned int bctx = 0;
        bf.fab$l_fna = bspec;
        bf.fab$b_fns = (uint8_t)strlen(bspec);
        bn.nam$l_rsa = brsa;
        bn.nam$b_rss = 255;
        bf.fab$l_nam = &bn;
        scan_count = scan_err_count = 0;
        st = lib$file_scan((unsigned int *)&bf, (int (*)())ok_cb, (int (*)())err_cb, &bctx);
        CHECK(scan_count == 0 && scan_err_count <= 1,
              "file_scan: a missing directory runs no success routine and ends (no loop)");
        CHECK(st != RMS$_NORMAL, "file_scan: a missing directory is not reported as success");
        (void)lib$file_scan_end((unsigned int *)&bf, &bctx);
    }

    /* ---- LIB$RENAME_FILE ---- */
    {
        struct dsc$descriptor_s oldd, newd;
        dsc(&oldd, DEV "[DATA]GAMMA.TXT");
        dsc(&newd, DEV "[DATA]DELTA.TXT");
        uint32_t st = lib$rename_file(&oldd, &newd, NULL, NULL, NULL, NULL, NULL, NULL,
                                      NULL, NULL, NULL, NULL);
        printf("  INFO: rename status 0x%X\n", st);

        CHECK(st & 1, "rename_file: GAMMA.TXT -> DELTA.TXT succeeds");
        CHECK(!exists_ci("DATA", "GAMMA.TXT") && exists_ci("DATA", "DELTA.TXT"),
              "rename_file: on disk the old name is gone and the new name exists");
    }

    /* ---- LIB$DELETE_FILE ---- */
    {
        struct dsc$descriptor_s d;
        dsc(&d, DEV "[DATA]ALPHA*.TXT");
        uint32_t st = lib$delete_file(&d, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
                                      NULL);
        printf("  INFO: delete status 0x%X\n", st);
        CHECK(st & 1, "delete_file: ALPHA*.TXT deletion succeeds");
        CHECK(!exists_ci("DATA", "ALPHA1.TXT") && !exists_ci("DATA", "ALPHA2.TXT"),
              "delete_file: both ALPHA files are gone from disk");
        CHECK(exists_ci("DATA", "BETA1.DAT") && exists_ci("DATA", "DELTA.TXT"),
              "delete_file: files the wildcard does not match survive");
    }

    /* cleanup */
    {
        char p[512];
        snprintf(p, sizeof p, "%s/DATA", root);
        DIR *d = opendir(p);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
                char f[1024];
                snprintf(f, sizeof f, "%s/DATA/%s", root, e->d_name);
                unlink(f);
            }
            closedir(d);
        }
        rmdir(p);
        rmdir(root);
    }

    printf("=== test_lib_filectl: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
