/*
 * vmsabi_rms_test.c - the VMS-ABI RMS services (vms-692) driven the way GCC's
 * VMS-host gcc/vmsdbgout.cc vms_file_stats_name drives them: a DEC C program
 * at the default 32-bit pointer size includes <vms/rms.h>, fills a FAB/NAM in
 * the VMS byte layout and calls SYS$PARSE then SYS$SEARCH by their upper-case
 * names. Checks: the expanded and resultant strings land in the caller's
 * ESA/RSA with the component addresses pointing into them, NAM$W_FID and
 * NAM$W_DID are a real File ID / directory ID, the next $SEARCH of a
 * non-wildcard spec is RMS$_NMF, and a wildcard spec walks several files.
 * The proof's independent half is DCL DIRECTORY/FULL of the same file, which
 * must print the same File ID (SYSTARTUP_VMS_VMSABI_PROOF.COM).
 */
#define __NEW_STARLET 1
#include <stdio.h>
#include <string.h>
#include <vms/rms.h>
#include <vms/stsdef.h>
#include <vms/descrip.h>
#include <vms/lnmdef.h>
#include <vms/starlet.h>
#include <vms/atrdef.h>
#include <vms/fibdef.h>
#include <vms/fatdef.h>
#include <vms/iodef.h>
#include <vms/ssdef.h>

#define RMS_NMF 99018           /* RMS$_NMF (rmsdef) */
#define RMS_FAB 99596           /* RMS$_FAB */
#define RMS_IFI 99684           /* RMS$_IFI */
#define RMS_IOP 99700           /* RMS$_IOP */

static int fails, first_fail;

/* vms-7b3: LIBRTL string routines by their VMS names. Each takes either
 * descriptor form; the 64-bit one (DSC64) is told apart by MBO=1/MBMO=-1. */
extern int STR$COPY_DX(void *dst, void *src);
extern int STR$PREFIX(void *dst, void *src);
extern int STR$FREE1_DX(void *dst);
extern int LIB$GET_FOREIGN(void *res, ...);
extern int LIB$GET_SYMBOL(void *sym, void *val, ...);
struct dsc64_t {
    unsigned short mbo;
    unsigned char dtype, cls;
    int mbmo;
    unsigned long long len, ptr;
};
#define DSC64_INIT(d, cl, l, p) \
    do { (d).mbo = 1; (d).dtype = DSC$K_DTYPE_T; (d).cls = (cl); (d).mbmo = -1; \
         (d).len = (l); (d).ptr = (unsigned long long)(unsigned long)(p); } while (0)

static void check(int ok, int code, const char *what)
{
    printf("VMSABI: %s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok && !fails++)
        first_fail = code;
}

int main(void)
{
    static char spec[] = "SYS$SYSTEM:JOINT_E2E.EXE";
    char es[NAM$C_MAXRSS + 1], rs[NAM$C_MAXRSS + 1];
    struct FAB fab;
    struct NAM nam;
    int st;

    setvbuf(stdout, NULL, _IONBF, 0);
    fab = cc$rms_fab;
    nam = cc$rms_nam;
    nam.nam$l_esa = es;
    nam.nam$b_ess = NAM$C_MAXRSS;
    nam.nam$l_rsa = rs;
    nam.nam$b_rss = NAM$C_MAXRSS;
    fab.fab$l_fna = spec;
    fab.fab$b_fns = (unsigned char)strlen(spec);
    fab.fab$l_nam = &nam;

    st = SYS$PARSE(&fab, 0, 0);
    es[nam.nam$b_esl] = 0;
    printf("VMSABI: $PARSE st=%%X%08X esl=%u es=\"%s\" fnb=%%X%08X dev=\"%.*s\"\n",
           (unsigned)st, nam.nam$b_esl, es, nam.nam$l_fnb, nam.nam$b_dev, nam.nam$l_dev);
    check((st & STS$M_SUCCESS) && fab.fab$l_sts == (unsigned)st, 10, "SYS$PARSE succeeds, FAB$L_STS = R0");
    check(nam.nam$b_esl > 0 && strstr(es, "JOINT_E2E.EXE") != NULL, 11, "expanded string in the caller's ESA");
    check(nam.nam$l_name >= es && nam.nam$l_name < es + nam.nam$b_esl &&
              strncmp(nam.nam$l_name, "JOINT_E2E", nam.nam$b_name) == 0 && nam.nam$b_name == 9,
          12, "NAM$L_NAME/NAM$B_NAME address the name inside the caller's ESA");
    check(nam.nam$b_type == 4 && strncmp(nam.nam$l_type, ".EXE", 4) == 0, 13, "NAM$L_TYPE is .EXE");
    check((nam.nam$l_fnb & NAM$M_EXP_NAME) && (nam.nam$l_fnb & NAM$M_EXP_TYPE), 14,
          "NAM$L_FNB says name and type were given");

    st = SYS$SEARCH(&fab, 0, 0);
    rs[nam.nam$b_rsl] = 0;
    printf("VMSABI: $SEARCH st=%%X%08X rsl=%u rs=\"%s\" FID=(%u,%u,%u) DID=(%u,%u,%u)\n",
           (unsigned)st, nam.nam$b_rsl, rs, nam.nam$w_fid[0], nam.nam$w_fid[1], nam.nam$w_fid[2],
           nam.nam$w_did[0], nam.nam$w_did[1], nam.nam$w_did[2]);
    check(st & STS$M_SUCCESS, 20, "SYS$SEARCH finds the file");
    check(nam.nam$b_rsl > 0 && strchr(rs, ';') != NULL && strchr(rs, '[') != NULL, 21,
          "resultant string with directory and version in the caller's RSA");
    check(nam.nam$w_fid[0] != 0 && nam.nam$w_fid[1] != 0, 22, "NAM$W_FID is a File ID");
    check(nam.nam$w_did[0] != 0, 23, "NAM$W_DID is the directory's File ID");
    check(nam.nam$l_dev >= rs && nam.nam$l_dev < rs + nam.nam$b_rsl && nam.nam$b_dev > 1 &&
              nam.nam$l_dev[nam.nam$b_dev - 1] == ':',
          24, "NAM$L_DEV/NAM$B_DEV address the device inside the caller's RSA");
    printf("VMSABI-FID: (%u,%u,%u)\n", nam.nam$w_fid[0], nam.nam$w_fid[1], nam.nam$w_fid[2] & 0xFF);

    /* vms-38b: $ASSIGN the device $SEARCH named, through a 32-bit descriptor
     * (as vmsdbgout.cc does), then $DASSGN it; a second $DASSGN must fail. */
    char devname[32];
    unsigned devlen = nam.nam$b_dev < sizeof devname ? nam.nam$b_dev : sizeof devname - 1;
    memcpy(devname, nam.nam$l_dev, devlen);
    struct dsc$descriptor_s devdsc = { (unsigned short)devlen, DSC$K_DTYPE_T, DSC$K_CLASS_S, devname };
    unsigned short chan = 0;
    st = SYS$ASSIGN(&devdsc, &chan, 0, 0, 0);
    printf("VMSABI: $ASSIGN %.*s st=%%X%08X chan=%u\n", (int)devlen, devname, (unsigned)st, chan);
    check((st & 1) && chan != 0, 30, "SYS$ASSIGN with a 32-bit descriptor gives a channel");
    /* vms-38b: $QIOW IO$_ACCESS!IO$M_ACCESS on that channel, the way GCC's
     * vms_file_stats_name asks the ACP for a file's attributes: a 32-bit FIB
     * descriptor with the directory's File ID, the file name, the resultant
     * name, and an attribute list. */
    {
        FIBDEF fib;
        memset(&fib, 0, sizeof fib);
        fib.fib$w_did[0] = nam.nam$w_did[0];
        fib.fib$w_did[1] = nam.nam$w_did[1];
        fib.fib$w_did[2] = nam.nam$w_did[2];
        struct { unsigned int len; void *addr; } fibdsc = { sizeof fib, &fib };
        char fname[64];
        const char *rb = strrchr(rs, ']');
        snprintf(fname, sizeof fname, "%s", rb ? rb + 1 : rs);
        struct dsc$descriptor_s filedsc = { (unsigned short)strlen(fname), DSC$K_DTYPE_T, DSC$K_CLASS_S, fname };
        char res[64];
        unsigned short reslen = 0;
        struct dsc$descriptor_s resdsc = { sizeof res, DSC$K_DTYPE_T, DSC$K_CLASS_S, res };
        long long cre = 0;
        FAT recattr;
        char asc[ATR$S_ASCNAME];
        unsigned int uchar = 0, uic = 0;
        unsigned short fpro = 0;
        memset(&recattr, 0, sizeof recattr);
        ATRDEF atr[] = {
            { ATR$S_ASCNAME, ATR$C_ASCNAME, asc },
            { ATR$S_CREDATE, ATR$C_CREDATE, &cre },
            { ATR$S_RECATTR, ATR$C_RECATTR, &recattr },
            { ATR$S_UCHAR, ATR$C_UCHAR, &uchar },
            { ATR$S_FPRO, ATR$C_FPRO, &fpro },
            { ATR$S_UIC, ATR$C_UIC, &uic },
            { 0, 0, 0 }
        };
        struct { unsigned short status, count; unsigned int dev; } iosb = { 0, 0, 0 };
        st = SYS$QIOW(0, chan, IO$_ACCESS | IO$M_ACCESS, &iosb, 0, 0,
                      &fibdsc, &filedsc, &reslen, &resdsc, atr, 0);
        unsigned an = 0;
        while (an < sizeof asc && asc[an] != ' ')
            an++;
        int padded = 1;
        for (unsigned i = an; i < sizeof asc; i++)
            if (asc[i] != ' ')
                padded = 0;
        printf("VMSABI-QIO: st=%%X%08X iosb=%%X%04X fid=(%u,%u,%u) res=\"%.*s\" asc=\"%.*s\" "
               "rtype=%u rsize=%u cre=%%X%016llX uchar=%%X%08X fpro=%%X%04X uic=[%o,%o]\n",
               (unsigned)st, iosb.status, fib.fib$w_fid[0], fib.fib$w_fid[1], fib.fib$w_fid[2] & 0xFF,
               (int)reslen, res, (int)an, asc, recattr.fat$v_rtype, recattr.fat$w_rsize,
               (unsigned long long)cre, uchar, fpro, uic >> 16, uic & 0xFFFF);
        printf("VMSABI-QIOFID: (%u,%u,%u)\n", fib.fib$w_fid[0], fib.fib$w_fid[1], fib.fib$w_fid[2] & 0xFF);
        check((st & 1) && (iosb.status & 1), 40, "SYS$QIOW IO$_ACCESS with an attribute list succeeds");
        check(fib.fib$w_fid[0] == nam.nam$w_fid[0] && fib.fib$w_fid[1] == nam.nam$w_fid[1], 41,
              "the FIB comes back with the File ID $SEARCH found");
        check(reslen == strlen(fname) && memcmp(res, fname, reslen) == 0, 42,
              "P3/P4 receive the resultant NAME.TYP;VER");
        check(an == strlen(fname) && memcmp(asc, fname, an) == 0 && padded, 43,
              "ATR$C_ASCNAME is NAME.TYP;VER padded with spaces");
        /* The gate checks ATR$C_CREDATE against the date DIRECTORY/FULL shows. */
        printf("VMSABI-QIOCRE: %%X%016llX\n", (unsigned long long)cre);
        check(cre != 0 && recattr.fat$v_rtype != 0, 44, "ATR$C_CREDATE and ATR$C_RECATTR are filled");
        st = SYS$QIOW(0, chan, IO$_DEACCESS, &iosb, 0, 0, &fibdsc, 0, 0, 0, atr, 0);
        check((st & 1) && (iosb.status & 1), 45, "SYS$QIOW IO$_DEACCESS releases the read access");
        ATRDEF bad[] = { { 4, 99, &uchar }, { 0, 0, 0 } };
        st = SYS$QIOW(0, chan, IO$_ACCESS, &iosb, 0, 0, &fibdsc, &filedsc, 0, 0, bad, 0);
        check((st & 1) && iosb.status == SS$_BADATTRIB, 46, "an unknown attribute code is SS$_BADATTRIB");
    }

    st = SYS$DASSGN(chan);
    check(st & 1, 31, "SYS$DASSGN releases it");
    check(!(SYS$DASSGN(chan) & 1), 32, "a second SYS$DASSGN of the same channel fails");

    /* vms-38b: $CRELNM in LNM$SYSTEM through a 32-bit ILE3 item list, $TRNLNM
     * back; DCL SHOW LOGICAL (another process) reads it after the image ends. */
    struct { unsigned short len, code; void *buf; unsigned short *retlen; } items[2];
    static char eqv[] = "PROVED_BY_CRELNM";
    static char tab[] = "LNM$SYSTEM", lnm[] = "VMSABI_GATE";
    struct dsc$descriptor_s tabdsc = { sizeof tab - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, tab };
    struct dsc$descriptor_s lnmdsc = { sizeof lnm - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, lnm };
    memset(items, 0, sizeof items);
    items[0].len = sizeof eqv - 1;
    items[0].code = LNM$_STRING;
    items[0].buf = eqv;
    st = SYS$CRELNM(0, &tabdsc, &lnmdsc, 0, items);
    printf("VMSABI: $CRELNM LNM$SYSTEM VMSABI_GATE st=%%X%08X\n", (unsigned)st);
    check(st & 1, 33, "SYS$CRELNM with a 32-bit ILE3 list defines the name");
    char back[64];
    unsigned short backlen = 0;
    memset(items, 0, sizeof items);
    items[0].len = sizeof back;
    items[0].code = LNM$_STRING;
    items[0].buf = back;
    items[0].retlen = &backlen;
    st = SYS$TRNLNM(0, &tabdsc, &lnmdsc, 0, items);
    printf("VMSABI: $TRNLNM st=%%X%08X -> \"%.*s\"\n", (unsigned)st, (int)backlen, back);
    check((st & 1) && backlen == sizeof eqv - 1 && memcmp(back, eqv, backlen) == 0, 34,
          "SYS$TRNLNM returns the equivalence string and its length through the item list");

    st = SYS$SEARCH(&fab, 0, 0);
    check(st == RMS_NMF && nam.nam$l_wcc == 0, 25, "the next $SEARCH is RMS$_NMF and drops the context");

    /* A wildcard walks the directory. */
    static char wild[] = "SYS$SYSTEM:*.EXE";
    fab.fab$l_fna = wild;
    fab.fab$b_fns = (unsigned char)strlen(wild);
    st = SYS$PARSE(&fab, 0, 0);
    int n = 0, saw_self = 0;
    while (st & 1) {
        st = SYS$SEARCH(&fab, 0, 0);
        if (!(st & 1))
            break;
        rs[nam.nam$b_rsl] = 0;
        n++;
        if (strstr(rs, "JOINT_E2E.EXE"))
            saw_self = 1;
    }
    printf("VMSABI: wildcard SYS$SYSTEM:*.EXE -> %d match(es), end st=%%X%08X\n", n, (unsigned)st);
    check(n >= 2 && saw_self && st == RMS_NMF, 26, "a wildcard $SEARCH walks the directory to RMS$_NMF");

    /* vms-5303e: $MODIFY (bfd/vms-misc.c calls it) is honest. An open file gets
     * RMS$_IOP and stays as it was; a FAB never opened gets RMS$_IFI; a block
     * that is not a FAB gets RMS$_FAB. Never a success that changed nothing. */
    {
        struct FAB mf = cc$rms_fab;
        struct FAB nf = cc$rms_fab;
        struct FAB bad = cc$rms_fab;
        unsigned char rfm_before;

        mf.fab$l_fna = spec;
        mf.fab$b_fns = (unsigned char)strlen(spec);
        st = SYS$OPEN(&mf, 0, 0);
        check(st & STS$M_SUCCESS, 27, "SYS$OPEN opens the image file");
        rfm_before = mf.fab$b_rfm;
        mf.fab$b_rfm = (unsigned char)(rfm_before == FAB$C_FIX ? FAB$C_VAR : FAB$C_FIX);
        st = SYS$MODIFY(&mf, 0, 0);
        check(st == RMS_IOP && !(st & STS$M_SUCCESS) && mf.fab$l_sts == (unsigned)st, 28,
              "SYS$MODIFY on an open file is RMS$_IOP, not a success");
        st = SYS$CLOSE(&mf, 0, 0);
        check(st & STS$M_SUCCESS, 29, "SYS$CLOSE closes it");
        nf.fab$l_fna = spec;
        nf.fab$b_fns = (unsigned char)strlen(spec);
        st = SYS$MODIFY(&nf, 0, 0);
        check(st == RMS_IFI, 30, "negative control: SYS$MODIFY on a FAB never opened is RMS$_IFI");
        bad.fab$b_bid = 0;
        st = SYS$MODIFY(&bad, 0, 0);
        check(st == RMS_FAB, 31, "negative control: SYS$MODIFY on a block that is not a FAB is RMS$_FAB");
    }

    /* vms-7b3: STR$COPY_DX and STR$PREFIX with the 32-bit form and with
     * DSC64 give the same string; LIB$GET_FOREIGN returns the foreign command
     * line (SYSTARTUP invokes this image as VMSABI_T DSCPROBE) into the form
     * the DCL symbol VMSABI_FORM names ("32" on the first invocation, "64" on
     * the second -- the line is the CLI's to give once per activation). */
    {
        static char head[] = "ODS2-", tail[] = "VMSABI";
        struct dsc$descriptor_s h32 = { 5, DSC$K_DTYPE_T, DSC$K_CLASS_S, head };
        struct dsc$descriptor_s t32 = { 6, DSC$K_DTYPE_T, DSC$K_CLASS_S, tail };
        struct dsc$descriptor_s r32 = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, 0 };
        struct dsc64_t h64, t64, r64;
        DSC64_INIT(h64, DSC$K_CLASS_S, 5, head);
        DSC64_INIT(t64, DSC$K_CLASS_S, 6, tail);
        DSC64_INIT(r64, DSC$K_CLASS_D, 0, 0);
        int s1 = STR$COPY_DX(&r32, &t32), s2 = STR$PREFIX(&r32, &h32);
        int s3 = STR$COPY_DX(&r64, &t64), s4 = STR$PREFIX(&r64, &h64);
        printf("VMSABI-DSC32: st=%%X%08X,%%X%08X \"%.*s\"\n", (unsigned)s1, (unsigned)s2,
               (int)r32.dsc$w_length, r32.dsc$a_pointer ? r32.dsc$a_pointer : "");
        printf("VMSABI-DSC64: st=%%X%08X,%%X%08X mbo=%u mbmo=%d \"%.*s\"\n", (unsigned)s3,
               (unsigned)s4, r64.mbo, r64.mbmo, (int)r64.len,
               r64.ptr ? (char *)(unsigned long)r64.ptr : "");
        check((s1 & s2 & 1) && r32.dsc$w_length == 11 && !memcmp(r32.dsc$a_pointer, "ODS2-VMSABI", 11),
              32, "STR$COPY_DX+STR$PREFIX into a 32-bit class-D descriptor give ODS2-VMSABI");
        check((s3 & s4 & 1) && r64.mbo == 1 && r64.mbmo == -1 && r64.len == 11 && r64.ptr
              && !memcmp((char *)(unsigned long)r64.ptr, "ODS2-VMSABI", 11),
              33, "STR$COPY_DX+STR$PREFIX into a DSC64 class-D descriptor give ODS2-VMSABI");
        STR$FREE1_DX(&r32);
        STR$FREE1_DX(&r64);

        static char symnam[] = "VMSABI_FORM";
        char formbuf[8];
        unsigned short formlen = 0;
        struct dsc$descriptor_s sym = { 11, DSC$K_DTYPE_T, DSC$K_CLASS_S, symnam };
        struct dsc$descriptor_s fv = { sizeof formbuf, DSC$K_DTYPE_T, DSC$K_CLASS_S, formbuf };
        int sf = LIB$GET_SYMBOL(&sym, &fv, &formlen);
        int want64 = (sf & 1) && formlen == 2 && formbuf[0] == '6';
        unsigned short flen = 0;
        int sg;
        const char *fp;
        unsigned fl;
        struct dsc$descriptor_s f32 = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, 0 };
        struct dsc64_t f64;
        DSC64_INIT(f64, DSC$K_CLASS_D, 0, 0);
        if (want64) {
            sg = LIB$GET_FOREIGN(&f64, 0, &flen);
            fp = f64.ptr ? (const char *)(unsigned long)f64.ptr : "";
            fl = (unsigned)f64.len;
        } else {
            sg = LIB$GET_FOREIGN(&f32, 0, &flen);
            fp = f32.dsc$a_pointer ? f32.dsc$a_pointer : "";
            fl = f32.dsc$w_length;
        }
        printf("VMSABI-FOREIGN%s: st=%%X%08X len=%u \"%.*s\"\n", want64 ? "64" : "32",
               (unsigned)sg, (unsigned)flen, (int)fl, fp);
        check((sg & 1) && fl == 8 && flen == 8 && !memcmp(fp, "DSCPROBE", 8), 34,
              want64 ? "LIB$GET_FOREIGN into a DSC64 returns the foreign command line"
                     : "LIB$GET_FOREIGN into a 32-bit descriptor returns the foreign command line");
        if (want64) {
            /* The second invocation proves the DSC64 form only. */
            if (fails) {
                printf("OVMX VMSABI DSC64 run: %d check(s) FAILED (first %d)\n", fails, first_fail);
                return first_fail;
            }
            printf("OVMX VMSABI DSC64 run: OK\n");
            return 7;
        }
    }

    if (fails) {
        printf("OVMX VMSABI RMS test: %d check(s) FAILED (first %d)\n", fails, first_fail);
        return first_fail;
    }
    printf("OVMX VMSABI RMS test: OK ($PARSE/$SEARCH over VMS-layout FAB/NAM)\n");
    return 7;
}
