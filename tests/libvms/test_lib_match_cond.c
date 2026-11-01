/*
 * test_lib_match_cond.c - lib$match_cond: condition-code match, severity and
 * control bits ignored, 1-based index or 0.
 */
#include <stdio.h>
#include <stdint.h>
#include "lib$routines.h"
#include "ssdef.h"

static int failures = 0;
static void check(int cond, const char *name)
{
    printf("  %s: %s\n", cond ? "OK" : "FAIL", name);
    if (!cond) failures++;
}

int main(void)
{
    uint32_t unwind = SS$_UNWIND, abort_ = SS$_ABORT, unlocked = SS$_ACCVIO;
    uint32_t sig;

    printf("Testing lib$match_cond...\n");
    sig = SS$_ABORT;
    check(lib$match_cond(&sig, &unwind, &abort_, &unlocked) == 2, "second entry matches -> 2");
    sig = SS$_ACCVIO;
    check(lib$match_cond(&sig, &unwind, &abort_, &unlocked) == 3, "third entry matches -> 3");
    sig = SS$_UNWIND;
    check(lib$match_cond(&sig, &unwind) == 1, "single-entry list -> 1");
    sig = SS$_NORMAL;
    check(lib$match_cond(&sig, &unwind, &abort_, &unlocked) == 0, "no match -> 0");

    /* Severity (bits 0..2) is ignored: the same condition at another severity matches. */
    sig = (SS$_ABORT & ~7u) | 1u;
    check(lib$match_cond(&sig, &abort_) == 1, "same condition, different severity -> match");
    /* A different message number does not match. */
    sig = SS$_ABORT + 8u;
    check(lib$match_cond(&sig, &abort_) == 0, "different message number -> 0");
    check(lib$match_cond(NULL, &abort_) == 0, "NULL match value -> 0");

    printf("lib$match_cond: %d failure(s)\n", failures);
    return failures ? 1 : 0;
}
