/*
 * SHOW DEVICE <prefix> matches the device name proper, past a node or
 * allocation-class prefix (rd vms-582). Seen in lab pod lab5fa: OVMX answered
 * `SHOW DEVICE D` with %SYSTEM-W-NOSUCHDEV while its device table held the
 * MSCP-served VAX1$DUA16384..7, because the prefix was compared against the
 * whole node-prefixed name. Hermetic: exercises dcl/devmatch.h directly.
 */
#include <stdio.h>
#include <string.h>
#include "dcl/devmatch.h"

static int fails;

static void expect(const char *devnam, const char *prefix, int want)
{
    int got = dcl_devnam_prefix_match(devnam, prefix, strlen(prefix));
    if (got != want) {
        printf("FAIL: match(\"%s\", \"%s\") = %d, want %d\n",
               devnam, prefix, got, want);
        fails++;
    } else {
        printf("ok:   match(\"%s\", \"%s\") = %d\n", devnam, prefix, got);
    }
}

int main(void)
{
    /* the lab5fa case: node-prefixed MSCP-served disk */
    expect("VAX1$DUA16386", "D", 1);
    expect("VAX1$DUA16386", "DUA", 1);
    expect("VAX1$DUA16386", "DK", 0);
    /* allocation-class prefix */
    expect("$2$DUA2", "D", 1);
    expect("$2$DUA2", "DUA2", 1);
    expect("_$2$DUA2", "DU", 1);
    /* local, unprefixed names keep matching as before (vms-bf0) */
    expect("OPA0", "O", 1);
    expect("VDA0", "V", 1);
    expect("VDA0", "D", 0);
    expect("OPA0", "ZZ", 0);
    /* the node prefix is never what a '$'-less prefix matches */
    expect("VAX1$DUA2", "V", 0);
    /* a prefix carrying '$' is matched against the whole name */
    expect("VAX1$DUA2", "VAX1$D", 1);
    expect("VAX2$DUA2", "VAX1$D", 0);
    expect("$2$DUA2", "$2$", 1);
    expect("_VAX1$DUA2", "_VAX1$DUA", 1);

    if (fails) {
        printf("%d FAILED\n", fails);
        return 1;
    }
    printf("all passed\n");
    return 0;
}
