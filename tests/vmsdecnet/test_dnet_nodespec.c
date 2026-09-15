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
 *
 * Plus a FUZZ + bounds proof (the attacker-facing-parser bar the project holds
 * dnet_dap/dnet_fal_access_decode to): these parse an UNTRUSTED COPY argument
 * that carries an access-control PASSWORD, so 200k mutated specs must never
 * over-read (ASan/UBSan, added by the CMake target where the toolchain has it),
 * never leak a half-parsed credential on failure, and never overflow a field.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dnet_nodespec.h"

static int failures = 0;
static void check(int cond, const char *what)
{
    printf("  [%s] %s\n", cond ? "PASS" : "FAIL", what);
    if (!cond)
        failures++;
}
#define EQ(a, b) (strcmp((a), (b)) == 0)

/* Alphabet biased to the metacharacters that drive the parser's branches (quote
 * toggle, "::", the access-string field split, the "" escape) plus a NUL and a
 * control byte -- pure 0..255 noise almost never forms a "::" or a quote, so it
 * would leave the interesting paths unexercised. */
static const char FZ_ALPHA[] = {
    '"', ':', ':', ' ', ' ', 'A', 'B', 'a', 'b', '1', '2', '.',
    '[', ']', '$', '\\', '\t', '\0', '/', ';', '-'
};
#define FZ_A ((int)sizeof FZ_ALPHA)

static void fz_fill(char *s, int cap)
{
    int n = rand() % cap;                 /* 0 .. cap-1 chars, then NUL */
    for (int k = 0; k < n; k++)
        s[k] = FZ_ALPHA[rand() % FZ_A];
    s[n] = '\0';
}

/* A parse result other than OK must leave NOTHING behind -- above all no
 * partial credential (INV-6 clean-on-failure). NONODE also returns *out zeroed. */
static int ns_dirty_on_fail(const struct dnet_nodespec *ns)
{
    return ns->node[0] || ns->username[0] || ns->password[0] ||
           ns->account[0] || ns->filespec[0] || ns->has_access;
}
/* On success every field must be NUL-terminated inside its buffer (no overflow;
 * the last byte of each fixed buffer is the terminator slot and must be NUL). */
static int ns_unbounded_on_ok(const struct dnet_nodespec *ns)
{
    return ns->node[sizeof ns->node - 1] || ns->username[sizeof ns->username - 1] ||
           ns->password[sizeof ns->password - 1] ||
           ns->account[sizeof ns->account - 1] ||
           ns->filespec[sizeof ns->filespec - 1];
}

static void test_nodespec_fuzz(void)
{
    struct dnet_nodespec ns;
    unsigned bad = 0, total = 0;
    srand(0xC0DE);
    for (int i = 0; i < 200000; i++) {
        char s[40];
        fz_fill(s, (int)sizeof s - 1);
        int rc = dnet_nodespec_parse(s, &ns);
        total++;
        if (rc == DNET_CTERM_OK) { if (ns_unbounded_on_ok(&ns)) bad++; }
        else                     { if (ns_dirty_on_fail(&ns))   bad++; }
    }
    check(bad == 0 && total == 200000,
          "nodespec: 200k fuzzed specs -- no leak-on-failure, no field overflow, no crash");

    /* Truncation walk of a valid credentialed spec: every prefix is handled
     * without an over-read (ASan enforces) and never leaks on the failing ones. */
    const char *valid = "VAX1\"SYSTEM SECRET FIELDTEST\"::DISK$U:[X]REMOTE.TXT";
    int clean = 1;
    for (size_t p = 0; p <= strlen(valid); p++) {
        char pre[80];
        memcpy(pre, valid, p); pre[p] = '\0';
        int rc = dnet_nodespec_parse(pre, &ns);
        if (rc != DNET_CTERM_OK && ns_dirty_on_fail(&ns)) clean = 0;
    }
    check(clean, "nodespec: every truncated prefix of a valid spec -- clean, no over-read, no leak");
}

static void test_copy_plan_fuzz(void)
{
    struct dnet_copy_plan cp;
    unsigned bad = 0, total = 0;
    srand(0xF00D);
    for (int i = 0; i < 200000; i++) {
        char a[36], b[36];
        fz_fill(a, (int)sizeof a - 1);
        fz_fill(b, (int)sizeof b - 1);
        int rc = dnet_copy_plan(a, b, &cp);
        total++;
        if (rc == DNET_CTERM_OK) {
            if (cp.node[sizeof cp.node - 1] || cp.username[sizeof cp.username - 1] ||
                cp.password[sizeof cp.password - 1] || cp.account[sizeof cp.account - 1] ||
                cp.remote_spec[sizeof cp.remote_spec - 1] ||
                cp.local_spec[sizeof cp.local_spec - 1])
                bad++;
        } else {
            /* clean-on-failure: no credential / field survives a non-OK plan */
            if (cp.node[0] || cp.username[0] || cp.password[0] || cp.account[0] ||
                cp.remote_spec[0] || cp.local_spec[0] || cp.has_access)
                bad++;
        }
    }
    check(bad == 0 && total == 200000,
          "copy_plan: 200k fuzzed arg pairs -- no leak-on-failure, no field overflow, no crash");
}

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

    /* ================= dnet_copy_plan: COPY direction policy ================= */
    struct dnet_copy_plan cp;

    /* --- 12. remote SOURCE -> GET (remote -> local) --- */
    rc = dnet_copy_plan("VAX1\"SYSTEM SECRET\"::DISK$U:[X]REMOTE.TXT", "LOCAL.TXT", &cp);
    check(rc == DNET_CTERM_OK, "COPY remote-src local-dst plans");
    check(cp.is_get == 1, "  is_get == 1 (remote source -> GET)");
    check(EQ(cp.node, "VAX1"), "  node = VAX1");
    check(EQ(cp.username, "SYSTEM") && EQ(cp.password, "SECRET"), "  creds lifted from the remote");
    check(EQ(cp.remote_spec, "DISK$U:[X]REMOTE.TXT"), "  remote_spec node-stripped");
    check(EQ(cp.local_spec, "LOCAL.TXT"), "  local_spec verbatim");
    check(cp.has_access == 1, "  has_access == 1");

    /* --- 13. remote DEST -> PUT (local -> remote) --- */
    rc = dnet_copy_plan("LOCAL.TXT", "MARS\"GUEST OPEN\"::[000000]OUT.DAT", &cp);
    check(rc == DNET_CTERM_OK, "COPY local-src remote-dst plans");
    check(cp.is_get == 0, "  is_get == 0 (remote dest -> PUT)");
    check(EQ(cp.node, "MARS") && EQ(cp.username, "GUEST") && EQ(cp.password, "OPEN"),
          "  node + creds from the remote dest");
    check(EQ(cp.remote_spec, "[000000]OUT.DAT"), "  remote_spec node-stripped");
    check(EQ(cp.local_spec, "LOCAL.TXT"), "  local_spec verbatim");

    /* --- 14. remote with NO access string (node bare) --- */
    rc = dnet_copy_plan("OVMXR3::A.TXT", "B.TXT", &cp);
    check(rc == DNET_CTERM_OK && cp.is_get == 1 && cp.has_access == 0 &&
          cp.username[0] == '\0' && EQ(cp.node, "OVMXR3") && EQ(cp.remote_spec, "A.TXT"),
          "COPY bare-node remote source -> GET, no creds");

    /* --- 15. refusals --- */
    rc = dnet_copy_plan("LOCAL.TXT", "OTHER.TXT", &cp);
    check(rc == DNET_CTERM_EINVAL, "both-local COPY refused (not a DECnet transfer)");
    rc = dnet_copy_plan("A::X", "B::Y", &cp);
    check(rc == DNET_CTERM_EINVAL, "node-to-node COPY refused (not the outbound-client path)");
    rc = dnet_copy_plan("N\"unterminated::X", "LOCAL.TXT", &cp);
    check(rc == DNET_CTERM_EBADLEN, "splitter error on a side is propagated");
    check(dnet_copy_plan(NULL, "B", &cp) == DNET_CTERM_EINVAL, "null src refused");
    check(dnet_copy_plan("A::X", NULL, &cp) == DNET_CTERM_EINVAL, "null dst refused");
    check(dnet_copy_plan("A::X", "B", NULL) == DNET_CTERM_EINVAL, "null out refused");

    /* ===================== fuzz + bounds proof ============================= */
    test_nodespec_fuzz();
    test_copy_plan_fuzz();

    printf("%s: %d failure(s)\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
