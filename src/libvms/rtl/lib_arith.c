/*
 * lib_arith.c - LIB$ Extended (multiple-precision) arithmetic + CRC
 *
 * Implements:
 *
 *   LIB$ADDX       - Add two multiple-precision binary numbers
 *   LIB$SUBX       - Subtract two multiple-precision binary numbers
 *   LIB$EMUL       - Extended multiply (32x32 -> 64, plus addend)
 *   LIB$EDIV       - Extended divide (64 / 32 -> 32 quotient + 32 remainder)
 *   LIB$CRC_TABLE  - Build the 16-longword CRC table
 *   LIB$CRC        - Compute a CRC over a string
 *
 * The multiple-precision operands are arrays of longwords stored low
 * order first (little end first), matching the VAX/Alpha in-memory
 * representation the LIB$ jackets operate on.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual
 *              - LIB$ADDX / LIB$SUBX (multiple-precision add/subtract),
 *              - LIB$EMUL (extended multiply, product = a*b + c),
 *              - LIB$EDIV (extended divide, remainder carries the sign of
 *                the dividend),
 *              - LIB$CRC_TABLE / LIB$CRC (nibble-at-a-time CRC).
 *            OpenVMS VAX Architecture Reference Manual (EMUL/EDIV
 *            instruction semantics).
 */

#include <stdint.h>
#include "ssdef.h"
#include "lib$routines.h"

/*
 * lib$addx - Add two multiple-precision binary numbers.
 *
 * sum = addend + augend, treating each operand as an array of
 * array_length longwords, low order first.  array_length is optional;
 * when omitted the operands are a single quadword (2 longwords), the
 * VMS default.
 */
uint32_t lib$addx(const uint32_t *addend, const uint32_t *augend,
                  uint32_t *sum, const int32_t *array_length)
{
    if (!addend || !augend || !sum)
        return SS$_BADPARAM;

    int32_t n = array_length ? *array_length : 2;
    if (n <= 0)
        return SS$_BADPARAM;

    uint64_t carry = 0;
    for (int32_t i = 0; i < n; i++) {
        uint64_t s = (uint64_t)addend[i] + (uint64_t)augend[i] + carry;
        sum[i] = (uint32_t)s;
        carry = s >> 32;
    }
    return SS$_NORMAL;
}

/*
 * lib$subx - Subtract two multiple-precision binary numbers.
 *
 * difference = minuend - subtrahend, longword arrays low order first.
 */
uint32_t lib$subx(const uint32_t *minuend, const uint32_t *subtrahend,
                  uint32_t *difference, const int32_t *array_length)
{
    if (!minuend || !subtrahend || !difference)
        return SS$_BADPARAM;

    int32_t n = array_length ? *array_length : 2;
    if (n <= 0)
        return SS$_BADPARAM;

    uint64_t borrow = 0;
    for (int32_t i = 0; i < n; i++) {
        uint64_t sub = (uint64_t)subtrahend[i] + borrow;
        uint64_t m = (uint64_t)minuend[i];
        if (m >= sub) {
            difference[i] = (uint32_t)(m - sub);
            borrow = 0;
        } else {
            difference[i] = (uint32_t)(m + 0x100000000ULL - sub);
            borrow = 1;
        }
    }
    return SS$_NORMAL;
}

/*
 * lib$emul - Extended multiply.
 *
 * product(64) = (multiplier * multiplicand) + addend, all operands
 * treated as signed longwords.
 */
uint32_t lib$emul(const int32_t *multiplier, const int32_t *multiplicand,
                  const int32_t *addend, long long *product)
{
    if (!multiplier || !multiplicand || !addend || !product)
        return SS$_BADPARAM;

    *product = (long long)((int64_t)*multiplier * (int64_t)*multiplicand
                           + (int64_t)*addend);
    return SS$_NORMAL;
}

/*
 * lib$ediv - Extended divide.
 *
 * quotient  = dividend / divisor
 * remainder = dividend - (quotient * divisor)
 *
 * Both are truncated toward zero, so the remainder carries the sign of
 * the dividend, matching the VAX EDIV instruction and the LIB$EDIV
 * jacket.
 */
uint32_t lib$ediv(const int32_t *divisor, const long long *dividend,
                  int32_t *quotient, int32_t *remainder)
{
    if (!divisor || !dividend || !quotient || !remainder)
        return SS$_BADPARAM;
    if (*divisor == 0)
        return LIB$_INVARG;

    int64_t divd = (int64_t)*dividend;
    int64_t divs = (int64_t)*divisor;

    *quotient  = (int32_t)(divd / divs);
    *remainder = (int32_t)(divd % divs);
    return SS$_NORMAL;
}

/*
 * lib$crc_table - Build the 16-longword CRC table used by lib$crc.
 *
 * The polynomial is supplied by the caller.  Each of the 16 table
 * entries is derived by feeding the nibble value through four
 * shift-and-conditional-XOR steps, which is the standard reflected
 * nibble-table construction LIB$CRC consumes.
 */
void lib$crc_table(const uint32_t *polynomial, uint32_t *table)
{
    if (!polynomial || !table)
        return;

    uint32_t poly = *polynomial;
    for (uint32_t group = 0; group < 16; group++) {
        uint32_t temp = group;
        for (int i = 0; i < 4; i++) {
            if (temp & 1u)
                temp = (temp >> 1) ^ poly;
            else
                temp = temp >> 1;
        }
        table[group] = temp;
    }
}

/*
 * lib$crc - Compute a cyclic redundancy check over a string.
 *
 * The CRC is accumulated a nibble at a time using the table built by
 * lib$crc_table, starting from the caller-supplied initial value.  The
 * computed CRC is returned as the function value (not a status code).
 */
uint32_t lib$crc(const uint32_t *table, const uint32_t *initial,
                 const struct dsc$descriptor_s *string)
{
    if (!table || !initial || !string)
        return 0;

    uint32_t crc = *initial;
    const uint8_t *p = (const uint8_t *)string->dsc$a_pointer;
    uint16_t len = string->dsc$w_length;

    for (uint16_t i = 0; i < len; i++) {
        crc = crc ^ p[i];
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
        crc = (crc >> 4) ^ table[crc & 0x0Fu];
    }
    return crc;
}
