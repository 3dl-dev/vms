
#define __NEW_STARLET 1

#include <stdio.h>
#include <ssdef.h>
#include <stsdef.h>
#include <starlet.h>
#include "errchk.h"

int main (void) {

static int r0_status;
static int req_bytes;
static char buffer[128];

    req_bytes = sizeof (buffer);
    r0_status = sys$get_entropy (buffer, req_bytes);
    errchk_sig (r0_status);
    (void)printf ("%i bytes of entropy fetched\n", req_bytes);

}
