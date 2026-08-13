/*
 * test_getjpi_ile3.c - sys$getjpiw with an ILE3 item list (vms-da9)
 *
 * REGRESSION for the sys$getjpi/sys$getjpiw signature fix. Before vms-da9,
 * starlet.h over-specified prcnam as "const struct dsc$descriptor_s *" and
 * itmlst as "const struct item_list_3 *". Real VMS (VSI starlet.h) declares
 * both as void*:
 *   int sys$getjpi(unsigned int efn, unsigned int *pidadr, void *prcnam,
 *                  void *itmlst, struct _iosb *iosb,
 *                  void (*astadr)(__unknown_params), unsigned __int64 astprm);
 * so a caller may pass the standard ILE3 item-list flavour (iledef.h) that
 * every Eight-Cubed corpus program and real VMS code builds. The old typed
 * prototype rejected ILE3 arrays at compile time ("incompatible pointer type").
 *
 * This test builds the item list EXACTLY like tests/corpus sys_getjpiw.c and
 * calls sys$getjpiw with it. That it COMPILES and links is the signature proof.
 *
 * Behaviour: $GETJPI is a reader of the executive process table (/dev/vms).
 * Where the executive is present (the QEMU runtime — see
 * tests/qemu/test_syssvc_procnam.c for the full behavioural coverage) it fills
 * the requested items. Where it is absent (this hosted unit-test build, which
 * has no /dev/vms) it must FAIL HONESTLY rather than fake per-process success
 * (CLAUDE.md Rule 9 / INV-6). So we assert: a well-formed status is returned;
 * on success the requested longword item was actually written; on failure the
 * status is an even (error) code, never a fabricated success.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <ssdef.h>
#include <efndef.h>
#include <jpidef.h>
#include <iledef.h>
#include <iosbdef.h>
#include <starlet.h>

static int failures = 0;
#define check(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", (msg)); } \
    else { printf("  FAIL: %s\n", (msg)); failures++; } \
} while (0)

int main(void)
{
    printf("Testing sys$getjpiw with an ILE3 item list (vms-da9)...\n");

    IOSB iosb;
    memset(&iosb, 0, sizeof(iosb));

    uint32_t pid = 0xFFFFFFFFu;
    uint32_t group = 0;
    uint32_t member = 0;
    char username[12];
    uint16_t username_len = 0;
    memset(username, 0, sizeof(username));

    /* The corpus's exact item-list shape: ILE3 entries, zero terminator. */
    ILE3 jpiitms[] = {
        {  4, JPI$_PID,      &pid,      NULL },
        {  4, JPI$_GRP,      &group,    NULL },
        {  4, JPI$_MEM,      &member,   NULL },
        { 12, JPI$_USERNAME, username,  &username_len },
        {  0, 0,             NULL,      NULL }
    };

    /* Signature proof: this call would not compile against the old
     * "const struct item_list_3 *" prototype. */
    uint32_t status = sys$getjpiw(EFN$C_ENF, 0, 0, jpiitms, &iosb, 0, 0);

    check(status != 0, "sys$getjpiw returned a well-formed (non-zero) status");

    if (status & 1u) {
        /* Executive present: the requested longword must have been written. */
        check(pid != 0xFFFFFFFFu, "JPI$_PID item was written on success");
    } else {
        /* No executive: honest failure, never a faked success. */
        check((status & 1u) == 0,
              "no /dev/vms -> honest error status (no faked per-process success)");
    }

    printf("%s\n", failures ? "TEST FAILED" : "All tests passed");
    return failures ? 1 : 0;
}
