/*
 * test_sysuaf_record.c - Layout tests for the binary SYSUAF record (vms-846.1)
 *
 * Verifies the fixed-length on-disk record for the RMS indexed SYSUAF.DAT:
 *   - deterministic size / field offsets (no implicit padding)
 *   - primary key XABKEY geometry (segment 0 = 32-byte username at offset 0)
 *   - documented UIC encoding: (group << 16) | member
 *
 * The struct's own _Static_assert block guards the layout at compile time;
 * this test proves it at run time and exercises the key-XAB builder.
 */

#include <stdio.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>
#include "sysuaf.h"
#include "rms/xab.h"

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

int main(void)
{
    printf("test_sysuaf_record: binary SYSUAF record layout (vms-846.1)\n");

    /* --- Fixed on-disk layout: size and field offsets --- */
    check(sizeof(sysuaf_rms_record_t) == 368, "record is 368 bytes");
    check(offsetof(sysuaf_rms_record_t, uaf$t_username) == 0, "username @ 0");
    check(offsetof(sysuaf_rms_record_t, uaf$b_pwd)      == 32, "pwd hash @ 32");
    check(offsetof(sysuaf_rms_record_t, uaf$l_uic)      == 96, "uic @ 96");
    check(offsetof(sysuaf_rms_record_t, uaf$l_flags)    == 100, "flags @ 100");
    check(offsetof(sysuaf_rms_record_t, uaf$q_priv)     == 104, "priv @ 104");
    check(offsetof(sysuaf_rms_record_t, uaf$t_defdir)   == 112, "defdir @ 112");

    /* Field widths match the declared key/field constants. */
    sysuaf_rms_record_t rec;
    check(sizeof(rec.uaf$t_username) == SYSUAF_USERNAME_LEN, "username width 32");
    check(sizeof(rec.uaf$b_pwd)      == SYSUAF_PWHASH_LEN,   "pwd width 64");
    check(sizeof(rec.uaf$t_defdir)   == SYSUAF_DEFDIR_LEN,   "defdir width 256");

    /* --- UIC encoding: group in high word, member in low word --- */
    memset(&rec, 0, sizeof(rec));
    uint32_t group = 0x0010, member = 0x0004;   /* [20,4] in octal-ish form */
    rec.uaf$l_uic = (group << 16) | member;
    check((rec.uaf$l_uic >> 16)     == group,  "uic group in high word");
    check((rec.uaf$l_uic & 0xFFFF)  == member, "uic member in low word");

    /* --- Primary key XABKEY geometry --- */
    struct XABKEY k = sysuaf_rms_primary_key();
    check(k.xab$b_cod == XAB$C_KEY,  "xab is a key definition");
    check(k.xab$b_ref == 0,          "key of reference 0 (primary)");
    check(k.xab$b_dtp == XAB$C_STG,  "key data type string");
    check(k.xab$b_nseg == 1,         "single key segment");
    check(k.xab$w_pos0 == 0,         "key segment at record offset 0");
    check(k.xab$b_siz0 == SYSUAF_USERNAME_LEN, "key segment size 32");
    check(k.xab$w_tks  == SYSUAF_USERNAME_LEN, "total key size 32");
    check((k.xab$w_flg & XAB$M_DUP) == 0, "no duplicate keys (usernames unique)");
    check(k.xab$l_knm != NULL && strcmp(k.xab$l_knm, "USERNAME") == 0,
          "key name USERNAME");

    printf("test_sysuaf_record: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
