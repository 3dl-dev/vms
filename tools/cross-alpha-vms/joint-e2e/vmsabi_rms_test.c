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

#define RMS_NMF 99018           /* RMS$_NMF (rmsdef) */

static int fails, first_fail;

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

    if (fails) {
        printf("OVMX VMSABI RMS test: %d check(s) FAILED (first %d)\n", fails, first_fail);
        return first_fail;
    }
    printf("OVMX VMSABI RMS test: OK ($PARSE/$SEARCH over VMS-layout FAB/NAM)\n");
    return 7;
}
