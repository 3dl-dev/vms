/*
 * test_dnet_nodespec.c - node-filespec splitter unit tests (rd vms-ea8/vms-6a4;
 * the FAL/DAP COPY engine foundation is the merged vms-8c2).
 *
 * Exercises dnet_nodespec_parse: node-name extraction, the space-separated
 * username/password/account access-control split, the "" -> " escape, the
 * top-level "::" detection (a "::" inside the access string must NOT be taken
 * as the node separator), the no-node local-path case, and the INV-6 refusals
 * (unterminated access string, over-long field, empty node/file, a stray token
 * after the access string, control bytes). Pure logic; no socket / privilege.
 */
#include <stdio.h>
#include <string.h>

#include "dnet_nodespec.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}
#define EQ(a, b) (strcmp((a), (b)) == 0)

int main(void)
{
    struct dnet_nodespec ns;
    int rc;

    printf("test_dnet_nodespec: DECnet node-filespec splitter\n");

    /* --- 1. bare node, no access string --- */
    rc = dnet_nodespec_parse("OVMXR3::DISK$USER:[SMITH]LOGIN.COM", &ns);
    check(rc == DNET_CTERM_OK, "NODE::spec parses");
    check(EQ(ns.node, "OVMXR3"), "  node = OVMXR3");
    check(EQ(ns.filespec, "DISK$USER:[SMITH]LOGIN.COM"), "  filespec stripped of node");
    check(ns.has_access == 0, "  has_access == 0");
    check(ns.username[0] == '\0' && ns.password[0] == '\0' && ns.account[0] == '\0',
          "  creds all empty");

    /* --- 2. full access-control string: user password account --- */
    rc = dnet_nodespec_parse("VAX1\"SYSTEM SECRET FIELDTEST\"::SYS$LOGIN:FOO.TXT", &ns);
    check(rc == DNET_CTERM_OK, "NODE\"user pw acct\"::spec parses");
    check(EQ(ns.node, "VAX1"), "  node = VAX1");
    check(ns.has_access == 1, "  has_access == 1");
    check(EQ(ns.username, "SYSTEM"), "  username = SYSTEM");
    check(EQ(ns.password, "SECRET"), "  password = SECRET");
    check(EQ(ns.account, "FIELDTEST"), "  account = FIELDTEST");
    check(EQ(ns.filespec, "SYS$LOGIN:FOO.TXT"), "  filespec = SYS$LOGIN:FOO.TXT");

    /* --- 3. two-field access string (user password, no account) --- */
    rc = dnet_nodespec_parse("MARS\"GUEST OPEN\"::[000000]DATA.DAT", &ns);
    check(rc == DNET_CTERM_OK, "NODE\"user pw\"::spec parses");
    check(EQ(ns.username, "GUEST") && EQ(ns.password, "OPEN"), "  user/pw split");
    check(ns.account[0] == '\0', "  account empty");

    /* --- 4. single-field access string (username only) --- */
    rc = dnet_nodespec_parse("MARS\"GUEST\"::A.B", &ns);
    check(rc == DNET_CTERM_OK && EQ(ns.username, "GUEST") &&
          ns.password[0] == '\0' && ns.account[0] == '\0',
          "NODE\"user\"::spec -> username only");

    /* --- 5. node ADDRESS form a.n --- */
    rc = dnet_nodespec_parse("1.42::DKA0:[DIR]F.TXT", &ns);
    check(rc == DNET_CTERM_OK && EQ(ns.node, "1.42"), "node address 1.42 preserved");

    /* --- 6. extra internal whitespace between fields is collapsed --- */
    rc = dnet_nodespec_parse("N\"  A   B   C  \"::X", &ns);
    check(rc == DNET_CTERM_OK && EQ(ns.username, "A") && EQ(ns.password, "B") &&
          EQ(ns.account, "C"), "surrounding/relative whitespace collapses to 3 fields");

    /* --- 7. doubled-quote "" escape inside the access string --- */
    rc = dnet_nodespec_parse("N\"US\"\"ER PW\"::X", &ns);
    check(rc == DNET_CTERM_OK && EQ(ns.username, "US\"ER") && EQ(ns.password, "PW"),
          "\"\" unescapes to a literal quote inside a field");

    /* --- 8. a "::" INSIDE the access string is NOT the node separator --- */
    rc = dnet_nodespec_parse("N\"A::B PW\"::REAL.SPEC", &ns);
    check(rc == DNET_CTERM_OK, "quoted \"::\" is not the top-level separator");
    check(EQ(ns.node, "N"), "  node = N (split at the real top-level ::)");
    check(EQ(ns.username, "A::B") && EQ(ns.password, "PW"), "  quoted :: kept in field");
    check(EQ(ns.filespec, "REAL.SPEC"), "  filespec = REAL.SPEC");

    /* --- 9. NO node prefix -> local path signal --- */
    rc = dnet_nodespec_parse("DISK$USER:[SMITH]LOGIN.COM;3", &ns);
    check(rc == DNET_NODESPEC_NONODE, "no \"::\" -> DNET_NODESPEC_NONODE");
    /* a single device colon must not be mistaken for a node separator */
    rc = dnet_nodespec_parse("DKA0:FOO.TXT", &ns);
    check(rc == DNET_NODESPEC_NONODE, "device single-colon is not a node");

    /* --- 10. INV-6 refusals (bad structure / bounds), never a silent pass --- */
    rc = dnet_nodespec_parse("N\"unterminated::X", &ns);
    check(rc == DNET_CTERM_EBADLEN, "unterminated access string refused");

    rc = dnet_nodespec_parse("N::", &ns);
    check(rc == DNET_CTERM_EINVAL, "empty file part refused");

    rc = dnet_nodespec_parse("::X", &ns);
    check(rc == DNET_CTERM_EINVAL, "empty node name refused");

    rc = dnet_nodespec_parse("N\"A B C D\"::X", &ns);
    check(rc == DNET_CTERM_EINVAL, "a 4th access field refused (> user/pw/account)");

    rc = dnet_nodespec_parse("N\"A\" junk::X", &ns);
    check(rc == DNET_CTERM_EINVAL, "stray token after a closed access string refused");

    /* over-long field: a 200-char username exceeds DNET_SC_MAX_STR (64) */
    {
        char big[300];
        int n = 0;
        n += sprintf(big + n, "N\"");
        for (int i = 0; i < 200; i++) big[n++] = 'U';
        strcpy(big + n, "\"::X");
        rc = dnet_nodespec_parse(big, &ns);
        check(rc == DNET_CTERM_EBADLEN, "over-long username field refused (not truncated)");
    }

    /* control byte in a field */
    rc = dnet_nodespec_parse("N\"A\tB\"::X", &ns);
    /* a TAB is whitespace to the splitter -> two fields A,B (not a control byte
     * inside a field); assert that faithful behaviour rather than a refusal */
    check(rc == DNET_CTERM_OK && EQ(ns.username, "A") && EQ(ns.password, "B"),
          "TAB separates fields (whitespace), not a control-byte error");
    rc = dnet_nodespec_parse("BAD\x01NODE::X", &ns);
    check(rc == DNET_CTERM_EINVAL, "control byte in the node name refused");

    /* --- 11. null-argument guard --- */
    check(dnet_nodespec_parse(NULL, &ns) == DNET_CTERM_EINVAL, "null spec refused");
    check(dnet_nodespec_parse("N::X", NULL) == DNET_CTERM_EINVAL, "null out refused");

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
