/*
 * vms_help.c - HELP.EXE, the standalone image form of the HELP utility.
 *
 * HELP is primarily a DCL built-in verb (src/vmsdcl: cmd_help -> dcl_help.c).
 * This image exists so the INSTALLED SYS$SYSTEM:HELP.EXE can be activated
 * directly from a target device -- the anti-LARP crux of the product-install
 * end-to-end gate (tests/qemu/test_product_install_e2e.sh runs
 * "RUNHELP MOUNT" against the just-installed image and requires real output).
 *
 * vms-01b: this used to be a 555-line ORPHANED reader that nothing dispatched
 * and that loaded a COMPILED-IN help string (a hardcoded facade). Both are
 * gone. It is now a thin wrapper over the single shared hierarchical HELP
 * engine (src/vmsdcl/dcl_help.c), reading the real library data from
 * SYS$HELP:HELPLIB.HLP -- no hardcoded topic content, one reader shared with
 * the DCL built-in.
 *
 * Clean-room provenance and the numbered-level .HLP format are documented in
 * src/vmsdcl/include/dcl/help.h (project Rule 8).
 *
 * Build: linked against libvmsfs + libvmslnm for VMS path/logical translation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>

#include "ovmx_layout.h"
#include "ssdef.h"
#include "vmsfs/device.h"
#include "vmsfs/filespec.h"
#include "vms/logical.h"
#include "dcl/help.h"

/*
 * Which HELP library to open. Order (first wins):
 *   1. $OVMX_HELPLIB -- an explicit Linux path (locator override; content still
 *      comes from the real file). The OVMX analogue of VMS "HELP /LIBRARY=".
 *   2. SYS$HELP:HELPLIB.HLP -- the VMS filespec, opened over the Files-11 ACP.
 * help_open_any() (dcl_help.c) routes a VMS spec to the ACP (with a POSIX /vms
 * fallback for host tooling) and a Linux path to a direct read (vms-4ac); the
 * pre-flip fopen(vmsfs_to_linux_path()) here read the retired /vms passthrough
 * and so answered %HELP-E-OPENIN on the runtime even though HELPLIB.HLP is
 * mastered on the volume.
 */
int main(int argc, char *argv[])
{
    /* Bootstrap the VMS namespace so SYS$HELP resolves. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    const char *env = getenv("OVMX_HELPLIB");
    const char *lib_spec = (env && env[0]) ? env : VMS_HELPLIB_PATH;

    help_lib_t *lib = help_open_any(lib_spec);
    if (!lib) {
        /* Honest failure -- no compiled-in fallback (Rule 9 / INV-DCL). */
        fprintf(stderr,
                "%%HELP-E-OPENIN, error opening help library %s\n",
                VMS_HELPLIB_PATH);
        return 1;
    }

    int rc = 0;
    if (argc > 1) {
        /* Non-interactive: HELP topic [subtopic ...] */
        const char *path[16];
        int n = 0;
        for (int i = 1; i < argc && n < 16; i++)
            path[n++] = argv[i];
        int st = help_render(lib, path, n, stdout);
        rc = $VMS_STATUS_SUCCESS(st) ? 0 : 1;
    } else {
        /* Interactive: Topic? / <path> Subtopic? loop from SYS$INPUT. */
        help_interactive(lib, NULL, 0, stdin, stdout);
    }

    help_close(lib);
    return rc;
}
