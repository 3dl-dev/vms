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
 * Locate the HELP library on disk. Order (first hit wins):
 *   1. $OVMX_HELPLIB -- an explicit Linux path (locator override; content still
 *      comes from the real file). The OVMX analogue of VMS "HELP /LIBRARY=".
 *   2. SYS$HELP:HELPLIB.HLP translated through the logical-name tables.
 * Returns 1 and fills buf on success, 0 if no readable library was found.
 */
static int locate_library(char *buf, size_t bufsz)
{
    const char *env = getenv("OVMX_HELPLIB");
    if (env && env[0]) {
        FILE *fp = fopen(env, "r");
        if (fp) { fclose(fp); snprintf(buf, bufsz, "%s", env); return 1; }
    }

    char linux_path[1024];
    uint32_t st = vmsfs_to_linux_path(VMS_HELPLIB_PATH, linux_path,
                                      sizeof(linux_path));
    if ($VMS_STATUS_SUCCESS(st)) {
        FILE *fp = fopen(linux_path, "r");
        if (fp) { fclose(fp); snprintf(buf, bufsz, "%s", linux_path); return 1; }
    }
    return 0;
}

int main(int argc, char *argv[])
{
    /* Bootstrap the VMS namespace so SYS$HELP resolves. */
    vmsfs_device_add(SYSDISK_DEVICE, SYSDISK_MOUNT);
    lnm_setup_defaults(lnm_get_manager(), SYSDISK_MOUNT);

    char lib_path[1024];
    if (!locate_library(lib_path, sizeof(lib_path))) {
        /* Honest failure -- no compiled-in fallback (Rule 9 / INV-DCL). */
        fprintf(stderr,
                "%%HELP-E-OPENIN, error opening help library %s\n",
                VMS_HELPLIB_PATH);
        return 1;
    }

    help_lib_t *lib = help_open_file(lib_path);
    if (!lib) {
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
