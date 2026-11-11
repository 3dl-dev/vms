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
#include "starlet.h"
#include "descrip.h"
#include "iodef.h"

/*
 * The interactive "Topic?" line, read the VMS way: a channel to SYS$INPUT
 * ($ASSIGN) and $QIOW IO$_READPROMPT, the prompt riding the read (libc-drop
 * vms-3381: the engine reads no stream itself). No channel -- no executive
 * terminal -- is end of input, an honest one: HELP shows what it was asked
 * and exits.
 */
static int help_qio_read(const char *prompt, char *buf, size_t bufsz)
{
    static uint16_t chan;
    static int state;                  /* 0 untried, 1 assigned, -1 none */
    static const char in[] = "SYS$INPUT";
    uint16_t iosb[4];
    uint32_t st;
    size_t plen = prompt ? strlen(prompt) : 0;

    if (state == 0) {
        struct dsc$descriptor_s d = { sizeof in - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                      (char *)in };
        state = (sys$assign(&d, &chan, 0, NULL, 0) & 1) ? 1 : -1;
    }
    if (state != 1 || !buf || bufsz < 2)
        return -1;
    fflush(stdout);
    memset(iosb, 0, sizeof iosb);
    st = sys$qiow(0, chan, plen ? IO$_READPROMPT : IO$_READVBLK, iosb, NULL, 0,
                  buf, (uint32_t)(bufsz - 1), 0, 0, (uintptr_t)prompt, (uint32_t)plen);
    if (st & 1)
        st = iosb[0];
    if (!(st & 1))
        return -1;
    size_t n = iosb[1] < bufsz - 1 ? iosb[1] : bufsz - 1;
    buf[n] = '\0';
    if (iosb[2] == 26 && n == 0)
        return -1;                     /* CTRL/Z */
    return 0;
}

/*
 * Which HELP library to open. Order (first wins):
 *   1. $OVMX_HELPLIB -- an explicit Linux path (locator override; content still
 *      comes from the real file). The OVMX analogue of VMS "HELP /LIBRARY=".
 *   2. SYS$HELP:HELPLIB.HLP -- the VMS filespec, opened over the Files-11 ACP.
 * help_open_any() (dcl_help.c) reads the library through RMS only -- $GET for
 * a .HLP, block $READ for a .HLB (dcl_help_acp.c, vms-3381); the pre-flip
 * fopen(vmsfs_to_linux_path()) here read the retired /vms passthrough and so
 * answered %HELP-E-OPENIN on the runtime even though HELPLIB.HLP is mastered
 * on the volume.
 */
/* HELP.EXE still writes its display to stdio (a libc-drop of its own, not
 * DCL's: DCL's HELP verb renders into SYS$OUTPUT's sink, rd vms-d1a) */
static int file_write(void *arg, const char *p, size_t n)
{
    return fwrite(p, 1, n, (FILE *)arg) == n ? 0 : -1;
}

static int file_flush(void *arg) { return fflush((FILE *)arg); }

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
        const help_out_t out = { file_write, file_flush, stdout };
        int st = help_render(lib, path, n, &out);
        rc = $VMS_STATUS_SUCCESS(st) ? 0 : 1;
    } else {
        /* Interactive: Topic? / <path> Subtopic? loop from SYS$INPUT. */
        const help_out_t out = { file_write, file_flush, stdout };
        help_set_reader(help_qio_read);
        help_interactive(lib, NULL, 0, &out);
        help_set_reader(NULL);
    }

    help_close(lib);
    return rc;
}
