/*
 * test_putmsg_facnam.c - sys$putmsg honors the facnam facility-name override
 * (vms-7a2). Previously facnam was silently discarded ((void)facnam), so a
 * caller's override never took effect while $PUTMSG still returned SS$_NORMAL --
 * a facade-risk row in the compat register (sys-fao-msg). This proves the
 * %FACILITY token of the formatted message is rewritten to the caller's name,
 * with the rest of the message (severity, ident, text) preserved.
 *
 * Host unit test: sys$putmsg formats from the compiled-in message table and
 * calls the action routine with each formatted line -- no /dev/vms needed (it is
 * OVMX-USERSPACE). The action routine captures the line and returns non-zero so
 * $PUTMSG suppresses its own stderr write, keeping the test output clean.
 */
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"

static int pass = 0, fail = 0;
static void check(int cond, const char *name)
{
    if (cond) { printf("  PASS: %s\n", name); pass++; }
    else      { printf("  FAIL: %s\n", name); fail++; }
}

static char captured[512];
static uint32_t capture(struct dsc$descriptor_s *msg, uint32_t prm)
{
    (void)prm;
    uint16_t n = msg->dsc$w_length;
    if (n > (uint16_t)(sizeof(captured) - 1)) n = (uint16_t)(sizeof(captured) - 1);
    memcpy(captured, msg->dsc$a_pointer, n);
    captured[n] = '\0';
    return 1;   /* captured -> suppress $PUTMSG's own stderr write */
}

/* The tail of a "%FACILITY-..." message, starting at the first '-' (so the
 * severity/ident/text that must survive a facility override). */
static const char *tail_after_facility(const char *m)
{
    const char *dash = (m[0] == '%') ? strchr(m + 1, '-') : NULL;
    return dash ? dash : "";
}

int main(void)
{
    printf("=== test_putmsg_facnam (vms-7a2: sys$putmsg honors facnam) ===\n");

    /* arg_count = 1, msgid = SS$_NORMAL -> "%SYSTEM-S-NORMAL, ..." */
    uint32_t msgvec[2] = { 1, SS$_NORMAL };

    /* (1) no facnam -> the message's own facility. */
    captured[0] = '\0';
    sys$putmsg(msgvec, capture, NULL, 0);
    check(captured[0] == '%', "default: formatted message starts with '%'");
    char def_msg[512];
    strncpy(def_msg, captured, sizeof(def_msg) - 1);
    def_msg[sizeof(def_msg) - 1] = '\0';
    char def_tail[512];
    strncpy(def_tail, tail_after_facility(def_msg), sizeof(def_tail) - 1);
    def_tail[sizeof(def_tail) - 1] = '\0';
    check(strncmp(def_msg, "%MYAPP-", 7) != 0,
          "default: facility is NOT the override name");

    /* (2) facnam override -> only the %FACILITY token changes. */
    struct dsc$descriptor_s fac = { 5, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"MYAPP" };
    captured[0] = '\0';
    sys$putmsg(msgvec, capture, &fac, 0);
    check(strncmp(captured, "%MYAPP-", 7) == 0,
          "facnam override: %FACILITY rewritten to MYAPP (was silently discarded)");
    check(strcmp(tail_after_facility(captured), def_tail) == 0,
          "facnam override: severity/ident/text after the facility are preserved");

    /* (3) VMS facility names are upper case: a lower-case facnam is upcased. */
    struct dsc$descriptor_s facl = { 3, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)"abc" };
    captured[0] = '\0';
    sys$putmsg(msgvec, capture, &facl, 0);
    check(strncmp(captured, "%ABC-", 5) == 0,
          "facnam override is upper-cased (abc -> ABC)");

    printf("=== test_putmsg_facnam: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
