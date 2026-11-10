/*
 * decnetd_retired.c - SYS$SYSTEM:DECNETD.EXE after rd vms-0a7 S2 + S3.
 *
 * DECNETD.EXE used to be every DECnet role behind argv flags. Each role is now
 * its own VMS image: NETACP.EXE (NCP SET EXECUTOR STATE ON creates it, no
 * command line), RTPAD.EXE ($ SET HOST), FAL.EXE (object 17, through SYS$NET),
 * RMS's own remote access (DIRECTORY/TYPE/COPY node::), and the self-tests in
 * the test image tests/vmsdecnet/netacp_test.c. Nothing activates DECNETD.EXE
 * any more; it stays on the system volume only until S4 removes the file and
 * the staging that still lists it. Run, it does no network work: it exits with
 * SS$_UNSUPPORTED, so DCL reports an honest failure -- never a NETACP
 * look-alike and never a plausible success.
 */
#include <stdint.h>
#include "starlet.h"
#include "ssdef.h"

int main(void)
{
    sys$exit(SS$_UNSUPPORTED);
    return 0;
}
