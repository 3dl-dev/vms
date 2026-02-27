/*
 * lib_bitops.c - LIB$ Bit and Arithmetic Interlocked Operations
 *
 * Implements atomic bit-manipulation and field-extraction routines:
 *
 *   LIB$ADAWI  - Add aligned word interlocked
 *   LIB$BBCCI  - Branch on Bit Clear and Clear Interlocked
 *   LIB$BBSSI  - Branch on Bit Set and Set Interlocked
 *   LIB$EXTV   - Extract signed bit field (variable position/size)
 *   LIB$EXTZV  - Extract unsigned (zero-extended) bit field
 *   LIB$INSV   - Insert bit field into target
 *
 * The "interlocked" operations use C11 stdatomic / gcc builtins to
 * achieve the atomicity that the VMS VAX instructions provided.
 * The bit-field operations (EXTV/EXTZV/INSV) treat the base address
 * as a flat byte array and extract/insert fields at arbitrary bit
 * positions spanning word boundaries.
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual,
 *            OpenVMS VAX Architecture Reference Manual
 */

#include <stdint.h>
#include <stdatomic.h>
#include "ssdef.h"
#include "lib$routines.h"

/*
 * lib$adawi - Add aligned word interlocked.
 *
 * Atomically adds *add to *sum (a signed 16-bit aligned word) and
 * stores the result back in *sum. Sets *sign to reflect the result:
 *   -1 if result < 0
 *    0 if result == 0
 *    1 if result > 0
 *
 * Returns SS$_NORMAL on success.
 *
 * Parameters:
 *   add  - Pointer to signed word addend
 *   sum  - Pointer to signed word accumulator (updated in place)
 *   sign - Pointer to signed word receiving sign of result
 */
uint32_t lib$adawi(const int16_t *add, int16_t *sum, int16_t *sign)
{
    if (!add || !sum || !sign) return SS$_BADPARAM;

    /*
     * Atomically fetch-and-add. Cast to _Atomic int16_t for the
     * operation; the result is the value after the addition.
     */
    _Atomic int16_t *atomic_sum = (_Atomic int16_t *)sum;
    int16_t result = atomic_fetch_add_explicit(atomic_sum, *add,
                                               memory_order_seq_cst) + *add;

    if (result < 0)
        *sign = -1;
    else if (result == 0)
        *sign = 0;
    else
        *sign = 1;

    return SS$_NORMAL;
}

/*
 * lib$bbcci - Branch on Bit Clear and Clear Interlocked.
 *
 * Atomically tests the bit at bit position *bit_position in the bit
 * array at *base_address, clears it, and returns the old value (1 if
 * it was set, 0 if it was clear).
 *
 * Bit position is absolute: bit N is in byte N/8, at bit offset N%8
 * within that byte.
 *
 * Parameters:
 *   bit_position  - Pointer to longword containing bit position
 *   base_address  - Pointer to start of bit array
 *
 * Returns: old value of the bit (1 or 0) -- not a VMS status code
 */
uint32_t lib$bbcci(const int32_t *bit_position, void *base_address)
{
    if (!bit_position || !base_address) return 0;

    int32_t pos = *bit_position;
    _Atomic unsigned char *byte_ptr =
        (_Atomic unsigned char *)((unsigned char *)base_address + pos / 8);
    unsigned char bit_mask = (unsigned char)(1u << (pos % 8));

    /* Atomically clear the bit and return its old value */
    unsigned char old = atomic_fetch_and_explicit(byte_ptr,
                                                  (unsigned char)~bit_mask,
                                                  memory_order_seq_cst);
    return (old & bit_mask) ? 1u : 0u;
}

/*
 * lib$bbssi - Branch on Bit Set and Set Interlocked.
 *
 * Atomically tests the bit at bit position *bit_position in the bit
 * array at *base_address, sets it, and returns the old value (1 if
 * it was set, 0 if it was clear).
 *
 * Parameters:
 *   bit_position  - Pointer to longword containing bit position
 *   base_address  - Pointer to start of bit array
 *
 * Returns: old value of the bit (1 or 0) -- not a VMS status code
 */
uint32_t lib$bbssi(const int32_t *bit_position, void *base_address)
{
    if (!bit_position || !base_address) return 0;

    int32_t pos = *bit_position;
    _Atomic unsigned char *byte_ptr =
        (_Atomic unsigned char *)((unsigned char *)base_address + pos / 8);
    unsigned char bit_mask = (unsigned char)(1u << (pos % 8));

    /* Atomically set the bit and return its old value */
    unsigned char old = atomic_fetch_or_explicit(byte_ptr, bit_mask,
                                                 memory_order_seq_cst);
    return (old & bit_mask) ? 1u : 0u;
}

/* ================================================================
 * Bit-field helpers (shared by EXTV, EXTZV, INSV)
 *
 * VMS bit-field operations address bits within a flat byte array.
 * Bit position 0 is the LSB of byte 0.  Bit position 32 is the LSB
 * of byte 4, etc.  Fields may span byte (and word) boundaries.
 * The maximum field size is 32 bits.
 * ================================================================ */

/*
 * Extract *size bits starting at bit *pos from the byte array at base.
 * Returns a uint32_t holding the raw (zero-extended) field value.
 */
static uint32_t bitfield_extract(const void *base, int32_t pos,
                                 uint8_t size)
{
    const unsigned char *p = (const unsigned char *)base;

    /* Handle zero-size case */
    if (size == 0) return 0;

    /* Caller must ensure base_address points to memory covering at least
     * (pos + size + 7) / 8 bytes from the base.  No bounds checking is
     * performed here — this matches the VMS lib$extv / lib$extzv API
     * contract which provides no extent parameter. */

    /* Collect up to 5 bytes spanning the field */
    int byte_start = pos / 8;
    int bit_offset = pos % 8;

    uint64_t accum = 0;
    int bytes_needed = (bit_offset + size + 7) / 8;
    if (bytes_needed > 5) bytes_needed = 5;

    for (int i = bytes_needed - 1; i >= 0; i--) {
        accum = (accum << 8) | (uint64_t)p[byte_start + i];
    }

    /* Shift right to align field at bit 0, then mask */
    accum >>= (unsigned)bit_offset;

    uint32_t mask = (size == 32) ? 0xFFFFFFFFu : (uint32_t)((1u << size) - 1u);
    return (uint32_t)(accum & (uint64_t)mask);
}

/*
 * lib$extv - Extract signed bit field.
 *
 * Extracts a *size-bit field starting at bit position *pos from the
 * array at base_address and sign-extends it to 32 bits.
 *
 * Parameters:
 *   pos          - Pointer to longword bit position (0-based)
 *   size         - Pointer to byte field size (0-32 bits)
 *   base_address - Pointer to start of bit array
 *
 * Returns: sign-extended field value (not a VMS status code)
 */
int32_t lib$extv(const int32_t *pos, const uint8_t *size,
                 const void *base_address)
{
    if (!pos || !size || !base_address) return 0;

    uint8_t sz = *size;
    if (sz == 0) return 0;
    if (sz > 32) sz = 32;

    uint32_t raw = bitfield_extract(base_address, *pos, sz);

    /* Sign-extend: if the top extracted bit is set, fill upper bits with 1s */
    if (sz < 32) {
        uint32_t sign_bit = 1u << (sz - 1u);
        if (raw & sign_bit) {
            raw |= ~((sign_bit << 1u) - 1u);  /* sign-extend */
        }
    }

    return (int32_t)raw;
}

/*
 * lib$extzv - Extract zero-extended (unsigned) bit field.
 *
 * Extracts a *size-bit field starting at bit position *pos from the
 * array at base_address and zero-extends it to 32 bits.
 *
 * Parameters:
 *   pos          - Pointer to longword bit position (0-based)
 *   size         - Pointer to byte field size (0-32 bits)
 *   base_address - Pointer to start of bit array
 *
 * Returns: zero-extended field value (not a VMS status code)
 */
uint32_t lib$extzv(const int32_t *pos, const uint8_t *size,
                   const void *base_address)
{
    if (!pos || !size || !base_address) return 0;

    uint8_t sz = *size;
    if (sz == 0) return 0;
    if (sz > 32) sz = 32;

    return bitfield_extract(base_address, *pos, sz);
}

/*
 * lib$insv - Insert bit field.
 *
 * Inserts the low *size bits of *source into the array at
 * base_address, starting at bit position *pos.
 *
 * Returns SS$_NORMAL.
 *
 * Parameters:
 *   source       - Pointer to longword whose low bits are inserted
 *   pos          - Pointer to longword bit position (0-based)
 *   size         - Pointer to byte field size (0-32 bits)
 *   base_address - Pointer to start of bit array (modified in place)
 */
uint32_t lib$insv(const int32_t *source, const int32_t *pos,
                  const uint8_t *size, void *base_address)
{
    if (!source || !pos || !size || !base_address) return SS$_BADPARAM;

    uint8_t sz = *size;
    if (sz == 0) return SS$_NORMAL;
    if (sz > 32) sz = 32;

    unsigned char *p = (unsigned char *)base_address;
    int32_t bit_pos = *pos;
    uint32_t val = (uint32_t)*source;

    /* Mask source value to field size */
    uint32_t field_mask = (sz == 32) ? 0xFFFFFFFFu : (uint32_t)((1u << sz) - 1u);
    val &= field_mask;

    /* Caller must ensure base_address points to writable memory covering at
     * least (*pos + *size + 7) / 8 bytes from the base.  No bounds checking
     * is performed here — this matches the VMS lib$insv API contract which
     * provides no extent parameter. */

    /* Insert bit-by-bit (simple, correct for all field sizes) */
    for (uint8_t i = 0; i < sz; i++) {
        int abs_bit = bit_pos + i;
        int byte_idx = abs_bit / 8;
        int bit_idx  = abs_bit % 8;

        if (val & (1u << i)) {
            p[byte_idx] |= (unsigned char)(1u << bit_idx);
        } else {
            p[byte_idx] &= (unsigned char)~(1u << bit_idx);
        }
    }

    return SS$_NORMAL;
}
