/*
 * fal_main.c - SYS$SYSTEM:FAL.EXE, the DECnet File Access Listener network
 * server process (rd vms-d85; SYS$NET, rd vms-0a7 S3). NETACP creates it with
 * the AUTHENTICATED user's UIC and default privileges for one inbound
 * object-17 access, its SYS$NET naming the waiting link; it $ASSIGNs _NET:,
 * accepts the link with IO$_ACCESS, serves the DAP session over $QIOW on the
 * channel and exits. See dnet_fal_proc.h. Like real VMS's FAL.EXE it is not a
 * command a user runs: without a SYS$NET it exits SS$_NOLOGNAM.
 */
#include "dnet_fal_proc.h"

int main(void)
{
    uint32_t st = dnet_fal_net_serve();
    return (st & 1) ? 0 : 1;
}
