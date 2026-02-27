/*
 * test_lib_bitops.c - Unit tests for lib$extv, lib$extzv, lib$insv,
 *                     lib$adawi, lib$bbcci, lib$bbssi
 *
 * Tests VMS bitfield extraction, insertion, and atomic bit ops.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "lib$routines.h"
#include "ssdef.h"

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
/* lib$extzv — zero-extended field extraction                         */
/* ------------------------------------------------------------------ */
static void test_extzv(void)
{
    printf("Testing lib$extzv (zero-extended bit field extract)...\n");

    /* Byte array: 0b11001010 = 0xCA */
    unsigned char data[4] = {0xCA, 0x00, 0x00, 0x00};

    /* Extract bits 1..3 from byte 0: bits are [bit7..bit0] = 1100_1010
     * bit 0 = 0, bit 1 = 1, bit 2 = 0, bit 3 = 1 → field[1..3] = 0b101 = 5 */
    int32_t  pos  = 1;
    uint8_t  size = 3;
    uint32_t val  = lib$extzv(&pos, &size, data);
    check(val == 5, "extzv bits 1-3 of 0xCA = 5");

    /* Extract bit 0 alone: 0xCA & 1 = 0 */
    int32_t  pos2  = 0;
    uint8_t  size2 = 1;
    val = lib$extzv(&pos2, &size2, data);
    check(val == 0, "extzv bit 0 of 0xCA = 0");

    /* Extract bits 7 alone: MSB of 0xCA = 1 */
    int32_t  pos3  = 7;
    uint8_t  size3 = 1;
    val = lib$extzv(&pos3, &size3, data);
    check(val == 1, "extzv bit 7 of 0xCA = 1");

    /* Extract full 8 bits: should be 0xCA = 202 */
    int32_t  pos4  = 0;
    uint8_t  size4 = 8;
    val = lib$extzv(&pos4, &size4, data);
    check(val == 0xCA, "extzv all 8 bits = 0xCA");

    /* Zero-size extraction = 0 */
    int32_t  pos5  = 0;
    uint8_t  size5 = 0;
    val = lib$extzv(&pos5, &size5, data);
    check(val == 0, "extzv size 0 = 0");

    /* Cross-byte boundary: data[0]=0xCA (11001010), data[1]=0xF0 (11110000)
     * bits 6-9: bit6=1, bit7=1, bit8=0, bit9=0 → 0b0011 = 3 */
    unsigned char data2[4] = {0xCA, 0xF0, 0x00, 0x00};
    int32_t  pos6  = 6;
    uint8_t  size6 = 4;
    val = lib$extzv(&pos6, &size6, data2);
    /* 0xCA = 11001010, bits 6-7 = 11; 0xF0 = 11110000, bits 0-1 = 00 */
    /* field at bit 6, size 4: bit6=1, bit7=1, bit8=0, bit9=0 → 0b0011 = 3 */
    check(val == 3, "extzv cross-byte boundary");
}

/* ------------------------------------------------------------------ */
/* lib$extv — sign-extended field extraction                          */
/* ------------------------------------------------------------------ */
static void test_extv(void)
{
    printf("Testing lib$extv (sign-extended bit field extract)...\n");

    /* 0b00001111 = 0x0F */
    unsigned char data[4] = {0x0F, 0x00, 0x00, 0x00};

    /* Extract bits 0-3 (= 0b1111 = 15 unsigned), sign-extended = -1 */
    int32_t pos  = 0;
    uint8_t size = 4;
    int32_t val  = lib$extv(&pos, &size, data);
    check(val == -1, "extv 0b1111 (4-bit) sign-extends to -1");

    /* Extract bits 4-7 (= 0b0000 = 0), sign-extended = 0 */
    int32_t pos2  = 4;
    uint8_t size2 = 4;
    val = lib$extv(&pos2, &size2, data);
    check(val == 0, "extv 0b0000 (4-bit) sign-extends to 0");

    /* Positive: bits 0-2 of 0x0F = 0b111 = 7, but sign-extends because bit 2 set */
    /* 3-bit field with all 1s: -1 */
    int32_t pos3  = 0;
    uint8_t size3 = 3;
    val = lib$extv(&pos3, &size3, data);
    check(val == -1, "extv 3-bit all-ones sign-extends to -1");

    /* Positive: bits 3-5 of 0x0F = bit3=1, bit4=0, bit5=0 → 0b001 = 1 */
    int32_t pos4  = 3;
    uint8_t size4 = 3;
    val = lib$extv(&pos4, &size4, data);
    check(val == 1, "extv 3-bit positive stays positive");
}

/* ------------------------------------------------------------------ */
/* lib$insv — bit field insertion                                      */
/* ------------------------------------------------------------------ */
static void test_insv(void)
{
    printf("Testing lib$insv (bit field insert)...\n");

    unsigned char buf[4];

    /* Insert 0b101 (5) at bit position 1, size 3 */
    memset(buf, 0, sizeof(buf));
    int32_t src  = 5;   /* 0b101 */
    int32_t pos  = 1;
    uint8_t size = 3;
    uint32_t st  = lib$insv(&src, &pos, &size, buf);
    check(st == SS$_NORMAL, "insv returns SS$_NORMAL");
    /* bits 1,2,3 of buf[0] should be 1,0,1 → buf[0] & 0b00001110 = 0b00001010 = 0x0A */
    check((buf[0] & 0x0E) == 0x0A, "insv 5 at bit 1 size 3");

    /* Insert 0 (clear bits) at bit position 1, size 3 */
    memset(buf, 0xFF, sizeof(buf));
    int32_t src2  = 0;
    int32_t pos2  = 1;
    uint8_t size2 = 3;
    lib$insv(&src2, &pos2, &size2, buf);
    check((buf[0] & 0x0E) == 0x00, "insv 0 clears bits 1-3");

    /* Insert at bit 0, size 8 (full byte replacement) */
    memset(buf, 0xFF, sizeof(buf));
    int32_t src3  = 0xAB;
    int32_t pos3  = 0;
    uint8_t size3 = 8;
    lib$insv(&src3, &pos3, &size3, buf);
    check(buf[0] == 0xAB, "insv full byte at bit 0");

    /* Cross-byte boundary: insert 0b1111 at bit 6, size 4 */
    memset(buf, 0x00, sizeof(buf));
    int32_t src4  = 0xF;   /* all 4 bits set */
    int32_t pos4  = 6;
    uint8_t size4 = 4;
    lib$insv(&src4, &pos4, &size4, buf);
    /* bits 6,7 of buf[0] should be set; bits 0,1 of buf[1] should be set */
    check((buf[0] & 0xC0) == 0xC0, "insv cross-byte: upper bits of buf[0]");
    check((buf[1] & 0x03) == 0x03, "insv cross-byte: lower bits of buf[1]");

    /* Zero size = no-op */
    memset(buf, 0xAA, sizeof(buf));
    int32_t src5  = 0xFF;
    int32_t pos5  = 0;
    uint8_t size5 = 0;
    lib$insv(&src5, &pos5, &size5, buf);
    check(buf[0] == (char)0xAA, "insv size 0 is no-op");
}

/* ------------------------------------------------------------------ */
/* lib$adawi — atomic add interlocked                                  */
/* ------------------------------------------------------------------ */
static void test_adawi(void)
{
    printf("Testing lib$adawi (atomic add word)...\n");

    int16_t add  = 10;
    int16_t sum  = 5;
    int16_t sign = 0;

    uint32_t st = lib$adawi(&add, &sum, &sign);
    check(st == SS$_NORMAL, "adawi returns SS$_NORMAL");
    check(sum  == 15, "adawi result = 15");
    check(sign == 1,  "adawi sign = +1 for positive result");

    /* Result is zero */
    int16_t add2  = -15;
    int16_t sum2  = 15;
    int16_t sign2 = 99;
    lib$adawi(&add2, &sum2, &sign2);
    check(sum2  == 0, "adawi result = 0");
    check(sign2 == 0, "adawi sign = 0 for zero result");

    /* Result is negative */
    int16_t add3  = -10;
    int16_t sum3  = 3;
    int16_t sign3 = 99;
    lib$adawi(&add3, &sum3, &sign3);
    check(sum3  == -7, "adawi result = -7");
    check(sign3 == -1, "adawi sign = -1 for negative result");

    /* Null parameters */
    st = lib$adawi(NULL, &sum, &sign);
    check(st == SS$_BADPARAM, "adawi null add returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$bbcci / lib$bbssi — atomic bit test-and-clear / test-and-set   */
/* ------------------------------------------------------------------ */
static void test_bbcci_bbssi(void)
{
    printf("Testing lib$bbcci / lib$bbssi (atomic bit ops)...\n");

    unsigned char bits[4] = {0x00, 0x00, 0x00, 0x00};

    /* Set bit 3 (initially clear) */
    int32_t pos = 3;
    uint32_t old = lib$bbssi(&pos, bits);
    check(old == 0, "bbssi: old value was 0 (clear)");
    check((bits[0] & (1 << 3)) != 0, "bbssi: bit 3 is now set");

    /* Set bit 3 again (already set) */
    old = lib$bbssi(&pos, bits);
    check(old == 1, "bbssi: old value was 1 (set)");

    /* Clear bit 3 */
    old = lib$bbcci(&pos, bits);
    check(old == 1, "bbcci: old value was 1 (set)");
    check((bits[0] & (1 << 3)) == 0, "bbcci: bit 3 is now clear");

    /* Clear bit 3 again (already clear) */
    old = lib$bbcci(&pos, bits);
    check(old == 0, "bbcci: old value was 0 (clear)");

    /* Test bit in second byte (bit position 8) */
    unsigned char bits2[2] = {0x00, 0x00};
    int32_t pos2 = 8;
    lib$bbssi(&pos2, bits2);
    check(bits2[1] == 0x01, "bbssi: bit 8 sets LSB of byte 1");

    lib$bbcci(&pos2, bits2);
    check(bits2[1] == 0x00, "bbcci: bit 8 clears LSB of byte 1");

    /* Null parameters return 0 (not crash) */
    old = lib$bbssi(NULL, bits);
    check(old == 0, "bbssi null pos returns 0");
    old = lib$bbcci(&pos, NULL);
    check(old == 0, "bbcci null base returns 0");
}

/* ------------------------------------------------------------------ */
/* Bit boundary: bit 31 (MSB of first 4 bytes)                        */
/* ------------------------------------------------------------------ */
static void test_bit31(void)
{
    printf("Testing bit 31 boundary conditions...\n");

    unsigned char data[4] = {0x00, 0x00, 0x00, 0x80};  /* bit 31 set */
    int32_t pos  = 31;
    uint8_t size = 1;

    uint32_t val = lib$extzv(&pos, &size, data);
    check(val == 1, "extzv bit 31 of 0x80000000 = 1");

    int32_t sval = lib$extv(&pos, &size, data);
    check(sval == -1, "extv bit 31 sign-extended to -1");
}

int main(void)
{
    printf("=== test_lib_bitops: lib$extv, lib$extzv, lib$insv, lib$adawi, lib$bbcci, lib$bbssi ===\n");

    test_extzv();
    test_extv();
    test_insv();
    test_adawi();
    test_bbcci_bbssi();
    test_bit31();

    if (failures == 0)
        printf("All lib_bitops tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
