/*
 * fal_main.c - SYS$SYSTEM:FAL.EXE, the DECnet File Access Listener network
 * server process (rd vms-d85). NETACP creates it with the AUTHENTICATED
 * user's UIC and default privileges for one inbound object-17 access; it
 * serves the DAP session over its two link mailboxes and exits. See
 * dnet_fal_proc.h. Like real VMS's FAL.EXE it is not a command a user runs.
 */
#include <stdio.h>

#include "dnet_fal_proc.h"

int main(void)
{
    uint32_t st = dnet_fal_proc_serve();
    return (st & 1) ? 0 : 1;
}
