/*
 * RMHOLD.C - the OVMX side of the controlled lock-tree remaster arms
 * (rd vms-c2b / rd vms-6ad1 RA7).
 *
 * THIS IS THE SAME CONTRACT tests/lab/captures/vms-50d-ra5-14cells-20261013/
 * RMHOLD.MAR runs on a real OpenVMS VAX V7.3 node (that oracle has MACRO +
 * LINK, no C compiler). One argument string, the same four leading digits
 * and the same resource naming, so an arm can put the SAME holder on a real
 * VAX and on an OVMX node and compare what each one's lock database says.
 * The two sources are kept from drifting by repeating the contract here
 * verbatim, exactly as EVACWL.C/EVACWL.MAR do for the evacuation workload.
 *
 *   RMHOLD <r><s><n><w><name>
 *
 *     r     root lock mode digit (0 NL, 1 CR, 2 CW, 3 PR, 4 PW, 5 EX)
 *     s     sub-lock mode digit, same encoding
 *     n     number of sub-locks (0-9), named S0 .. S(n-1), each $ENQ'd
 *           with PARID = the ROOT's lock id, so they are real children
 *     w     W: take the root EX first and write the known 16-byte value
 *              block "RMLVB-0123456789" on the down-conversion to r (only
 *              the FIRST holder of a resource can establish its value
 *              block that way)
 *           A: as '-', then 300 rounds of converting every sub-lock to NL
 *              and back to s -- lock ACTIVITY, which is what drives
 *              activity-based remastering
 *           B: as A, with 3000 rounds
 *           any other character (the arms write '-'): request the root
 *              directly in mode r with LCK$M_VALBLK
 *     name  root resource name, taken with LCK$M_SYSTEM
 *
 * Then it reports every lock it holds from the EXECUTIVE'S OWN ANSWER --
 * one $GETLKIW per lock id, printing the lock id, the resource name, the
 * granted and requested modes, the queue state and the parent lock id that
 * THIS node's lock database holds (INV-6: nothing here is echoed back from
 * the request; a field the executive does not answer is printed as
 * "(not answered)") -- and hibernates holding everything, so the arm can
 * read the other nodes' SDA while the locks are still held.
 *
 * WHY A HOLDER PROGRAM ON OVMX AT ALL. OVMX does not choose to become a
 * lock tree's new master: the remaster op-0x0e arrives addressed to it, and
 * every controlled arm measured so far (RA2/RA4/RA5/RA8) shows the new
 * master is a node that HOLDS a lock in the tree. So an OVMX node can only
 * be handed a tree -- and can only be the subject of RA7 -- if it first
 * holds a lock on a resource a real VAX masters, which is what this program
 * makes it do. EVACWL cannot: its resource name and its EX mode are fixed,
 * and two EX requests never coexist.
 *
 * Built by the same cc -> LINK.EXE toolchain as PARTS.EXE/RC3.EXE (see
 * mk_rmhold_native.sh): a VMS-native ET_DYN image whose PT_INTERP is
 * IMGACT.EXE, activated over the ACP exactly as any customer program is.
 * It calls the PUBLIC sys$enqw / sys$getlkiw / sys$hiber entry points, the
 * same ones src/libvms exports to any OVMX program.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <ssdef.h>
#include <descrip.h>
#include <lckdef.h>
#include <lkidef.h>
#include <iledef.h>
#include <efndef.h>
#include <starlet.h>

#define RMHOLD_MAX_SUB    9
#define RMHOLD_NAME_MAX   32
#define VALBLK_LEN        16

/* The value-block pattern RMHOLD.MAR writes, byte for byte. An arm reads it
 * back on another node's SDA to prove the block crossed the wire. */
static const char RMHOLD_PAT[VALBLK_LEN] = "RMLVB-0123456789";

/* A lock status block, laid out as $ENQ fills it (the shape every OVMX
 * program uses -- tests/qemu/test_syssvc_getlki.c's struct lksb_caller). */
struct rmhold_lksb {
    uint16_t lksb$w_status;
    uint16_t lksb$w_reserved;
    uint32_t lksb$l_lkid;
    char     lksb$b_valblk[VALBLK_LEN];
};

struct rmhold_args {
    uint32_t root_mode;
    uint32_t sub_mode;
    int      nsub;
    char     w;
    char     name[RMHOLD_NAME_MAX + 1];
};

struct rmhold_state {
    struct rmhold_lksb root;
    struct rmhold_lksb sub[RMHOLD_MAX_SUB];
    char               sub_name[RMHOLD_MAX_SUB][3];   /* "S0" .. "S8" */
};

static const char *const MODE_NAME[6] = { "NL", "CR", "CW", "PR", "PW", "EX" };

static const char *mode_name(unsigned m)
{
    return m < 6 ? MODE_NAME[m] : "??";
}

/* "<r><s><n><w><name>" -- the same four leading characters RMHOLD.MAR reads
 * out of its foreign command line, and the same rest-is-the-name rule. */
static int parse_spec(const char *spec, struct rmhold_args *a)
{
    size_t len = spec ? strlen(spec) : 0;
    int i;

    if (len < 5) return 0;
    for (i = 0; i < 3; i++)
        if (spec[i] < '0' || spec[i] > '9') return 0;
    a->root_mode = (uint32_t)(spec[0] - '0');
    a->sub_mode  = (uint32_t)(spec[1] - '0');
    a->nsub      = spec[2] - '0';
    a->w         = spec[3];
    if (a->root_mode > 5 || a->sub_mode > 5) return 0;
    if (a->nsub > RMHOLD_MAX_SUB) return 0;
    if (len - 4 > RMHOLD_NAME_MAX) return 0;
    memcpy(a->name, spec + 4, len - 4);
    a->name[len - 4] = '\0';
    return 1;
}

static void init_sub_names(struct rmhold_args *a, struct rmhold_state *st)
{
    int i;
    for (i = 0; i < a->nsub; i++) {
        st->sub_name[i][0] = 'S';
        st->sub_name[i][1] = (char)('0' + i);
        st->sub_name[i][2] = '\0';
    }
}

static void descriptor_for(struct dsc$descriptor_s *d, const char *s)
{
    d->dsc$w_length  = (uint16_t)strlen(s);
    d->dsc$b_dtype   = DSC$K_DTYPE_T;
    d->dsc$b_class   = DSC$K_CLASS_S;
    d->dsc$a_pointer = (char *)s;
}

/* The root, mode r, with LCK$M_SYSTEM|LCK$M_VALBLK -- RMHOLD.MAR's non-'W'
 * branch (label 60$'s fall-through). */
static uint32_t enq_root_direct(struct rmhold_args *a, struct rmhold_state *st)
{
    struct dsc$descriptor_s resnam;
    descriptor_for(&resnam, a->name);
    return sys$enqw(EFN$C_ENF, a->root_mode, &st->root,
                    LCK$M_SYSTEM | LCK$M_VALBLK, &resnam, 0,
                    NULL, 0, NULL, 0, 0, 0);
}

/* The 'W' branch: EX first, then write the pattern into the value block on
 * the down-conversion to mode r. RMHOLD.MAR labels 60$/70$/71$. */
static uint32_t enq_root_write_valblk(struct rmhold_args *a,
                                      struct rmhold_state *st)
{
    struct dsc$descriptor_s resnam;
    uint32_t status;

    descriptor_for(&resnam, a->name);
    status = sys$enqw(EFN$C_ENF, LCK$K_EXMODE, &st->root,
                      LCK$M_SYSTEM | LCK$M_VALBLK, &resnam, 0,
                      NULL, 0, NULL, 0, 0, 0);
    if (!(status & 1)) return status;

    memcpy(st->root.lksb$b_valblk, RMHOLD_PAT, VALBLK_LEN);
    return sys$enqw(EFN$C_ENF, a->root_mode, &st->root,
                    LCK$M_SYSTEM | LCK$M_VALBLK | LCK$M_CONVERT, NULL, 0,
                    NULL, 0, NULL, 0, 0, 0);
}

static uint32_t enq_root(struct rmhold_args *a, struct rmhold_state *st)
{
    return a->w == 'W' ? enq_root_write_valblk(a, st)
                       : enq_root_direct(a, st);
}

/* Sub-lock i: mode s, PARID = the root's lock id (RMHOLD.MAR label 10$). */
static uint32_t enq_sub(struct rmhold_args *a, struct rmhold_state *st, int i)
{
    struct dsc$descriptor_s resnam;
    descriptor_for(&resnam, st->sub_name[i]);
    return sys$enqw(EFN$C_ENF, a->sub_mode, &st->sub[i], 0, &resnam,
                    st->root.lksb$l_lkid, NULL, 0, NULL, 0, 0, 0);
}

static uint32_t enq_all_subs(struct rmhold_args *a, struct rmhold_state *st)
{
    int i;
    for (i = 0; i < a->nsub; i++) {
        uint32_t status = enq_sub(a, st, i);
        if (!(status & 1)) {
            printf("RMHOLD: $ENQW %s failed, status %%X%08X\n",
                   st->sub_name[i], (unsigned)status);
            return status;
        }
    }
    return SS$_NORMAL;
}

/* One activity round: every sub-lock down to NL and back to s. RMHOLD.MAR
 * label 54$ -- the conversions are what a remaster-by-activity counts. */
static uint32_t activity_round(struct rmhold_args *a, struct rmhold_state *st)
{
    int i;
    for (i = 0; i < a->nsub; i++) {
        uint32_t status = sys$enqw(EFN$C_ENF, LCK$K_NLMODE, &st->sub[i],
                                   LCK$M_CONVERT, NULL, 0,
                                   NULL, 0, NULL, 0, 0, 0);
        if (!(status & 1)) return status;
        status = sys$enqw(EFN$C_ENF, a->sub_mode, &st->sub[i],
                          LCK$M_CONVERT, NULL, 0, NULL, 0, NULL, 0, 0, 0);
        if (!(status & 1)) return status;
    }
    return SS$_NORMAL;
}

static int activity_rounds_for(char w)
{
    if (w == 'A') return 300;
    if (w == 'B') return 3000;
    return 0;
}

static uint32_t run_activity(struct rmhold_args *a, struct rmhold_state *st)
{
    int rounds = activity_rounds_for(a->w), i;

    if (rounds == 0 || a->nsub == 0) return SS$_NORMAL;
    printf("RMHOLD: %d activity rounds over %d sub-locks\n", rounds, a->nsub);
    fflush(stdout);
    for (i = 0; i < rounds; i++) {
        uint32_t status = activity_round(a, st);
        if (!(status & 1)) {
            printf("RMHOLD: activity round %d failed, status %%X%08X\n",
                   i, (unsigned)status);
            return status;
        }
    }
    printf("RMHOLD: activity complete\n");
    fflush(stdout);
    return SS$_NORMAL;
}

/* Everything printed below is the EXECUTIVE'S answer for this lock id, read
 * back with $GETLKIW -- never the value this program asked for. A lock the
 * executive does not answer for is reported as such. */
struct lki_readout {
    uint32_t lockid;
    uint32_t parent;
    uint32_t pid;
    uint32_t grantcount;
    char     resnam[RMHOLD_NAME_MAX + 1];
    uint16_t resnam_len;
    uint8_t  state[3];
    uint32_t status;
};

static uint32_t getlki_read(uint32_t lkid, struct lki_readout *r)
{
    uint16_t len_res = 0, len_state = 0, len_id = 0,
             len_par = 0, len_pid = 0, len_cnt = 0;
    ILE3 items[] = {
        { 4,                   LKI$_LOCKID,     &r->lockid,     &len_id    },
        { 4,                   LKI$_PARENT,     &r->parent,     &len_par   },
        { 4,                   LKI$_PID,        &r->pid,        &len_pid   },
        { 4,                   LKI$_GRANTCOUNT, &r->grantcount, &len_cnt   },
        { 3,                   LKI$_STATE,      r->state,       &len_state },
        { RMHOLD_NAME_MAX,     LKI$_RESNAM,     r->resnam,      &len_res   },
        ILE3_TERMINATOR
    };

    memset(r, 0, sizeof(*r));
    r->status = sys$getlkiw(EFN$C_ENF, &lkid, items, NULL, NULL, 0, NULL);
    r->resnam_len = len_res <= RMHOLD_NAME_MAX ? len_res : RMHOLD_NAME_MAX;
    r->resnam[r->resnam_len] = '\0';
    if (len_state != 3) memset(r->state, 0xff, sizeof(r->state));
    return r->status;
}

static void report_lock(const char *label, uint32_t lkid)
{
    struct lki_readout r;

    if (!(getlki_read(lkid, &r) & 1)) {
        printf("RMHOLD: %-4s lkid %08X -- $GETLKIW failed, status %%X%08X "
               "(not answered)\n", label, (unsigned)lkid, (unsigned)r.status);
        return;
    }
    printf("RMHOLD: %-4s lkid %08X res '%s' gr %s rq %s queue %u "
           "parent %08X pid %08X grantcount %u\n",
           label, (unsigned)r.lockid, r.resnam,
           mode_name(r.state[LKI$B_STATE_GRMODE]),
           mode_name(r.state[LKI$B_STATE_RQMODE]),
           (unsigned)r.state[LKI$B_STATE_QUEUE],
           (unsigned)r.parent, (unsigned)r.pid, (unsigned)r.grantcount);
}

static void report_all(struct rmhold_args *a, struct rmhold_state *st)
{
    int i;
    report_lock("root", st->root.lksb$l_lkid);
    for (i = 0; i < a->nsub; i++)
        report_lock(st->sub_name[i], st->sub[i].lksb$l_lkid);
    fflush(stdout);
}

static void usage(void)
{
    printf("RMHOLD: usage: RMHOLD <r><s><n><w><name>  "
           "(r,s = mode digit 0..5; n = sub-locks 0..9; "
           "w = W|A|B|-; name = root resource)\n");
    fflush(stdout);
}

int main(int argc, char **argv)
{
    struct rmhold_args a;
    struct rmhold_state st;
    uint32_t status;

    memset(&a, 0, sizeof(a));
    memset(&st, 0, sizeof(st));

    if (argc < 2 || !parse_spec(argv[1], &a)) {
        usage();
        return SS$_BADPARAM;
    }
    init_sub_names(&a, &st);

    printf("RMHOLD: root '%s' %s, %d sub-lock(s) %s, w '%c'\n",
           a.name, mode_name(a.root_mode), a.nsub, mode_name(a.sub_mode), a.w);
    fflush(stdout);

    status = enq_root(&a, &st);
    if (!(status & 1)) {
        printf("RMHOLD: $ENQW root '%s' failed, status %%X%08X\n",
               a.name, (unsigned)status);
        return status;
    }
    status = enq_all_subs(&a, &st);
    if (!(status & 1)) return status;

    report_all(&a, &st);

    status = run_activity(&a, &st);
    if (!(status & 1)) return status;

    printf("RMHOLD: holding -- hibernating\n");
    fflush(stdout);
    for (;;)
        sys$hiber();
    /* NOTREACHED */
}
