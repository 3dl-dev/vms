/*
 * test_chkpro.c - $CHKPRO decisions against the OpenVMS VAX V7.3 oracle (vms-d404).
 *
 * Every case is one of docs/oracle/semantics/chkpro/vax73.txt, with the subject
 * given in the item list (CHP$_UIC, CHP$_PRIV, CHP$_RIGHTS) so no executive is
 * needed: "UP." cases are the DEFAULT account's profile ([200,200], no privilege
 * that matters), "ME." cases the probe's SYSTEM process ([1,4]) with the
 * privileges it held at that point. The booted runtime runs the same probe
 * through the semantic-oracle gate; this pins the decision on every build.
 */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "starlet.h"
#include "ssdef.h"
#include "chpdef.h"
#include "armdef.h"
#include "iledef.h"

#define RD ARM$M_READ
#define WR ARM$M_WRITE
#define EX ARM$M_EXECUTE
#define DL ARM$M_DELETE
#define CT ARM$M_CONTROL
#define UIC(g, m) ((uint32_t)(g) << 16 | (uint32_t)(m))
#define DEF  UIC(0200, 0200)
#define SYS  UIC(1, 4)
#define OTH  UIC(0300, 1)
#define GRP  UIC(0200, 1)
#define P_SYSPRV  (1ULL << 28)
#define P_BYPASS  (1ULL << 29)
#define P_GRPPRV  (1ULL << 34)
#define P_READALL (1ULL << 35)
#define ALLPRV    0xFFFFFFFFFFFFFFFFULL

enum { NOACL, DEFREAD, DEFNONE, GRPNONE, WILDRW, OTHER, DEFLT, CTRL, SYSNONE };
static const struct { uint32_t id, access, flags; } aces[] = {
    [DEFREAD] = { DEF, RD, 0 },               [DEFNONE] = { DEF, 0, 0 },
    [GRPNONE] = { UIC(0200, 0xFFFF), 0, 0 }, [WILDRW] = { 0x3FFFFFFF, RD | WR, 0 },
    [OTHER] = { OTH, 0, 0 },                  [DEFLT] = { DEF, RD, 0x0100 },
    [CTRL] = { DEF, CT, 0 },                  [SYSNONE] = { SYS, 0, 0 },
};

struct tcase {
    const char *id;
    uint32_t uic; uint64_t privs;
    int acl; uint32_t acc, own, prot, flags;
    uint32_t expect;
};

#define N SS$_NORMAL
#define X SS$_NOPRIV
static const struct tcase cases[] = {
    { "UP.WORLD.NONE.READ",   DEF, 0, NOACL, RD, SYS, 0xFF00, 0, X },
    { "UP.WORLD.ALL.READ",    DEF, 0, NOACL, RD, SYS, 0x0000, 0, N },
    { "UP.WORLD.R.READ",      DEF, 0, NOACL, RD, SYS, 0xEF00, 0, N },
    { "UP.WORLD.R.WRITE",     DEF, 0, NOACL, WR, SYS, 0xEF00, 0, X },
    { "UP.WORLD.R.RW",        DEF, 0, NOACL, RD | WR, SYS, 0xEF00, 0, X },
    { "UP.WORLD.E.EXECUTE",   DEF, 0, NOACL, EX, SYS, 0xBF00, 0, N },
    { "UP.WORLD.D.DELETE",    DEF, 0, NOACL, DL, SYS, 0x7F00, 0, N },
    { "UP.GROUP.R.READ",      DEF, 0, NOACL, RD, GRP, 0xFE00, 0, N },
    { "UP.OWNER.RWED.READ",   DEF, 0, NOACL, RD, DEF, 0xFF0F, 0, N },
    { "UP.OWNER.NONE.READ",   DEF, 0, NOACL, RD, DEF, 0x00F0, 0, N },
    { "UP.CONTROL.OWNER",     DEF, 0, NOACL, CT, DEF, 0xFFFF, 0, N },
    { "UP.CONTROL.WORLD",     DEF, 0, NOACL, CT, SYS, 0x0000, 0, N },
    { "UP.CONTROL.GROUP",     DEF, 0, NOACL, CT, GRP, 0x0000, 0, N },
    { "UP.CONTROL.WORLD.NONE", DEF, 0, NOACL, CT, SYS, 0xFF00, 0, N },
    { "UP.CONTROL.WORLD.R",   DEF, 0, NOACL, CT, SYS, 0xEF00, 0, N },
    { "UP.CONTROL.WORLD.W",   DEF, 0, NOACL, CT, SYS, 0xDF00, 0, N },
    { "UP.CONTROL.WORLD.RWE", DEF, 0, NOACL, CT, SYS, 0x8F00, 0, N },
    { "UP.CONTROL.WORLD.D",   DEF, 0, NOACL, CT, SYS, 0x7F00, 0, N },
    { "UP.CONTROL.GROUP.NONE", DEF, 0, NOACL, CT, GRP, 0x0F00, 0, N },
    { "UP.CONTROL.GROUP.W",   DEF, 0, NOACL, CT, GRP, 0xFD00, 0, N },
    { "UP.CONTROL.OWNER.NONE", DEF, 0, NOACL, CT, DEF, 0x00F0, 0, N },
    { "UP.CONTROL.RW",        DEF, 0, NOACL, CT | RD, SYS, 0xEF00, 0, N },
    { "UP.ZERO.ACCESS",       DEF, 0, NOACL, 0, SYS, 0xFFFF, 0, N },
    { "UP.OBSERVE.READ",      DEF, 0, NOACL, RD, SYS, 0xEF00, CHP$M_OBSERVE, N },
    { "UP.ALTER.WRITE",       DEF, 0, NOACL, WR, SYS, 0xEF00, CHP$M_ALTER, X },
    { "UP.ACL.GRANT",         DEF, 0, DEFREAD, RD, SYS, 0xFF00, 0, N },
    { "UP.ACL.GRANT.WRITE",   DEF, 0, DEFREAD, WR, SYS, 0xFF00, 0, X },
    { "UP.ACL.DENY.WORLD",    DEF, 0, DEFNONE, RD, SYS, 0x0000, 0, X },
    { "UP.ACL.DENY.GROUP",    DEF, 0, GRPNONE, RD, GRP, 0x0000, 0, X },
    { "UP.ACL.DENY.OWNER",    DEF, 0, DEFNONE, RD, DEF, 0xFF0F, 0, N },
    { "UP.ACL.WILD",          DEF, 0, WILDRW, RD | WR, SYS, 0xFF00, 0, N },
    { "UP.ACL.NOMATCH",       DEF, 0, OTHER, RD, SYS, 0xEF00, 0, N },
    { "UP.ACL.DEFAULTACE",    DEF, 0, DEFLT, RD, SYS, 0xFF00, 0, X },
    { "UP.ACL.CONTROL",       DEF, 0, CTRL, CT, SYS, 0xFF00, 0, N },
    { "UP.ACL.CONTROL.DENY",  DEF, 0, DEFNONE, CT, SYS, 0x0000, 0, X },
    { "UP.ACL.CONTROL.READACE", DEF, 0, DEFREAD, CT, SYS, 0x0000, 0, X },
    { "UP.ACL.PARTIAL",       DEF, 0, DEFREAD, RD | WR, SYS, 0x0000, 0, X },
    { "UP.ACL.PARTIAL.OWNER", DEF, 0, DEFREAD, RD | WR, DEF, 0xFF0F, 0, N },
    { "ME.ALLPRV.NONE",       SYS, ALLPRV, NOACL, RD | WR, OTH, 0xFFFF, 0, N },
    { "ME.ALLPRV.ACLDENY",    SYS, ALLPRV, SYSNONE, RD, OTH, 0xFFFF, 0, N },
    { "ME.NOPRV.NONE",        SYS, 0, NOACL, RD, OTH, 0xFFFF, 0, X },
    { "ME.NOPRV.SYSTEM",      SYS, 0, NOACL, RD | WR, OTH, 0xFFF0, 0, N },
    { "ME.NOPRV.SYSTEM.ACLDENY", SYS, 0, SYSNONE, RD, OTH, 0xFFF0, 0, N },
    { "ME.NOPRV.CONTROL.SYSTEM", SYS, 0, NOACL, CT, OTH, 0xFFF0, 0, N },
    { "ME.NOPRV.OWNER",       SYS, 0, NOACL, RD, SYS, 0xFF0F, 0, N },
    { "ME.UIC.DEFAULT.WORLDNONE", DEF, 0, NOACL, RD, SYS, 0xFF00, 0, X },
    { "ME.UIC.DEFAULT.OWNER", DEF, 0, NOACL, RD, DEF, 0xFF0F, 0, N },
    { "ME.PRIV.ZERO",         SYS, 0, NOACL, RD, OTH, 0xFFF0, 0, N },
    { "ME.PRIV.BYPASS",       SYS, P_BYPASS, NOACL, RD | WR, OTH, 0xFFFF, 0, N },
    { "ME.PRIV.SYSPRV",       SYS, P_SYSPRV, NOACL, RD, OTH, 0xFFF0, 0, N },
    { "ME.PRIV.SYSPRV.NOSYSFIELD", SYS, P_SYSPRV, NOACL, RD, OTH, 0xFFFF, 0, X },
    { "ME.PRIV.READALL.READ", SYS, P_READALL, NOACL, RD, OTH, 0xFFFF, 0, X },
    { "ME.PRIV.READALL.WRITE", SYS, P_READALL, NOACL, WR, OTH, 0xFFFF, 0, X },
    { "ME.PRIV.READALL.OBSERVE", SYS, P_READALL, NOACL, RD, OTH, 0xFFFF, CHP$M_OBSERVE, X },
    { "ME.PRIV.READALL.USEREADALL", SYS, P_READALL, NOACL, RD, OTH, 0xFFFF, CHP$M_USEREADALL, N },
    { "ME.PRIV.READALL.USEREADALL.WRITE", SYS, P_READALL, NOACL, WR, OTH, 0xFFFF, CHP$M_USEREADALL, N },
    { "ME.PRIV.READALL.USEREADALL.EXECUTE", SYS, P_READALL, NOACL, EX, OTH, 0xFFFF, CHP$M_USEREADALL, N },
    { "ME.PRIV.READALL.USEREADALL.ACLDENY", SYS, P_READALL, SYSNONE, RD, OTH, 0xFFFF, CHP$M_USEREADALL, X },
    { "ME.PRIV.GRPPRV.OWNGROUP", DEF, P_GRPPRV, NOACL, RD, GRP, 0xFFF0, 0, N },
    { "ME.PRIV.GRPPRV.OTHERGROUP", DEF, P_GRPPRV, NOACL, RD, OTH, 0xFFF0, 0, X },
    { "ME.UICPRIV.ZERO.OWNGROUP", DEF, 0, NOACL, RD, GRP, 0xFFF0, 0, X },
    { "UP.AFTER.SETPRV",      DEF, 0, NOACL, RD, SYS, 0xEF00, 0, N },
};

static uint32_t run(const struct tcase *c)
{
    uint32_t acc = c->acc, own = c->own, prot = c->prot, flg = c->flags, uic = c->uic;
    uint32_t priv[2] = { (uint32_t)c->privs, (uint32_t)(c->privs >> 32) };
    uint8_t ace[12];
    ILE3 it[9];
    int n = 0;

    memset(it, 0, sizeof(it));
#define ITEM(l, c_, b) (it[n].ile3$w_length = (l), it[n].ile3$w_code = (c_), it[n].ile3$ps_bufaddr = (b), n++)
    ITEM(4, CHP$_ACCESS, &acc);
    ITEM(4, CHP$_FLAGS, &flg);
    ITEM(4, CHP$_OWNER, &own);
    ITEM(4, CHP$_PROT, &prot);
    ITEM(4, CHP$_UIC, &uic);
    ITEM(8, CHP$_PRIV, priv);
    ITEM(0, CHP$_RIGHTS, NULL);
    if (c->acl != NOACL) {
        uint32_t id = aces[c->acl].id, a = aces[c->acl].access, f = aces[c->acl].flags;
        ace[0] = 12; ace[1] = 1; ace[2] = (uint8_t)f; ace[3] = (uint8_t)(f >> 8);
        memcpy(ace + 4, &a, 4);
        memcpy(ace + 8, &id, 4);
        ITEM(12, CHP$_ACL, ace);
    }
#undef ITEM
    return sys$chkpro(it, NULL, NULL);
}

int main(void)
{
    int fail = 0, pass = 0;
    size_t i;

    printf("=== test_chkpro: $CHKPRO against docs/oracle/semantics/chkpro/vax73.txt ===\n");
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint32_t st = run(&cases[i]);
        if (st == cases[i].expect) {
            pass++;
        } else {
            printf("  FAIL: %s st=%08X, VAX V7.3 says %08X\n", cases[i].id, st, cases[i].expect);
            fail++;
        }
    }
    {
        ILE3 empty[1];
        memset(empty, 0, sizeof(empty));
        if (sys$chkpro(NULL, NULL, NULL) == SS$_ACCVIO) pass++;
        else { printf("  FAIL: CHKPRO.NO.ITEMS is not SS$_ACCVIO\n"); fail++; }
        (void)empty;
    }
    printf("=== test_chkpro: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
