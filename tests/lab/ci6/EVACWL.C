/*
 * EVACWL.C - the evacuation workload (vms-06c).
 *
 * Takes the named lock EVAC$WORKLOAD EX (optionally starting NL and
 * converting, when invoked with "standby"), then loops: $GETTIM, format a
 * fixed 64-byte record (this node's SCSNODE via $GETSYI, this process's PID,
 * a sequence number, the current time), $PUT it to EVAC.DAT (RMS, path
 * EVAC$DATA:EVAC.DAT -- RMS resolves the EVAC$DATA logical name itself, as
 * part of ordinary file-spec parsing; no explicit $TRNLNM call), $FLUSH,
 * sleep 1 second ($SCHDWK + $HIBER). On start it reads the file's LAST
 * record (a sequential scan to EOF; these files are small) and continues
 * at that record's seq+1 -- the proof that a standby instance taking over
 * resumes the sequence rather than restarting it.
 *
 * This is the SAME CONTRACT tests/lab/ci6/EVACWL.MAR runs on a real VAX
 * V7.3 node (that oracle has MACRO + LINK, no C compiler): one resource
 * name, one record layout, byte-identical on both images (the companion
 * file's header repeats this layout verbatim so the two sources do not
 * drift apart).
 *
 * RECORD LAYOUT (EVAC.DAT, RMS sequential, fixed 64-byte records, no
 * terminator byte -- FAB$C_FIX / FAB$W_MRS=64):
 *
 *   bytes  0-15  NODE  (16 bytes, ASCII, left-justified, space-padded --
 *                       SYI$_SCSNODE / $GETSYI)
 *   bytes 16-25  PID   (10 bytes, ASCII decimal, right-justified,
 *                       space-padded -- JPI$_PID / $GETJPI, this process)
 *   bytes 26-35  SEQ   (10 bytes, ASCII decimal, right-justified,
 *                       space-padded -- this record's sequence number)
 *   bytes 36-63  TIME  (28 bytes, ASCII, left-justified, space-padded --
 *                       the 23-char $ASCTIM string "DD-MMM-YYYY HH:MM:SS.CC")
 *
 * LOCK NAMESPACE (document what group the name lands in, per the item):
 * $ENQW is called WITHOUT LCK$M_SYSTEM, so on real OpenVMS the resource
 * name "EVAC$WORKLOAD" is scoped to the REQUESTING PROCESS'S UIC GROUP, not
 * system-wide -- two EVACWL instances only contend for the SAME resource if
 * they run under the SAME UIC group (the lab runs both under group 0,
 * SYSTEM, so this is effectively system-wide there; a different group on
 * either side would make them invisible to each other despite the identical
 * name). OVMX's own lock manager does NOT implement that scoping today:
 * resource_find() (src/kernel-core/vms_lock.c) keys its hash purely by the
 * resource NAME TEXT, with no UIC/group component at all, so on OVMX the
 * name is effectively global regardless of LCK$M_SYSTEM or UIC group. That
 * happens to agree with the lab's group-0 case; it is a real executive
 * fidelity gap, not something this program should paper over, so it is
 * recorded here rather than silently relied on.
 *
 * Built by OVMX's own toolchain (TCC + LINK.EXE + DECC$SHR), run under
 * tests/qemu against a live vms.ko -- see EVACWL.COM for the MACRO side's
 * build+run and docs in this same directory.
 */

#define __NEW_STARLET 1

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ssdef.h>
#include <stsdef.h>
#include <descrip.h>
#include <lckdef.h>
#include <rmsdef.h>
#include <efndef.h>
#include <lnmdef.h>   /* struct item_list_3 */
#include <gen64def.h> /* GENERIC_64 */
#include <starlet.h>
#include <rms.h>

#define RECLEN       64
#define FLD_NODE_OFF 0
#define FLD_NODE_LEN 16
#define FLD_PID_OFF  16
#define FLD_PID_LEN  10
#define FLD_SEQ_OFF  26
#define FLD_SEQ_LEN  10
#define FLD_TIME_OFF 36
#define FLD_TIME_LEN 28

/* JPI$_PID / SYI$_SCSNODE item codes come from the tree's own headers
 * (prcdef.h); real-values are oracle-checked at build time
 * (tools/compat/check_oracle_constants.py), not re-derived here. */
#include <prcdef.h>

static void field_put_text(char *rec, int off, int len, const char *s)
{
    size_t n = strlen(s);
    if (n > (size_t)len) n = (size_t)len;
    memset(rec + off, ' ', len);
    memcpy(rec + off, s, n);
}

static void field_put_ulong(char *rec, int off, int len, unsigned long v)
{
    char tmp[24];
    snprintf(tmp, sizeof(tmp), "%lu", v);
    size_t n = strlen(tmp);
    memset(rec + off, ' ', len);
    if (n > (size_t)len) n = (size_t)len;
    /* right-justified */
    memcpy(rec + off + (len - (int)n), tmp, n);
}

static unsigned long field_get_ulong(const char *rec, int off, int len)
{
    char tmp[24];
    int n = len < (int)sizeof(tmp) - 1 ? len : (int)sizeof(tmp) - 1;
    memcpy(tmp, rec + off, n);
    tmp[n] = '\0';
    return strtoul(tmp, NULL, 10);
}

/* this node's SCSNODE, via $GETSYI (falls back to "OVMX" the same way
 * sys$getsyi's own SYI$_SCSNODE answer does when SYSGEN is unconfigured --
 * see getsyi_impl, src/libvms/syssvc/sys_misc.c). */
static void get_node_name(char *out, size_t outlen)
{
    char node[32] = {0};
    uint16_t retlen = 0;
    struct item_list_3 itms[] = {
        { (uint16_t)sizeof(node) - 1, SYI$_SCSNODE, node, &retlen },
        { 0, 0, NULL, NULL }
    };
    uint32_t st = sys$getsyiw(EFN$C_ENF, NULL, NULL, itms, NULL, NULL, 0);
    if (!(st & 1) || retlen == 0) {
        strncpy(out, "OVMX", outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    size_t n = retlen < outlen - 1 ? retlen : outlen - 1;
    memcpy(out, node, n);
    out[n] = '\0';
}

static unsigned long get_own_pid(void)
{
    uint32_t pid = 0;
    uint16_t retlen = 0;
    ILE3 itms[] = {
        { 4, JPI$_PID, &pid, &retlen },
        ILE3_TERMINATOR
    };
    uint32_t st = sys$getjpiw(EFN$C_ENF, NULL, NULL, itms, NULL, NULL, 0);
    return (st & 1) ? (unsigned long)pid : 0;
}

static void get_time_string(char *out, size_t outlen)
{
    char buf[32] = {0};
    uint16_t len = 0;
    struct dsc$descriptor_s d = { (uint16_t)(sizeof(buf) - 1), DSC$K_DTYPE_T,
                                  DSC$K_CLASS_S, buf };
    uint32_t st = sys$asctim(&len, &d, NULL, 0);
    if (!(st & 1) || len == 0) {
        strncpy(out, "(unknown time)", outlen - 1);
        out[outlen - 1] = '\0';
        return;
    }
    size_t n = len < outlen - 1 ? len : outlen - 1;
    memcpy(out, buf, n);
    out[n] = '\0';
}

/* take EVAC$WORKLOAD. `standby` requests NL first, converted to EX once
 * granted -- a process that starts in standby queues behind the current
 * EX holder and is granted only on takeover. Direct (non-standby) requests
 * EX outright; $ENQW blocks (queued, no LCK$M_NOQUEUE) until granted either
 * way, so a second instance legitimately waits for the first to release. */
static uint32_t take_workload_lock(int standby, uint32_t *lkid_out)
{
    $DESCRIPTOR(resnam, "EVAC$WORKLOAD");
    struct {
        uint16_t lksb$w_status;
        uint16_t lksb$w_reserved;
        uint32_t lksb$l_lkid;
        char     lksb$b_valblk[16];
    } lksb = {0};
    uint32_t st;

    if (standby) {
        st = sys$enqw(EFN$C_ENF, LCK$K_NLMODE, &lksb, 0, &resnam, 0,
                      NULL, 0, NULL, 0, 0, 0);
        if (!(st & 1))
            return st;
        st = sys$enqw(EFN$C_ENF, LCK$K_EXMODE, &lksb, LCK$M_CONVERT, NULL,
                      0, NULL, 0, NULL, 0, 0, 0);
    } else {
        st = sys$enqw(EFN$C_ENF, LCK$K_EXMODE, &lksb, 0, &resnam, 0,
                      NULL, 0, NULL, 0, 0, 0);
    }
    if (st & 1)
        *lkid_out = lksb.lksb$l_lkid;
    return st;
}

/* The last record's seq, 0 if the file is new/empty -- a sequential scan to
 * EOF (these files are small; no indexed access is needed). Leaves the RAB
 * positioned at EOF, so the caller's first $PUT appends. */
static unsigned long read_last_seq(struct RAB *rab)
{
    char rec[RECLEN];
    unsigned long last_seq = 0;
    uint32_t st;

    for (;;) {
        rab->rab$l_ubf = rec;
        rab->rab$w_usz = RECLEN;
        st = sys$get(rab, 0, 0);
        if (st == RMS$_EOF)
            break;
        if (!(st & 1))
            break;
        last_seq = field_get_ulong(rec, FLD_SEQ_OFF, FLD_SEQ_LEN);
    }
    return last_seq;
}

int main(int argc, char **argv)
{
    int standby = 0;
    long count = -1; /* -1 = forever */
    int i;

    for (i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "standby") == 0)
            standby = 1;
        else
            count = strtol(argv[i], NULL, 10);
    }

    uint32_t lkid = 0;
    uint32_t st = take_workload_lock(standby, &lkid);
    if (!(st & 1)) {
        fprintf(stderr, "EVACWL: $ENQW failed, status %u\n", st);
        return 1;
    }

    static struct FAB fab;
    static struct RAB rab;
    static char filename[] = "EVAC$DATA:EVAC.DAT";

    fab = cc$rms_fab;
    fab.fab$l_fna = filename;
    fab.fab$b_fns = (unsigned char)strlen(filename);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_fac = FAB$M_GET | FAB$M_PUT;
    fab.fab$w_mrs = RECLEN;
    fab.fab$b_rat = FAB$M_CR;
    fab.fab$b_rfm = FAB$C_FIX;
    fab.fab$b_shr = FAB$M_GET | FAB$M_PUT;

    st = sys$open(&fab, 0, 0);
    if (!(st & 1)) {
        /* Not found (or any other open failure): create it fresh. */
        st = sys$create(&fab, 0, 0);
        if (!(st & 1)) {
            fprintf(stderr, "EVACWL: cannot open or create EVAC.DAT, status %u\n", st);
            return 1;
        }
    }

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$b_rac = RAB$C_SEQ;
    rab.rab$l_rop = RAB$M_WBH;
    rab.rab$l_rbf = NULL;
    rab.rab$w_rsz = RECLEN;

    st = sys$connect(&rab, 0, 0);
    if (!(st & 1)) {
        fprintf(stderr, "EVACWL: $CONNECT failed, status %u\n", st);
        return 1;
    }

    unsigned long seq = read_last_seq(&rab) + 1;

    char node[32];
    get_node_name(node, sizeof(node));
    unsigned long pid = get_own_pid();

    printf("EVACWL: %s took EVAC$WORKLOAD EX at seq %lu\n", node, seq);
    fflush(stdout);

    for (;;) {
        char rec[RECLEN];
        char timestr[40];

        get_time_string(timestr, sizeof(timestr));

        field_put_text(rec, FLD_NODE_OFF, FLD_NODE_LEN, node);
        field_put_ulong(rec, FLD_PID_OFF, FLD_PID_LEN, pid);
        field_put_ulong(rec, FLD_SEQ_OFF, FLD_SEQ_LEN, seq);
        field_put_text(rec, FLD_TIME_OFF, FLD_TIME_LEN, timestr);

        rab.rab$l_rbf = rec;
        rab.rab$w_rsz = RECLEN;
        st = sys$put(&rab, 0, 0);
        if (!(st & 1)) {
            fprintf(stderr, "EVACWL: $PUT failed, status %u\n", st);
            break;
        }

        st = sys$flush(&rab, 0, 0);
        if (!(st & 1)) {
            fprintf(stderr, "EVACWL: $FLUSH failed, status %u\n", st);
            break;
        }

        seq++;

        if (count >= 0) {
            count--;
            if (count <= 0)
                break;
        }

        {
            $DESCRIPTOR(delay_d, "0 00:00:01.00");
            GENERIC_64 delay;
            sys$bintim(&delay_d, &delay);
            sys$schdwk(0, 0, (uint64_t *)&delay, 0);
            sys$hiber();
        }
    }

    sys$disconnect(&rab, 0, 0);
    sys$close(&fab, 0, 0);
    sys$deq(lkid, NULL, 0, 0);
    return 0;
}
