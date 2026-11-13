/*
 * test_vmsabi_dsc.c - a VMS-ABI entry point reads a string descriptor of
 * either form and gets the same answer (vms-7b3).
 *
 * The VMS-ABI entries (src/libvms/syssvc/sys_vmsabi.c: STR$PREFIX,
 * STR$COPY_DX, LIB$GET_FOREIGN, ...) read every descriptor through
 * vmsabi_dsc.h. This test lays each descriptor out byte for byte as a caller
 * does -- the 8-byte 32-bit form a DEC C program at the default pointer size
 * passes, and the 24-byte DSC64 form -- and checks that both read as the same
 * string and that a class-D result written back lands in the caller's own
 * fields. It also checks the cases that must NOT read as DSC64: a 32-bit
 * descriptor whose length is 1 (the MBO value) with an ordinary pointer, and
 * one whose pointer longword is -1 but whose length is not 1.
 *
 * The negative control tests/libvms/negctl_vmsabi_dsc64.sh rebuilds this
 * test with the DSC64 recognition broken and requires it to fail here.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "vmsabi_dsc.h"

static int fails;

#define CHECK(cond, what)                                                     \
    do {                                                                      \
        if (cond) printf("ok: %s\n", what);                                   \
        else { printf("FAIL: %s\n", what); fails++; }                         \
    } while (0)

/* The 32-bit form, as a 32-bit DEC C caller lays it out. */
static void mk32(unsigned char b[8], uint16_t len, uint8_t dt, uint8_t cl, uint32_t ptr)
{
    memset(b, 0, 8);
    memcpy(b, &len, 2);
    b[2] = dt;
    b[3] = cl;
    memcpy(b + 4, &ptr, 4);
}

/* The DSC64 form: MBO 1, MBMO -1, quadword length and pointer. */
static void mk64(unsigned char b[24], uint64_t len, uint8_t dt, uint8_t cl, uint64_t ptr)
{
    uint16_t mbo = 1;
    int32_t mbmo = -1;
    memset(b, 0, 24);
    memcpy(b, &mbo, 2);
    b[2] = dt;
    b[3] = cl;
    memcpy(b + 4, &mbmo, 4);
    memcpy(b + 8, &len, 8);
    memcpy(b + 16, &ptr, 8);
}

int main(void)
{
    const uint8_t DT_T = 14, CL_S = 1, CL_D = 2;   /* DSC$K_DTYPE_T, CLASS_S/_D */
    unsigned char d32[8], d64[24];
    struct ovmx_vmsabi_dsc a, b;

    /* 1. The same string, both forms. */
    mk32(d32, 6, DT_T, CL_S, 0x00012340u);
    mk64(d64, 6, DT_T, CL_S, 0x00012340u);
    ovmx_vmsabi_dsc_read(d32, &a);
    ovmx_vmsabi_dsc_read(d64, &b);
    CHECK(!a.is64, "32-bit form reads as the 32-bit form");
    CHECK(b.is64, "DSC64 form (MBO=1, MBMO=-1) reads as DSC64");
    CHECK(a.len == 6 && b.len == 6, "both forms read length 6");
    CHECK(a.ptr == 0x12340u && b.ptr == 0x12340u, "both forms read the same address");
    CHECK(a.dtype == DT_T && b.dtype == DT_T && a.cls == CL_S && b.cls == CL_S,
          "both forms read DTYPE_T / CLASS_S");

    /* 2. A 32-bit form whose length is 1 -- the MBO value -- is still the
     *    32-bit form: its pointer longword is not -1. */
    mk32(d32, 1, DT_T, CL_S, 0x00020000u);
    ovmx_vmsabi_dsc_read(d32, &a);
    CHECK(!a.is64 && a.len == 1 && a.ptr == 0x20000u,
          "length-1 32-bit descriptor is not taken for DSC64");

    /* 3. Pointer longword -1 with a length other than 1: the 32-bit form. */
    mk32(d32, 2, DT_T, CL_S, 0xFFFFFFFFu);
    CHECK(!ovmx_vmsabi_is_dsc64(d32), "MBMO alone (length 2) is not DSC64");

    /* 4. A 32-bit address sign-extends, as an Alpha longword load does. */
    mk32(d32, 3, DT_T, CL_S, 0x80001000u);
    ovmx_vmsabi_dsc_read(d32, &a);
    CHECK(a.ptr == 0xFFFFFFFF80001000ull, "32-bit address sign-extends to 64 bits");

    /* 5. A class-D result written back into each form. */
    mk32(d32, 0, DT_T, CL_D, 0);
    mk64(d64, 0, DT_T, CL_D, 0);
    ovmx_vmsabi_dsc_write(d32, 13, 0x00034560u);
    ovmx_vmsabi_dsc_write(d64, 13, 0x00034560u);
    uint16_t l16; uint32_t p32; uint64_t l64, p64; uint16_t mbo; int32_t mbmo;
    memcpy(&l16, d32, 2); memcpy(&p32, d32 + 4, 4);
    CHECK(l16 == 13 && p32 == 0x34560u && d32[3] == CL_D,
          "class-D result lands in the 32-bit form's word length and longword pointer");
    memcpy(&mbo, d64, 2); memcpy(&mbmo, d64 + 4, 4);
    memcpy(&l64, d64 + 8, 8); memcpy(&p64, d64 + 16, 8);
    CHECK(mbo == 1 && mbmo == -1 && l64 == 13 && p64 == 0x34560u,
          "class-D result lands in the DSC64's quadwords and keeps MBO/MBMO");
    ovmx_vmsabi_dsc_read(d32, &a);
    ovmx_vmsabi_dsc_read(d64, &b);
    CHECK(a.len == b.len && a.ptr == b.ptr, "both forms read back the same result");

    if (fails) {
        printf("test_vmsabi_dsc: %d FAILED\n", fails);
        return 1;
    }
    printf("test_vmsabi_dsc: all passed\n");
    return 0;
}
