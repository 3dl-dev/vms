/* crtl_rms2_test.c — the NEXT (richer) alpha-dec-vms CRTL/RMS port program,
 * wired as a REPRODUCIBLE joint-e2e VARIANT (build-joint-image.sh's JOINT_MAIN
 * override). It advances the vms-da0 ladder ONE rung past crtl_rms_test.c.
 *
 * crtl_rms_test.c already PROVED the byte-I/O + heap + basic-stdio DECC$ surface
 * (malloc/free, fopen/fwrite/fread/fclose, printf/fprintf, memset/memcmp) builds
 * + links zero-deferred against the GENUINE alpha DECC$SHR and activates over
 * the real Files-11 ACP. This program exercises the NEXT coherent CRTL working
 * set — the surface a real compiler/GCC port leans on — WITHOUT re-treading it:
 *
 *   - line/record I/O :  fputs (write lines) + fgets (read lines back)
 *   - positioning     :  fseek / ftell / rewind
 *   - formatted        :  sprintf / snprintf (format records) + sscanf (parse)
 *   - sort/convert    :  qsort (with a function-pointer comparator) + strtol/atoi
 *   - string          :  strlen/strcpy/strcat/strncmp/strchr/strstr
 *   - heap            :  calloc + realloc + free
 *   - env             :  getenv (tolerant — absence is not a failure)
 *
 * Every reference here is a GENUINE decc$ call: the real alpha-dec-vms cross cc1
 * decorates each name to the decc$ surface at codegen, and mk_decc_shr.sh's
 * ALPHA/EVAX branch whole-archives musl-alpha's own identically-decorated
 * definitions into DECC$SHR. No POSIX bypass, no fake success: this is the
 * shipping alpha-dec-vms PORT path.
 *
 * SENTINEL-RETURN CONVENTION (deterministic — no argv dependence; main returns a
 * constant on the success path). crt0 maps the return N through C$_EXIT1, so
 * BOOT-B decodes $STATUS as C$_EXIT1 + (N-1)*8, telling us exactly how far the
 * CRTL round-trip got:
 *
 *   7  = FULL SUCCESS  (all stages below verified)
 *   1  = calloc/realloc heap allocation failed
 *   2  = fopen(write) failed
 *   3  = fputs line write failed
 *   4  = fopen(read) failed
 *   5  = fgets read-back / line-content verify failed
 *   6  = fseek/ftell/rewind positioning verify failed
 *   8  = formatted/sort/convert/string verify failed (sprintf/sscanf/qsort/strtol/str*)
 */

/* alpha-dec-vms is LP64 (-mpointer-size=64): long/pointer are 64-bit. No libc
 * headers are set up in the cross image, so declare the CRTL surface directly;
 * the symbol NAMES are what matter for the link, and the cross cc1 decorates
 * them to the decc$ surface at codegen (matching crtl_rms_test.c's style). */
typedef unsigned long ovmx_size_t;

/* heap */
extern void *calloc(ovmx_size_t, ovmx_size_t);
extern void *realloc(void *, ovmx_size_t);
extern void  free(void *);

/* RMS stream I/O */
extern void      *fopen(const char *, const char *);
extern int        fputs(const char *, void *);
extern char      *fgets(char *, int, void *);
extern int        fseek(void *, long, int);
extern long       ftell(void *);
extern void       rewind(void *);
extern int        fclose(void *);

/* formatted */
extern int   sprintf(char *, const char *, ...);
extern int   snprintf(char *, ovmx_size_t, const char *, ...);
extern int   sscanf(const char *, const char *, ...);
extern int   printf(const char *, ...);
extern int   fprintf(void *, const char *, ...);
extern void *stderr;

/* convert */
extern long  strtol(const char *, char **, int);
extern int   atoi(const char *);
extern char *getenv(const char *);

/* sort */
extern void  qsort(void *, ovmx_size_t, ovmx_size_t,
                   int (*)(const void *, const void *));

/* string */
extern ovmx_size_t strlen(const char *);
extern char       *strcpy(char *, const char *);
extern char       *strcat(char *, const char *);
extern int         strncmp(const char *, const char *, ovmx_size_t);
extern char       *strchr(const char *, int);
extern char       *strstr(const char *, const char *);

#define R2_NAME   "PORTTEST2.DAT"
#define NLINES    5
#define SEEK_SET  0
#define SEEK_END  2

/* function-pointer comparator for qsort — exercises the EVAX cc1 codegen for
 * an indirect call through a comparator pointer (what qsort/bsearch need). */
static int cmp_int(const void *a, const void *b)
{
    int ia = *(const int *)a;
    int ib = *(const int *)b;
    return (ia > ib) - (ia < ib);
}

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;

    /* ---- 1. heap: calloc then realloc a working buffer. ---- */
    char *buf = (char *)calloc(64, sizeof(char));   /* 64 zeroed bytes */
    if (!buf)
        return 1;
    char *big = (char *)realloc(buf, 4096);
    if (!big) { free(buf); return 1; }
    buf = big;

    /* ---- 2. formatted: sprintf/snprintf format NLINES numbered records. ---- */
    /* Each line: "REC <n> VAL <7n+3>\n" — deterministic content we verify on
     * read-back. Build them into buf, one after another via strcat. */
    buf[0] = '\0';
    for (int i = 0; i < NLINES; i++) {
        char line[64];
        int n = snprintf(line, sizeof(line), "REC %d VAL %d\n", i, 7 * i + 3);
        if (n <= 0 || n >= (int)sizeof(line)) { free(buf); return 8; }
        strcat(buf, line);
    }

    /* ---- 3. RMS line I/O: create + write the records with fputs. ---- */
    void *wf = fopen(R2_NAME, "w");
    if (!wf) { free(buf); return 2; }
    if (fputs(buf, wf) < 0) { fclose(wf); free(buf); return 3; }
    fclose(wf);

    /* ---- 4. positioning + line read-back: reopen, fgets each line, verify. ---- */
    void *rf = fopen(R2_NAME, "r");
    if (!rf) { free(buf); return 4; }

    /* fseek to end + ftell to learn the byte size, then rewind. */
    if (fseek(rf, 0L, SEEK_END) != 0) { fclose(rf); free(buf); return 6; }
    long fsize = ftell(rf);
    if (fsize <= 0) { fclose(rf); free(buf); return 6; }
    rewind(rf);
    /* re-seek explicitly to SEEK_SET as a second positioning path. */
    if (fseek(rf, 0L, SEEK_SET) != 0) { fclose(rf); free(buf); return 6; }

    /* fgets each line, verify its parsed content matches the generator. */
    for (int i = 0; i < NLINES; i++) {
        char rline[128];
        if (!fgets(rline, (int)sizeof(rline), rf)) { fclose(rf); free(buf); return 5; }

        /* string search: every record must contain "VAL". */
        if (!strstr(rline, "VAL")) { fclose(rf); free(buf); return 5; }

        /* sscanf back the two integers and verify the deterministic relation. */
        int gn = -1, gv = -1;
        if (sscanf(rline, "REC %d VAL %d", &gn, &gv) != 2) { fclose(rf); free(buf); return 5; }
        if (gn != i || gv != 7 * i + 3) { fclose(rf); free(buf); return 5; }
    }
    fclose(rf);

    /* ---- 5. convert + sort + string on an in-memory record. ---- */
    /* strtol/atoi round-trip on a formatted number. */
    char numbuf[32];
    sprintf(numbuf, "%d", 12345);
    char *endp = (char *)0;
    long lv = strtol(numbuf, &endp, 10);
    if (lv != 12345) { free(buf); return 8; }
    if (atoi("678") != 678) { free(buf); return 8; }

    /* qsort an unsorted int array via the function-pointer comparator. */
    int arr[8] = { 5, 2, 9, 1, 7, 3, 8, 4 };
    qsort(arr, 8, sizeof(int), cmp_int);
    for (int i = 1; i < 8; i++)
        if (arr[i] < arr[i - 1]) { free(buf); return 8; }
    if (arr[0] != 1 || arr[7] != 9) { free(buf); return 8; }

    /* string ops: strcpy/strcat/strlen/strncmp/strchr. */
    char sbuf[64];
    strcpy(sbuf, "OVMX");
    strcat(sbuf, "/RMS");
    if (strlen(sbuf) != 8) { free(buf); return 8; }
    if (strncmp(sbuf, "OVMX", 4) != 0) { free(buf); return 8; }
    char *slash = strchr(sbuf, '/');
    if (!slash || strncmp(slash, "/RMS", 4) != 0) { free(buf); return 8; }

    /* ---- 6. env: tolerant probe (absence is NOT a failure — just exercise the
     *         genuine decc$getenv symbol so it must resolve at link time). ---- */
    const char *home = getenv("SYS$LOGIN");
    long homelen = home ? (long)strlen(home) : -1L;

    /* ---- 7. stdio variety + full-success sentinel. ---- */
    fprintf(stderr, "OVMX CRTL/RMS2 port test: %d records, %ld bytes on '%s', SYS$LOGIN len=%ld\n",
            NLINES, fsize, R2_NAME, homelen);
    printf("OVMX CRTL/RMS2 port test: OK (line-I/O+seek+fmt+qsort+convert+str) argc=%d\n", argc);

    free(buf);
    return 7;   /* distinctive success -> $STATUS = C$_EXIT1 + (7-1)*8 */
}
