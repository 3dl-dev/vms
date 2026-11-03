/*
 * rms_read_records.c - the host DCL harness's reader for a SYS$OUTPUT /
 * SYS$ERROR file (rd vms-a6ce).
 *
 * The harness (run_dcl_tests.sh via dcl_rms_run.sh) starts DCL with its
 * process-permanent output files naming a disk file, so every line DCL writes
 * is a record DCL $PUTs there through RMS (dcl_out.c's file route). This tool
 * reads that file back the VMS way -- RMS $OPEN, $CONNECT and one $GET per
 * record (src/vmsrms) -- and hands the harness each record as one line, so the
 * tests' EXPECT/EXPECT_NOT assertions see exactly the records the file holds.
 * It never parses the file's bytes itself: a record is what $GET returns.
 *
 * Usage: rms_read_records <vms-filespec>
 * Exit 0 after RMS$_EOF; 2 when the file cannot be $OPENed (the RMS status is
 * reported: no records were written there, which a test must see as a
 * failure, not as empty output); 3 when a $GET fails part way.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rms/rms.h"
#include "vms/logical.h"
#include "vmsfs/device.h"
#include "ovmx_layout.h"

int main(int argc, char **argv)
{
    static char rbuf[32768];
    struct FAB fab;
    struct RAB rab;
    uint32_t st;

    if (argc != 2) {
        fprintf(stderr, "usage: rms_read_records <vms-filespec>\n");
        return 2;
    }

    /* The system volume this process sees, set up exactly as DCL's session
     * does (dcl_main.c setup_session): SYS$SYSDEVICE is the volume at
     * VMS_ROOT, so the spec DCL $PUT its records to names the same file here. */
    {
        const char *vms_root = getenv("VMS_ROOT");
        if (!vms_root)
            vms_root = SYSDISK_MOUNT;
        lnm_manager_t *mgr = lnm_get_manager();
        if (mgr) {
            vmsfs_device_add(SYSDISK_DEVICE, vms_root);
            lnm_setup_defaults(mgr, vms_root);
        }
    }

    fab = cc$rms_fab;
    fab.fab$l_fna = argv[1];
    fab.fab$b_fns = (uint8_t)strlen(argv[1]);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_fac = FAB$M_GET;
    fab.fab$b_shr = FAB$M_SHRGET;
    {
        /* frame the records as the file's header says they were written */
        struct rms_fileattr fa;
        if (rms_file_attr(argv[1], &fa) == RMS$_NORMAL &&
            fa.rfm >= FAB$C_FIX && fa.rfm <= FAB$C_STMCR) {
            fab.fab$b_rfm = fa.rfm;
            if (fa.mrs)
                fab.fab$w_mrs = fa.mrs;
        }
    }
    st = sys$open(&fab, 0, 0);
    if (st != RMS$_NORMAL) {
        fprintf(stderr, "rms_read_records: $OPEN %s: status %%X%08X\n",
                argv[1], (unsigned)st);
        return 2;
    }

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    rab.rab$l_ubf = rbuf;
    rab.rab$w_usz = sizeof rbuf;
    st = sys$connect(&rab, 0, 0);
    if (st != RMS$_NORMAL) {
        fprintf(stderr, "rms_read_records: $CONNECT %s: status %%X%08X\n",
                argv[1], (unsigned)st);
        sys$close(&fab, 0, 0);
        return 2;
    }

    int rc = 0;
    for (;;) {
        st = sys$get(&rab, 0, 0);
        if (st == RMS$_EOF)
            break;
        if (st != RMS$_NORMAL) {
            fprintf(stderr, "rms_read_records: $GET %s: status %%X%08X\n",
                    argv[1], (unsigned)st);
            rc = 3;
            break;
        }
        fwrite(rab.rab$l_rbf, 1, rab.rab$w_rsz, stdout);
        fputc('\n', stdout);
    }
    sys$close(&fab, 0, 0);
    return rc;
}
