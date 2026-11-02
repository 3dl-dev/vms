/*
 * link_buildtool.c - the build-machine front end of the HOST linker tool.
 *
 * Included by link.c when it is compiled as the tool that builds OpenVMX on a
 * Linux build machine (CMake target `vmslink`, the bootstrap LINK.EXE of the
 * mk_*.sh recipes). Those recipes drive it with a flag vector:
 *
 *   -o IMAGE  --shareable | --executable  --use SHR.EXE  --library LIB
 *   --symbol-vector "A=PROCEDURE,..."  --gsmatch KIND,MAJ,MIN  --transfer SYM
 *   --base ADDR  --allow-undefined  input...
 *
 * This file is NEVER compiled into a shipped image (rd vms-3cb1, vms-18c9): the
 * LINK.EXE an OpenVMX system runs is built with OVMX_LINK_IMAGE and takes the
 * VMS LINK command through the CLI$ routines (link_cli.c). The shipped-image
 * strings gate (src/vmslink/test/run_link_command.sh) proves that.
 */

#include "link_cmd.h"

int main(int argc, char **argv)
{
    struct link_cmd c;
    memset(&c, 0, sizeof c);
    int named_out = 0;
    for (int i = 1; i < argc; i++)
        if (strcmp(argv[i], "-o") == 0) named_out = 1;
    if (!named_out) {
        fprintf(stderr, "%%LINK-F-BUILDTOOL, this is the build-machine linker, driven by "
                "build flags; the VMS LINK command is taken by SYS$SYSTEM:LINK.EXE\n");
        return EXIT_FAILURE;
    }
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) {
            c.out = argv[++i];
        } else if (strcmp(argv[i], "--shareable") == 0) {
            c.shareable = 1;
        } else if (strcmp(argv[i], "--executable") == 0) {
            c.executable = 1;
        } else if (strcmp(argv[i], "--allow-undefined") == 0) {
            c.allow_undef = 1;
        } else if (strcmp(argv[i], "--use") == 0 && i + 1 < argc) {
            lc_use(&c, argv[++i]);
        } else if (strcmp(argv[i], "--symbol-vector") == 0 && i + 1 < argc) {
            free(c.symvec); c.symvec = NULL;   /* the last one given stands */
            lc_symvec(&c, argv[++i]);
        } else if (strcmp(argv[i], "--gsmatch") == 0 && i + 1 < argc) {
            c.gsmatch = argv[++i];
        } else if (strcmp(argv[i], "--base") == 0 && i + 1 < argc) {
            char *e = NULL;
            c.base = strtoull(argv[++i], &e, 0);
            if (!e || *e) die("the base must be a number");
            c.base_set = 1;
        } else if (strcmp(argv[i], "--library") == 0 && i + 1 < argc) {
            lc_library(&c, argv[++i]);   /* searched where it appears (vms-4d0) */
        } else if (strcmp(argv[i], "--transfer") == 0 && i + 1 < argc) {
            c.transfer = argv[++i];      /* EVAX/Alpha main transfer address (vms-cbe) */
        } else if (argv[i][0] == '-') {
            fprintf(stderr, "%%LINK-W-IGNORED, unknown option %s\n", argv[i]);
        } else {
            lc_input(&c, argv[i]);
        }
    }
    if (c.nin == 0)
        die("no input (usage: LINK.EXE --shareable --symbol-vector \"f=PROCEDURE\" "
            "--gsmatch LEQUAL,1,0 [--allow-undefined] -o X.EXE a.o [b.o | lib.a ...] "
            "| LINK.EXE --executable --use LIB$SHR.EXE -o PROG.EXE prog.o)");
    return link_run(&c);
}
