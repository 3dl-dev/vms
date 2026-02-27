/*
 * test_lib_vm.c - Unit tests for lib$get_vm / lib$free_vm / lib$create_vm_zone / lib$delete_vm_zone
 *
 * Tests the VMS zone-based memory allocator.
 *
 * IMPORTANT: lib$get_vm and lib$free_vm take an optional third argument
 * (zone_id pointer).  On AArch64, calling a varargs function without
 * supplying the optional argument causes va_arg to read garbage from the
 * FP register save area.  Always pass a NULL or valid pointer explicitly.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
/* lib$routines.h defines LIB$_BADZONE = 0x008001E4, which matches what
 * the compiled library actually returns.  libdef.h has a different value
 * (0x0015806A) that is NOT what lib_vm.c uses at runtime. */
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

/* Use the default zone (zone_id == 0).
 * Pass NULL as the optional zone_id pointer so va_arg reads a valid value.
 */
static uint32_t get_vm_default(uint32_t size, void **ptr)
{
    return lib$get_vm(&size, ptr, (uint32_t *)NULL);
}

static uint32_t free_vm_default(uint32_t size, void **ptr)
{
    return lib$free_vm(&size, ptr, (uint32_t *)NULL);
}

/* ------------------------------------------------------------------ */
/* lib$get_vm / lib$free_vm — default zone                            */
/* ------------------------------------------------------------------ */
static void test_basic_alloc_free(void)
{
    printf("Testing lib$get_vm / lib$free_vm (default zone)...\n");
    void *ptr = NULL;

    uint32_t st = get_vm_default(64, &ptr);
    check(st == SS$_NORMAL, "lib$get_vm returns SS$_NORMAL");
    check(ptr != NULL, "lib$get_vm sets non-null pointer");

    if (ptr) {
        /* Write to the allocation to verify it's usable memory */
        memset(ptr, 0xAB, 64);
        check(*(uint8_t *)ptr == 0xAB, "allocated memory is writable");
    }

    st = free_vm_default(64, &ptr);
    check(st == SS$_NORMAL, "lib$free_vm returns SS$_NORMAL");
    check(ptr == NULL, "lib$free_vm clears pointer to NULL");
}

/* ------------------------------------------------------------------ */
/* Multiple small allocations — lookaside list exercise               */
/* ------------------------------------------------------------------ */
static void test_multiple_small(void)
{
    printf("Testing multiple small allocations...\n");
#define N_ALLOCS 8
    void *ptrs[N_ALLOCS];
    memset(ptrs, 0, sizeof(ptrs));

    for (int i = 0; i < N_ALLOCS; i++) {
        uint32_t st = get_vm_default(32, &ptrs[i]);
        check(st == SS$_NORMAL, "get_vm small ok");
        if (ptrs[i])
            memset(ptrs[i], (uint8_t)i, 32);
    }

    /* Verify each allocation has its own data */
    int distinct = 1;
    for (int i = 0; i < N_ALLOCS; i++) {
        if (!ptrs[i]) { distinct = 0; break; }
        if (*(uint8_t *)ptrs[i] != (uint8_t)i) { distinct = 0; break; }
    }
    check(distinct, "small allocations are distinct");

    for (int i = 0; i < N_ALLOCS; i++) {
        if (ptrs[i])
            free_vm_default(32, &ptrs[i]);
    }
#undef N_ALLOCS
}

/* ------------------------------------------------------------------ */
/* Large allocation (> 2048 bytes) — own mmap region                  */
/* ------------------------------------------------------------------ */
static void test_large_alloc(void)
{
    printf("Testing large allocation (4096 bytes)...\n");
    void *ptr = NULL;

    uint32_t st = get_vm_default(4096, &ptr);
    check(st == SS$_NORMAL, "large alloc returns SS$_NORMAL");
    check(ptr != NULL, "large alloc pointer non-null");

    if (ptr) {
        memset(ptr, 0xCC, 4096);
        check(*(uint8_t *)ptr == 0xCC, "large allocation is writable");
        free_vm_default(4096, &ptr);
    }
}

/* ------------------------------------------------------------------ */
/* lib$create_vm_zone / lib$delete_vm_zone                            */
/* ------------------------------------------------------------------ */
static void test_zone_create_delete(void)
{
    printf("Testing lib$create_vm_zone / lib$delete_vm_zone...\n");
    uint32_t zone_id = 0;

    uint32_t st = lib$create_vm_zone(&zone_id);
    check(st == SS$_NORMAL, "create_vm_zone returns SS$_NORMAL");
    check(zone_id != 0, "zone_id is non-zero (not default zone)");

    /* Allocate from the custom zone — pass zone_id explicitly */
    void *ptr = NULL;
    uint32_t size = 128;
    st = lib$get_vm(&size, &ptr, &zone_id);
    check(st == SS$_NORMAL, "get_vm from custom zone");
    check(ptr != NULL, "custom zone allocation non-null");

    if (ptr) {
        memset(ptr, 0x55, 128);
        check(*(uint8_t *)ptr == 0x55, "custom zone memory writable");
    }

    /* Delete zone — bulk frees all allocations */
    st = lib$delete_vm_zone(&zone_id);
    check(st == SS$_NORMAL, "delete_vm_zone returns SS$_NORMAL");
}

/* ------------------------------------------------------------------ */
/* Cannot delete default zone (zone_id == 0)                          */
/* ------------------------------------------------------------------ */
static void test_cannot_delete_default_zone(void)
{
    printf("Testing that default zone (0) cannot be deleted...\n");
    uint32_t zero = 0;
    uint32_t st = lib$delete_vm_zone(&zero);
    check(st == LIB$_BADZONE, "delete zone 0 returns LIB$_BADZONE");
}

/* ------------------------------------------------------------------ */
/* Invalid zone ID (out of range or inactive)                         */
/* ------------------------------------------------------------------ */
static void test_invalid_zone(void)
{
    printf("Testing invalid zone ID handling...\n");
    uint32_t bad_zone = 99;  /* Never created */
    uint32_t size = 64;
    void *ptr = NULL;

    uint32_t st = lib$get_vm(&size, &ptr, &bad_zone);
    check(st == LIB$_BADZONE, "get_vm with bad zone returns LIB$_BADZONE");
    check(ptr == NULL, "ptr remains NULL on bad zone");
}

/* ------------------------------------------------------------------ */
/* Null parameter checks                                               */
/* ------------------------------------------------------------------ */
static void test_null_params(void)
{
    printf("Testing null parameter handling...\n");
    void *ptr = NULL;
    uint32_t size = 64;

    uint32_t st = lib$get_vm(NULL, &ptr, (uint32_t *)NULL);
    check(st == SS$_BADPARAM, "get_vm null size returns SS$_BADPARAM");

    st = lib$get_vm(&size, NULL, (uint32_t *)NULL);
    check(st == SS$_BADPARAM, "get_vm null ptr returns SS$_BADPARAM");

    st = lib$create_vm_zone(NULL);
    check(st == SS$_BADPARAM, "create_vm_zone null zone_id returns SS$_BADPARAM");

    st = lib$delete_vm_zone(NULL);
    check(st == SS$_BADPARAM, "delete_vm_zone null zone_id returns SS$_BADPARAM");
}

/* ------------------------------------------------------------------ */
/* lib$get_vm_page / lib$free_vm_page                                 */
/* ------------------------------------------------------------------ */
static void test_vm_page(void)
{
    printf("Testing lib$get_vm_page / lib$free_vm_page...\n");
    uint32_t pages = 2;  /* 2 * 512 = 1024 bytes */
    void *ptr = NULL;

    uint32_t st = lib$get_vm_page(&pages, &ptr);
    check(st == SS$_NORMAL, "get_vm_page returns SS$_NORMAL");
    check(ptr != NULL, "get_vm_page pointer non-null");

    if (ptr) {
        memset(ptr, 0x77, 1024);
        check(*(uint8_t *)ptr == 0x77, "page allocation is writable");
        lib$free_vm_page(&pages, &ptr);
    }
}

/* ------------------------------------------------------------------ */
/* Alloc/free cycle using lookaside recycling                         */
/* ------------------------------------------------------------------ */
static void test_lookaside_recycle(void)
{
    printf("Testing lookaside list recycling...\n");
    void *ptr1 = NULL, *ptr2 = NULL;

    /* Allocate a 64-byte block and free it — goes to lookaside bin */
    uint32_t st = get_vm_default(64, &ptr1);
    check(st == SS$_NORMAL, "first alloc ok");

    if (ptr1) {
        *(uint8_t *)ptr1 = 0xDE;
        st = free_vm_default(64, &ptr1);
        check(st == SS$_NORMAL, "free ok (goes to lookaside)");
        check(ptr1 == NULL, "ptr1 cleared");
    }

    /* Second alloc of same size should reuse lookaside block */
    st = get_vm_default(64, &ptr2);
    check(st == SS$_NORMAL, "second alloc from lookaside ok");
    check(ptr2 != NULL, "lookaside ptr non-null");

    if (ptr2) {
        memset(ptr2, 0xAA, 64);
        check(*(uint8_t *)ptr2 == 0xAA, "recycled block is writable");
        free_vm_default(64, &ptr2);
    }
}

int main(void)
{
    printf("=== test_lib_vm: lib$get_vm / lib$free_vm / zone ops ===\n");

    test_basic_alloc_free();
    test_multiple_small();
    test_large_alloc();
    test_zone_create_delete();
    test_cannot_delete_default_zone();
    test_invalid_zone();
    test_null_params();
    test_vm_page();
    test_lookaside_recycle();

    if (failures == 0)
        printf("All lib_vm tests passed.\n");
    else
        printf("FAILED: %d test(s) failed.\n", failures);

    return failures;
}
