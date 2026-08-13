/*
 * test_setddir.c - Unit tests for sys$setddir (rd vms-947).
 *
 * $SETDDIR reads and (optionally) changes the process default directory string
 * -- the [dir] used to resolve relative file specifications. OVMX stores it in
 * the PCB (pcb->default_dir), the same store sys$assign consults to resolve
 * SYS$DISK / relative specs (src/libvms/syssvc/sys_assign.c). These tests prove
 * a change made through the service is observable both through the service's own
 * old-directory out-arguments AND in that PCB store, so subsequent relative
 * filespec resolution sees it -- not a per-call no-op.
 *
 * Semantics derived ONLY from the OpenVMS System Services Reference ($SETDDIR)
 * and the calling convention exercised by the vendored MadGoat MMK
 * (tests/corpus/tier3-mmk/mmk.c), a real consumer. No VSI source consulted.
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "ssdef.h"
#include "descrip.h"
#include "vms/pcb.h"

/* sys$setddir is exported by libvms; declare it locally the way MMK does. */
uint32_t sys$setddir(const struct dsc$descriptor_s *new_dir,
                     unsigned short *old_len,
                     struct dsc$descriptor_s *old_dir);

static int failures = 0;

#define CHECK(cond, msg) do { \
    if (cond) { \
        printf("  PASS: %s\n", msg); \
    } else { \
        printf("  FAIL: %s\n", msg); \
        failures++; \
    } \
} while (0)

/* Build a fixed-length string descriptor over a NUL-terminated string. */
static void mkdesc(struct dsc$descriptor_s *d, char *s)
{
    d->dsc$w_length  = (uint16_t)strlen(s);
    d->dsc$b_dtype   = DSC$K_DTYPE_T;
    d->dsc$b_class   = DSC$K_CLASS_S;
    d->dsc$a_pointer = s;
}

int main(void)
{
    printf("test_setddir: sys$setddir (vms-947)\n");

    /* PCB starts with default_dir = "SYS$LOGIN:" (vms_pcb.c). */
    if (!vms_pcb_init(0)) {
        printf("  FAIL: vms_pcb_init returned NULL\n");
        return 1;
    }
    struct vms_pcb *pcb = vms_pcb_get();
    CHECK(pcb != NULL, "vms_pcb_get returns a PCB");
    CHECK(strcmp(pcb->default_dir, "SYS$LOGIN:") == 0,
          "initial default directory is SYS$LOGIN:");

    /* ---- Read-only call: new_dir == NULL returns the current directory. ---- */
    {
        char buf[256];
        unsigned short len = 0;
        struct dsc$descriptor_s old;
        memset(buf, 0, sizeof(buf));
        mkdesc(&old, buf);
        old.dsc$w_length = sizeof(buf);       /* buffer capacity */

        uint32_t st = sys$setddir(NULL, &len, &old);
        CHECK($VMS_STATUS_SUCCESS(st), "read-only call succeeds");
        buf[len] = '\0';
        CHECK(len == (unsigned short)strlen("SYS$LOGIN:"),
              "read-only returns previous length");
        CHECK(strcmp(buf, "SYS$LOGIN:") == 0,
              "read-only returns current directory string");
        /* A pure read must not mutate the store. */
        CHECK(strcmp(pcb->default_dir, "SYS$LOGIN:") == 0,
              "read-only call does not change the default directory");
    }

    /* ---- Set a bracketed directory: device is preserved (MMK's usage). ---- */
    {
        char newbuf[] = "[FOO.BAR]";
        char oldbuf[256];
        unsigned short oldlen = 0;
        struct dsc$descriptor_s nd, od;
        memset(oldbuf, 0, sizeof(oldbuf));
        mkdesc(&nd, newbuf);
        mkdesc(&od, oldbuf);
        od.dsc$w_length = sizeof(oldbuf);

        uint32_t st = sys$setddir(&nd, &oldlen, &od);
        CHECK($VMS_STATUS_SUCCESS(st), "set [FOO.BAR] succeeds");
        oldbuf[oldlen] = '\0';
        CHECK(strcmp(oldbuf, "SYS$LOGIN:") == 0,
              "set returns the PREVIOUS directory (SYS$LOGIN:)");
        /* Bracketed dir keeps the current device prefix. */
        CHECK(strcmp(pcb->default_dir, "SYS$LOGIN:[FOO.BAR]") == 0,
              "bracketed dir merged onto current device in PCB store");
    }

    /* ---- The change is observable by a subsequent read (round-trip). ---- */
    {
        char buf[256];
        unsigned short len = 0;
        struct dsc$descriptor_s old;
        memset(buf, 0, sizeof(buf));
        mkdesc(&old, buf);
        old.dsc$w_length = sizeof(buf);

        uint32_t st = sys$setddir(NULL, &len, &old);
        buf[len] = '\0';
        CHECK($VMS_STATUS_SUCCESS(st), "second read-only call succeeds");
        CHECK(strcmp(buf, "SYS$LOGIN:[FOO.BAR]") == 0,
              "subsequent read observes the new default directory");
    }

    /* ---- Full device:[dir] spec is stored verbatim. ---- */
    {
        char newbuf[] = "DKA100:[SYS0.SYSMGR]";
        struct dsc$descriptor_s nd;
        mkdesc(&nd, newbuf);

        /* Set-only form: old_len / old_dir omitted (MMK's restore path). */
        uint32_t st = sys$setddir(&nd, NULL, NULL);
        CHECK($VMS_STATUS_SUCCESS(st), "set full spec (set-only form) succeeds");
        CHECK(strcmp(pcb->default_dir, "DKA100:[SYS0.SYSMGR]") == 0,
              "full device:[dir] spec stored verbatim in PCB store");
    }

    /* ---- old_len without old_dir reports the full current length. ---- */
    {
        unsigned short len = 0;
        uint32_t st = sys$setddir(NULL, &len, NULL);
        CHECK($VMS_STATUS_SUCCESS(st), "read-only, len-only call succeeds");
        CHECK(len == (unsigned short)strlen("DKA100:[SYS0.SYSMGR]"),
              "len-only reports full current directory length");
    }

    /* ---- Error: empty new directory string -> SS$_BADPARAM. ---- */
    {
        char before[256];
        strncpy(before, pcb->default_dir, sizeof(before) - 1);
        before[sizeof(before) - 1] = '\0';

        char empty[] = "";
        struct dsc$descriptor_s nd;
        mkdesc(&nd, empty);           /* dsc$w_length == 0 */

        uint32_t st = sys$setddir(&nd, NULL, NULL);
        CHECK(st == SS$_BADPARAM, "empty new directory returns SS$_BADPARAM");
        CHECK(strcmp(pcb->default_dir, before) == 0,
              "rejected empty spec leaves the default directory unchanged");
    }

    /* ---- Error: over-long new directory string -> SS$_BADPARAM. ---- */
    {
        char before[256];
        strncpy(before, pcb->default_dir, sizeof(before) - 1);
        before[sizeof(before) - 1] = '\0';

        static char big[400];
        memset(big, 'A', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        struct dsc$descriptor_s nd;
        mkdesc(&nd, big);

        uint32_t st = sys$setddir(&nd, NULL, NULL);
        CHECK(st == SS$_BADPARAM, "over-long new directory returns SS$_BADPARAM");
        CHECK(strcmp(pcb->default_dir, before) == 0,
              "rejected over-long spec leaves the default directory unchanged");
    }

    printf("test_setddir: %s (%d failure%s)\n",
           failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
