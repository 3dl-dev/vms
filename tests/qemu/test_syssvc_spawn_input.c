/*
 * test_syssvc_spawn_input.c - LIB$SPAWN's INPUT file is read through RMS over the ACP, and a
 * missing one fails honestly (vms-ccc).
 *
 * THE BUG. lib$spawn translated an INPUT=<filespec> to a Linux path (a leftover of the retired /vms
 * passthrough). For a file on the ODS-2 volume that path does not exist; $CREPRC's child ignored the
 * failed open(), kept the creator's /dev/null as SYS$INPUT, and DCL read EOF and exited 0 at once --
 * the command file never ran, and nothing said so.
 *
 * THE PROPERTY.
 *   1. A command file laid down on the ODS-2 volume through RMS, named as a VMS filespec, is what the
 *      spawned DCL executes (its WRITE SYS$OUTPUT lands in the output file).
 *   2. An INPUT spec that no file answers to is RMS$_FNF from lib$spawn, with no process created --
 *      never a subprocess quietly reading nothing.
 * (1) can fail against the old code (the DCL runs nothing, so the marker never appears).
 *
 * Requires a real, insmod'd vms.ko; without one it SKIPs (77), never a fake pass.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdint.h>
#include <poll.h>

#include "ssdef.h"
#include "rmsdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "clidef.h"
#include "starlet.h"
#include "rms_textfile.h"
#include "vms_kif.h"
#include "vms/logical.h"
#include "vmsfs/filespec.h"

#define EXIT_SKIP 77
#define WAIT_MS 30000
#define EFN 12
#define COM_SPEC "SYS$SYSROOT:[SYSMGR]SPIN_INPUT.COM"
#define OUT_PATH "/tmp/spin_input.out"
#define MARKER "SPIN_INPUT_RAN_OK"

static int pass, fail;
#define CHECK(c, m) do { if (c) { printf("  PASS: %s\n", m); pass++; } \
                         else   { printf("  FAIL: %s\n", m); fail++; } } while (0)

static struct dsc$descriptor_s dsc(const char *s)
{
    struct dsc$descriptor_s d = { (unsigned short)strlen(s), DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)s };
    return d;
}

int main(void)
{
    printf("=== test_syssvc_spawn_input (vms-ccc: SYS$INPUT through RMS, honest FNF) ===\n");
    int devfd = open("/dev/vms", O_RDWR);
    if (devfd < 0) { printf("  SKIP: no /dev/vms\n"); return EXIT_SKIP; }
    close(devfd);
    if (access("/bin/DCL.EXE", X_OK) != 0) { printf("  SKIP: no /bin/DCL.EXE staged\n"); return EXIT_SKIP; }
    lnm_manager_t *mgr = lnm_get_manager();
    if (!mgr) { printf("  SKIP: no lnm manager\n"); return EXIT_SKIP; }
    CHECK(lnm_create(mgr, LNM_PROCESS_TABLE, "SYS$SYSTEM", "/bin", 0, LNM_MODE_SUPER) & 1,
          "define SYS$SYSTEM -> /bin (stage DCL.EXE for lib$spawn)");
    if (!(vms_kif_open() >= 0 && (vms_kif_register(NULL) & 1))) { printf("  FAIL: register\n"); return 1; }

    /* (1) the command file is an ODS-2 file made through RMS, not a Linux file */
    CHECK(rms_textfile_write_line(COM_SPEC, "$ WRITE SYS$OUTPUT \"" MARKER "\"") == 0,
          "a command file is laid down on the ODS-2 volume through RMS");
    unlink(OUT_PATH);
    struct dsc$descriptor_s in = dsc(COM_SPEC), out = dsc(OUT_PATH);
    uint32_t flags = CLI$M_NOWAIT, efn = EFN, pid = 0;
    (void)sys$clref(efn);
    uint32_t r = lib$spawn(NULL, &in, &out, &flags, NULL, &pid, NULL, &efn,
                           NULL, NULL, NULL, NULL, NULL);
    CHECK((r & 1) && pid != 0, "lib$spawn accepts an ODS-2 INPUT filespec and creates the subprocess");
    int set = 0;
    for (int w = 0; w < WAIT_MS && !set; w += 50) {
        set = (sys$readef(efn, &(uint32_t){0}) == SS$_WASSET);
        if (!set) { struct pollfd nothing = { .fd = -1, .events = 0 }; poll(&nothing, 1, 50); }
    }
    CHECK(set, "the subprocess completed");
    char body[256] = "";
    int ofd = open(OUT_PATH, O_RDONLY);
    if (ofd >= 0) { ssize_t n = read(ofd, body, sizeof body - 1); if (n > 0) body[n] = 0; close(ofd); }
    /* negctl: spawn-input-via-linux-path */
    CHECK(strstr(body, MARKER) != NULL, "the DCL executed the ODS-2 command file (its output reached the output file)");

    /* (2) a missing input is an honest RMS$_FNF and creates nothing */
    struct dsc$descriptor_s missing = dsc("SYS$SYSROOT:[SYSMGR]NO_SUCH_SPAWN_INPUT.COM");
    uint32_t pid2 = 0;
    r = lib$spawn(NULL, &missing, NULL, &flags, NULL, &pid2, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    CHECK(r == RMS$_FNF && pid2 == 0, "a missing INPUT file is RMS$_FNF and no subprocess is created");

    printf("=== %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
