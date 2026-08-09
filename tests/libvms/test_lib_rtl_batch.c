/*
 * test_lib_rtl_batch.c - Unit tests for the LIB$ RTL routines added under
 * vms-801 R2.2:
 *
 *   lib$addx  lib$subx  lib$emul  lib$ediv          (extended arithmetic)
 *   lib$ffs   lib$ffc                               (bit scan)
 *   lib$crc_table  lib$crc                          (CRC)
 *   lib$scanc lib$skpc lib$char                     (character scan/convert)
 *   lib$analyze_sdesc  lib$analyze_sdesc_64         (descriptor analysis)
 *   lib$insqhi lib$insqti lib$remqhi lib$remqti     (self-relative queues)
 *   lib$insert_tree lib$lookup_tree lib$traverse_tree (binary tree)
 *   lib$get_common lib$put_common                   (process common area)
 *
 * Assertions are grounded in the documented behaviour (OpenVMS RTL Library
 * Manual) and in known test vectors (e.g. CRC-16/ARC of "123456789" =
 * 0xBB3D).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lib$routines.h"
#include "libdef.h"
#include "ssdef.h"
#include "descrip.h"

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
/* lib$addx / lib$subx — multiple-precision add / subtract            */
/* ------------------------------------------------------------------ */
static void test_addx_subx(void)
{
    printf("Testing lib$addx / lib$subx...\n");
    int32_t len = 2;

    /* 0x0000222200001111 + 0x0000444400003333 = 0x0000666600004444 */
    uint32_t a[2] = {0x00001111u, 0x00002222u};
    uint32_t b[2] = {0x00003333u, 0x00004444u};
    uint32_t s[2] = {0, 0};
    uint32_t st = lib$addx(a, b, s, &len);
    check(st == SS$_NORMAL, "addx returns SS$_NORMAL");
    check(s[0] == 0x00004444u && s[1] == 0x00006666u, "addx sum correct");

    /* Carry across the longword boundary: 0xFFFFFFFF + 1 = 0x1_00000000 */
    uint32_t c1[2] = {0xFFFFFFFFu, 0};
    uint32_t c2[2] = {1u, 0};
    uint32_t cs[2] = {0, 0};
    lib$addx(c1, c2, cs, &len);
    check(cs[0] == 0 && cs[1] == 1, "addx carry propagates");

    /* Subtract: 0x0000444400003333 - 0x0000222200001111 = 0x0000222200002222 */
    uint32_t d[2] = {0x00003333u, 0x00004444u};
    uint32_t e[2] = {0x00001111u, 0x00002222u};
    uint32_t diff[2] = {0, 0};
    st = lib$subx(d, e, diff, &len);
    check(st == SS$_NORMAL, "subx returns SS$_NORMAL");
    check(diff[0] == 0x00002222u && diff[1] == 0x00002222u, "subx diff correct");

    /* Borrow across the boundary: 0x1_00000000 - 1 = 0xFFFFFFFF */
    uint32_t f[2] = {0, 1};
    uint32_t g[2] = {1, 0};
    uint32_t bdiff[2] = {0, 0};
    lib$subx(f, g, bdiff, &len);
    check(bdiff[0] == 0xFFFFFFFFu && bdiff[1] == 0, "subx borrow propagates");
}

/* ------------------------------------------------------------------ */
/* lib$emul / lib$ediv — extended multiply / divide                   */
/* ------------------------------------------------------------------ */
static void test_emul_ediv(void)
{
    printf("Testing lib$emul / lib$ediv...\n");

    int32_t mplr = 4096, mplcand = 268435456, addend = 0;
    long long product = 0;
    uint32_t st = lib$emul(&mplr, &mplcand, &addend, &product);
    check(st == SS$_NORMAL, "emul returns SS$_NORMAL");
    check(product == 0x10000000000LL, "emul 4096*268435456 = 0x10000000000");

    int32_t m2 = 3, mc2 = 4, ad2 = 5;
    lib$emul(&m2, &mc2, &ad2, &product);
    check(product == 17, "emul 3*4+5 = 17");

    /* 4600387192 / 4096 = 1123141 remainder 1656 */
    int32_t divisor = 4096;
    long long dividend = 4600387192LL;
    int32_t quotient = 0, remainder = 0;
    st = lib$ediv(&divisor, &dividend, &quotient, &remainder);
    check(st == SS$_NORMAL, "ediv returns SS$_NORMAL");
    check(quotient == 1123141 && remainder == 1656, "ediv quotient/remainder");

    /* Remainder carries the sign of the dividend: -17 / 5 = -3 rem -2 */
    int32_t d2 = 5;
    long long nd = -17;
    lib$ediv(&d2, &nd, &quotient, &remainder);
    check(quotient == -3 && remainder == -2, "ediv negative dividend");

    /* Divide by zero is rejected. */
    int32_t zero = 0;
    long long any = 10;
    st = lib$ediv(&zero, &any, &quotient, &remainder);
    check(st == LIB$_INVARG, "ediv by zero returns LIB$_INVARG");
}

/* ------------------------------------------------------------------ */
/* lib$ffs / lib$ffc — find first set / clear bit                     */
/* ------------------------------------------------------------------ */
static void test_ffs_ffc(void)
{
    printf("Testing lib$ffs / lib$ffc...\n");

    int32_t x = 32 + 16;          /* bits 4 and 5 set */
    int32_t pos = 0, find = -1;
    uint8_t siz = 32;
    uint32_t st = lib$ffs(&pos, &siz, &x, &find);
    check(st == SS$_NORMAL, "ffs found a set bit");
    check(find == 4, "ffs first set bit of 0x30 is bit 4");

    int32_t zero = 0;
    st = lib$ffs(&pos, &siz, &zero, &find);
    check(st == LIB$_NOTFOU, "ffs on 0 returns LIB$_NOTFOU");

    /* first clear bit of 0b...0111 (bits 0,1,2 set) starting at 0 is bit 3 */
    int32_t y = 0x7;
    st = lib$ffc(&pos, &siz, &y, &find);
    check(st == SS$_NORMAL, "ffc found a clear bit");
    check(find == 3, "ffc first clear bit of 0x7 is bit 3");

    /* start the search past the low set bits: first clear at/after bit 1 is 3 */
    int32_t p1 = 1;
    uint8_t s1 = 10;
    st = lib$ffc(&p1, &s1, &y, &find);
    check(st == SS$_NORMAL && find == 3, "ffc honours start position");

    /* all bits set within the window -> not found */
    uint32_t allset = 0xFFFFFFFFu;
    st = lib$ffc(&pos, &siz, &allset, &find);
    check(st == LIB$_NOTFOU, "ffc on all-ones returns LIB$_NOTFOU");
}

/* ------------------------------------------------------------------ */
/* lib$crc_table / lib$crc — CRC-16/ARC test vector                   */
/* ------------------------------------------------------------------ */
static void test_crc(void)
{
    printf("Testing lib$crc_table / lib$crc...\n");

    uint32_t table[16];
    uint32_t poly = 0xA001u;      /* reflected CRC-16 (0x8005), = 0120001 octal */
    lib$crc_table(&poly, table);
    check(table[0] == 0, "crc_table[0] == 0");

    uint32_t init = 0;
    $DESCRIPTOR(check_d, "123456789");
    uint32_t crc = lib$crc(table, &init, &check_d);
    check((crc & 0xFFFFu) == 0xBB3Du, "CRC-16/ARC of \"123456789\" == 0xBB3D");

    /* Empty string yields the initial value. */
    struct dsc$descriptor_s empty = {0, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)""};
    uint32_t init2 = 0x1234;
    crc = lib$crc(table, &init2, &empty);
    check(crc == 0x1234, "crc of empty string == initial value");

    /* Deterministic and content-sensitive. */
    $DESCRIPTOR(a_d, "A");
    $DESCRIPTOR(b_d, "B");
    check(lib$crc(table, &init, &a_d) == lib$crc(table, &init, &a_d),
          "crc deterministic");
    check(lib$crc(table, &init, &a_d) != lib$crc(table, &init, &b_d),
          "crc distinguishes different data");
}

/* ------------------------------------------------------------------ */
/* lib$scanc / lib$skpc / lib$char                                    */
/* ------------------------------------------------------------------ */
static void test_scanc_skpc_char(void)
{
    printf("Testing lib$scanc / lib$skpc / lib$char...\n");

    $DESCRIPTOR(str_d, "the quick brown fox jumps over the lazy dog");
    uint8_t table[256];
    memset(table, 0, sizeof(table));
    table['a'] = 1;
    table['b'] = 1;

    uint8_t mask = 1;
    uint32_t off = lib$scanc(&str_d, table, &mask);
    /* first 'b' (in "brown") is at 0-based index 10 -> 1-based 11 */
    check(off == 11, "scanc finds first a/b at position 11");

    mask = 2;
    off = lib$scanc(&str_d, table, &mask);
    check(off == 0, "scanc with non-matching mask returns 0");

    $DESCRIPTOR(space_d, " ");
    $DESCRIPTOR(quote_d, "   When");
    uint32_t idx = lib$skpc(&space_d, &quote_d);
    check(idx == 4, "skpc first non-space at position 4");

    $DESCRIPTOR(aaa_d, "aaa");
    $DESCRIPTOR(a_d, "a");
    idx = lib$skpc(&a_d, &aaa_d);
    check(idx == 0, "skpc all-match returns 0");

    char buf[6] = {0};
    struct dsc$descriptor_s buf_d = {5, DSC$K_DTYPE_T, DSC$K_CLASS_S, buf};
    uint8_t code = 65;   /* 'A' */
    uint32_t st = lib$char(&buf_d, &code);
    check(st == SS$_NORMAL, "char returns SS$_NORMAL");
    check(buf[0] == 'A', "char wrote 'A'");
    check(buf[1] == ' ' && buf[4] == ' ', "char space-filled fixed destination");
    check(buf_d.dsc$w_length == 5, "char left fixed length unchanged");
}

/* ------------------------------------------------------------------ */
/* lib$analyze_sdesc / lib$analyze_sdesc_64                           */
/* ------------------------------------------------------------------ */
static void test_analyze_sdesc(void)
{
    printf("Testing lib$analyze_sdesc / _64...\n");

    $DESCRIPTOR(d, "test descriptor");   /* 15 chars */
    uint16_t len = 0;
    void *addr = NULL;
    uint32_t st = lib$analyze_sdesc(&d, &len, &addr);
    check(st == SS$_NORMAL, "analyze_sdesc returns SS$_NORMAL");
    check(len == 15, "analyze_sdesc length == 15");
    check(addr == d.dsc$a_pointer, "analyze_sdesc address matches");

    uint64_t len64 = 0;
    addr = NULL;
    st = lib$analyze_sdesc_64(&d, &len64, &addr);
    check(st == SS$_NORMAL, "analyze_sdesc_64 returns SS$_NORMAL");
    check(len64 == 15, "analyze_sdesc_64 length == 15");
    check(addr == d.dsc$a_pointer, "analyze_sdesc_64 address matches");
}

/* ------------------------------------------------------------------ */
/* lib$insqhi / lib$insqti / lib$remqhi / lib$remqti                  */
/* ------------------------------------------------------------------ */
struct qelem {
    void *flink;
    void *blink;
    unsigned int data;
};

static void test_queue(void)
{
    printf("Testing lib$insqhi / insqti / remqhi / remqti...\n");

    long long header = 0;
    uint32_t retry = 1;
    struct qelem e1 = {0, 0, 1};
    struct qelem e2 = {0, 0, 2};
    struct qelem e3 = {0, 0, 3};

    /* Insert e1 into an empty queue -> ONEENTQUE. */
    uint32_t st = lib$insqhi(&e1, &header, &retry);
    check(st == LIB$_ONEENTQUE, "insqhi into empty queue -> ONEENTQUE");

    /* Head insert e2, tail insert e3 -> order is e2, e1, e3. */
    st = lib$insqhi(&e2, &header, &retry);
    check(st == SS$_NORMAL, "insqhi non-empty -> SS$_NORMAL");
    st = lib$insqti(&e3, &header, &retry);
    check(st == SS$_NORMAL, "insqti non-empty -> SS$_NORMAL");

    struct qelem *p = NULL;
    st = lib$remqhi(&header, &p, &retry);
    check(st == SS$_NORMAL && p == &e2, "remqhi first == e2");
    st = lib$remqhi(&header, &p, &retry);
    check(st == SS$_NORMAL && p == &e1, "remqhi second == e1");
    st = lib$remqti(&header, &p, &retry);
    check(st == LIB$_ONEENTQUE && p == &e3, "remqti last == e3 (ONEENTQUE)");

    st = lib$remqhi(&header, &p, &retry);
    check(st == LIB$_QUEWASEMP, "remqhi on empty -> QUEWASEMP");
    st = lib$remqti(&header, &p, &retry);
    check(st == LIB$_QUEWASEMP, "remqti on empty -> QUEWASEMP");
}

/* ------------------------------------------------------------------ */
/* lib$insert_tree / lib$lookup_tree / lib$traverse_tree              */
/* ------------------------------------------------------------------ */
struct tnode {
    void *llink;
    void *rlink;
    short reserved;
    char key[16];
    unsigned int count;
};

static int t_compare(char *key, struct tnode *node)
{
    return strcmp(key, node->key);
}

static int t_allocate(char *key, struct tnode **node_out, void *user_data)
{
    (void)user_data;
    static struct tnode pool[16];
    static int next = 0;
    struct tnode *n = &pool[next++];
    memset(n, 0, sizeof(*n));
    strncpy(n->key, key, sizeof(n->key) - 1);
    n->count = 1;
    *node_out = n;
    return SS$_NORMAL;
}

static char traverse_buf[128];
static int t_action(struct tnode *node, void *user_data)
{
    (void)user_data;
    strncat(traverse_buf, node->key, sizeof(traverse_buf) - strlen(traverse_buf) - 1);
    strncat(traverse_buf, " ", sizeof(traverse_buf) - strlen(traverse_buf) - 1);
    return SS$_NORMAL;
}

static void test_tree(void)
{
    printf("Testing lib$insert_tree / lookup_tree / traverse_tree...\n");

    struct tnode *head = NULL;
    uint32_t flags = 0;
    struct tnode *node = NULL;
    const char *keys[] = {"dog", "apple", "cat", "banana", "apple"};

    for (int i = 0; i < 5; i++) {
        uint32_t st = lib$insert_tree(&head, (void *)keys[i], &flags,
                                      (int (*)(void))t_compare,
                                      (int (*)(void))t_allocate,
                                      &node, NULL);
        if (i == 4) {
            /* second "apple" is a duplicate */
            check(st == LIB$_KEYALRINS, "insert_tree duplicate -> KEYALRINS");
            node->count++;
        } else {
            check(st == SS$_NORMAL, "insert_tree new key -> SS$_NORMAL");
        }
    }

    /* lookup existing + missing */
    uint32_t st = lib$lookup_tree(&head, (void *)"cat",
                                  (int (*)(void))t_compare, &node);
    check(st == SS$_NORMAL && strcmp(node->key, "cat") == 0, "lookup_tree finds cat");
    st = lib$lookup_tree(&head, (void *)"apple",
                         (int (*)(void))t_compare, &node);
    check(st == SS$_NORMAL && node->count == 2, "lookup_tree apple count == 2");
    st = lib$lookup_tree(&head, (void *)"missing",
                         (int (*)(void))t_compare, &node);
    check(st == LIB$_KEYNOTFOU, "lookup_tree missing -> KEYNOTFOU");

    /* traversal is in ascending key order */
    traverse_buf[0] = '\0';
    st = lib$traverse_tree(&head, (int (*)(void))t_action, NULL);
    check(st == SS$_NORMAL, "traverse_tree returns SS$_NORMAL");
    check(strcmp(traverse_buf, "apple banana cat dog ") == 0,
          "traverse_tree visits nodes in sorted order");
}

/* ------------------------------------------------------------------ */
/* lib$get_common / lib$put_common                                    */
/* ------------------------------------------------------------------ */
static void test_common(void)
{
    printf("Testing lib$get_common / lib$put_common...\n");

    char rbuf[256];
    struct dsc$descriptor_s r_d = {255, DSC$K_DTYPE_T, DSC$K_CLASS_S, rbuf};
    uint16_t rlen = 255;

    /* Fresh process: common area is empty. */
    uint32_t st = lib$get_common(&r_d, &rlen);
    check(st == SS$_NORMAL, "get_common (empty) returns SS$_NORMAL");
    check(rlen == 0, "get_common (empty) length == 0");

    /* Store, then retrieve. */
    $DESCRIPTOR(store_d, "counter=42");
    st = lib$put_common(&store_d);
    check(st == SS$_NORMAL, "put_common returns SS$_NORMAL");

    r_d.dsc$w_length = 255;
    st = lib$get_common(&r_d, &rlen);
    check(st == SS$_NORMAL, "get_common (stored) returns SS$_NORMAL");
    check(rlen == 10 && memcmp(rbuf, "counter=42", 10) == 0,
          "get_common round-trips the stored data");
}

int main(void)
{
    printf("=== LIB$ RTL batch (vms-801 R2.2) unit tests ===\n");
    test_addx_subx();
    test_emul_ediv();
    test_ffs_ffc();
    test_crc();
    test_scanc_skpc_char();
    test_analyze_sdesc();
    test_queue();
    test_tree();
    test_common();

    printf("\n%s: %d failure(s)\n", failures ? "FAILED" : "PASSED", failures);
    return failures ? 1 : 0;
}
