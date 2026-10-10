/*
 * install.c - INSTALL utility: the executive's known file list
 * (bead vms-913.5; executive-resident since rd vms-7c64 / vms-220).
 *   INSTALL ADD     <image-filespec> [/OPEN] [/SHARED] [/HEADER_RESIDENT]
 *                                    [/PRIVILEGED=(priv,...)]
 *   INSTALL REPLACE <image-filespec> [qualifiers as ADD]
 *   INSTALL LIST    [<image-filespec>] [/FULL]
 *   INSTALL REMOVE  <image-filespec>
 *
 * On VMS the known file list is executive state, changed under CMKRNL. Here
 * too: every change is a VMS_IOCTL_KFE request (vms_kif_kfe), refused
 * SS$_NOPRIV without CMKRNL, and the list lives in the executive -- there is
 * no on-disk database (the former SYS$SYSTEM:VMS$KNOWN_IMAGES.DAT, which an
 * unprivileged startup could not write and anyone with that file could edit).
 * The executive PINS the image file an entry names and denies writes to it
 * for the entry's life; an entry installed /PRIVILEGED must be root-owned with
 * no write permission (the boot-staged images are), so nothing that runs in
 * OVMX can change it (Baron's ruling, rd vms-96e7).
 *
 * Output follows the lab VMS captures (tests/lab/captures/install-priv-
 * 20261009/): LIST prints the entry with its Open/Hdr/Shar/Prv tags; /FULL
 * adds the entry access count and the privileges.
 */
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>

#include "ovmx_layout.h"
#include "str_util.h"

#include "vms_kif.h"
#include "vms/privs.h"

/* SS$_ values the executive returns (ssdef.h numbers, kept local: this
 * utility includes no VMS headers beyond the kif). */
#define ST_NOPRIV     0x24u
#define ST_DUPLNAM    148u
#define ST_NOSUCHFILE 2320u
#define ST_ACCONFLICT 2048u

static const struct { const char *name; uint64_t bit; } priv_names[] = {
    { "CMKRNL", PRV$M_CMKRNL }, { "CMEXEC", PRV$M_CMEXEC }, { "SYSNAM", PRV$M_SYSNAM },
    { "GRPNAM", PRV$M_GRPNAM }, { "DETACH", PRV$M_DETACH }, { "TMPMBX", PRV$M_TMPMBX },
    { "WORLD", PRV$M_WORLD },   { "OPER", PRV$M_OPER },     { "NETMBX", PRV$M_NETMBX },
    { "SETPRV", PRV$M_SETPRV }, { "SYSPRV", PRV$M_SYSPRV }, { "BYPASS", PRV$M_BYPASS },
    { "GROUP", PRV$M_GROUP },   { "LOG_IO", PRV$M_LOG_IO }, { "PHY_IO", PRV$M_PHY_IO },
    { "ALTPRI", PRV$M_ALTPRI }, { "SETPRI", PRV$M_SETPRI },
};

static void report_failure(const char *what, const char *spec, uint32_t st)
{
    const char *why =
        st == ST_NOPRIV     ? "-SYSTEM-F-NOCMKRNL, operation requires CMKRNL privilege" :
        st == ST_DUPLNAM    ? "-INSTALL-E-DUPLICATE, Known File Entry already exists" :
        st == ST_NOSUCHFILE ? "-INSTALL-E-NOKFEFND, Known File Entry not found" :
        st == ST_ACCONFLICT ? "-SYSTEM-W-ACCONFLICT, file access conflict" :
        st == 0x908u        ? "-SYSTEM-W-NOSUCHDEV, no such device available (no executive)" : NULL;
    fprintf(stderr, "%%INSTALL-E-FAIL, failed to %s entry for %s\n", what, spec);
    if (why)
        fprintf(stderr, "%s\n", why);
    else
        fprintf(stderr, "-SYSTEM-F-ABORT, status %%X%08X\n", (unsigned)st);
}

#define KNOWN_IMAGES_DB  VMS_SYSTEM_DIR "/VMS$KNOWN_IMAGES.DAT"

/*
 * resolve_filespec - turn an image filespec into {SONAME, Linux path}.
 *
 * Accepts "SYS$SHARE:name", "SYS$SYSTEM:name", or a bare "name" (defaults
 * to SYS$SHARE, where installed shareable images live). Any other device/
 * logical prefix is rejected — real INSTALL only manages images in the
 * known SYS$SHARE/SYS$SYSTEM locations.
 *
 * Returns 0 on success, -1 if the filespec cannot be resolved.
 */
static int resolve_filespec(const char *filespec, char *soname, size_t soname_len,
                             char *linux_path, size_t path_len)
{
    const char *colon = strrchr(filespec, ':');
    const char *dir;
    const char *name;

    if (colon) {
        char prefix[32] = {0};
        size_t plen = (size_t)(colon - filespec);
        if (plen == 0 || plen >= sizeof(prefix))
            return -1;
        memcpy(prefix, filespec, plen);
        name = colon + 1;

        if (strcasecmp(prefix, "SYS$SHARE") == 0)
            dir = VMS_LIBRARY_DIR;
        else if (strcasecmp(prefix, "SYS$SYSTEM") == 0)
            dir = VMS_SYSTEM_DIR;
        else
            return -1;
    } else {
        dir = VMS_LIBRARY_DIR;
        name = filespec;
    }

    if (name[0] == '\0')
        return -1;

    str_upcase_copy(soname, name, soname_len);

    int n = snprintf(linux_path, path_len, "%s/%s", dir, soname);
    if (n < 0 || (size_t)n >= path_len)
        return -1;

    /*
     * ATOMIC FLIP (vms-0cb): SYS$DISK is now a genuine Files-11 (ODS-2) volume
     * owned by the executive ACP; the vmsfs_to_linux_path -> /vms POSIX
     * passthrough is retired, so a SYS$SHARE / SYS$SYSTEM image has no /vms
     * path to stat. PID 1 stages the SYS$SHARE shareables (and SYS$SYSTEM
     * utilities) off the ODS-2 volume THROUGH THE ACP into OVMX_BOOT_STAGE_DIR
     * (src/ovmx_init/ovmx_init.c stage_boot_images(); INV-6: bytes from the
     * ACP, never a /vms read). INSTALL cannot ride the in-process ACP path --
     * it must stat() a real file to register it and to record the path IMGACT
     * maps -- so when the canonical /vms path is absent, resolve to the staged
     * copy instead. The staging directory is flat, keyed by the uppercase
     * basename (== SONAME), so the same rewrite covers SYS$SHARE and SYS$SYSTEM.
     *
     * When neither the /vms path nor a staged copy exists, keep the canonical
     * path: cmd_add's stat() then reports an honest %INSTALL-E-FILNOTFND naming
     * the SYS$SHARE/SYS$SYSTEM location, never a faked success. On a real /vms
     * fixture (the host INSTALL unit test) the canonical path exists and is
     * used unchanged -- the staged fallback only engages on the flipped runtime.
     */
    struct stat st;
    if (stat(linux_path, &st) != 0) {
        char staged[256];
        int m = snprintf(staged, sizeof(staged), "%s/%s",
                         OVMX_BOOT_STAGE_DIR, soname);
        if (m > 0 && (size_t)m < sizeof(staged) &&
            (size_t)m < path_len && stat(staged, &st) == 0) {
            memcpy(linux_path, staged, (size_t)m + 1);
        }
    }
    return 0;
}



/* ------------------------------------------------------------------ */
/* Subcommands                                                         */
/* ------------------------------------------------------------------ */

/* ADD and REPLACE: open the resolved image file and hand it to the executive. */
static int add_or_replace(int replace, int argc, char *argv[])
{
    if (argc < 1) {
        fprintf(stderr, "%%INSTALL-E-NOFILE, no image filespec specified\n");
        return 1;
    }
    struct vms_kfe_args a;
    memset(&a, 0, sizeof a);
    a.op = replace ? VMS_KFE_OP_REPLACE : VMS_KFE_OP_ADD;
    for (int i = 1; i < argc; i++) {
        if (strcasecmp(argv[i], "/OPEN") == 0)
            a.flags |= VMS_KFE_F_OPEN;
        else if (strcasecmp(argv[i], "/SHARED") == 0)
            a.flags |= VMS_KFE_F_SHARED;
        else if (strcasecmp(argv[i], "/HEADER_RESIDENT") == 0)
            a.flags |= VMS_KFE_F_HDRRES;
        else if (strncasecmp(argv[i], "/PRIVILEGED=", 12) == 0) {
            char list[256];
            const char *v = argv[i] + 12;
            size_t n = strlen(v);
            if (n && v[0] == '(' && v[n - 1] == ')') { v++; n -= 2; }
            if (n >= sizeof list) n = sizeof list - 1;
            memcpy(list, v, n);
            list[n] = '\0';
            a.privs = parse_privilege_string(list);
            a.flags |= VMS_KFE_F_PRIV;
        } else {
            fprintf(stderr, "%%INSTALL-E-IVQUAL, unrecognized qualifier \"%s\"\n", argv[i]);
            return 1;
        }
    }
    char soname[64] = {0}, path[256] = {0};
    if (resolve_filespec(argv[0], soname, sizeof soname, path, sizeof path) != 0) {
        fprintf(stderr, "%%INSTALL-E-BADFILE, cannot resolve \"%s\"\n", argv[0]);
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "%%INSTALL-E-FILNOTFND, file %s not found\n", path);
        return 1;
    }
    a.fd = fd;
    snprintf(a.name, sizeof a.name, "%s", soname);
    snprintf(a.path, sizeof a.path, "%s", path);
    uint32_t st = vms_kif_kfe(&a);
    close(fd);
    if (!(st & 1)) {
        report_failure(replace ? "REPLACE" : "CREATE", argv[0], st);
        return 1;
    }
    return 0;
}

static int cmd_add(int argc, char *argv[])     { return add_or_replace(0, argc, argv); }
static int cmd_replace(int argc, char *argv[]) { return add_or_replace(1, argc, argv); }

static int cmd_remove(int argc, char *argv[])
{
    if (argc < 1) {
        fprintf(stderr, "%%INSTALL-E-NOFILE, no image filespec specified\n");
        return 1;
    }
    char soname[64] = {0}, path[256] = {0};
    if (resolve_filespec(argv[0], soname, sizeof soname, path, sizeof path) != 0) {
        fprintf(stderr, "%%INSTALL-E-BADFILE, cannot resolve \"%s\"\n", argv[0]);
        return 1;
    }
    struct vms_kfe_args a;
    memset(&a, 0, sizeof a);
    a.op = VMS_KFE_OP_REMOVE;
    a.fd = -1;                       /* by name: the file may be gone */
    snprintf(a.name, sizeof a.name, "%s", soname);
    uint32_t st = vms_kif_kfe(&a);
    if (!(st & 1)) {
        report_failure("REMOVE", argv[0], st);
        return 1;
    }
    return 0;
}

static void print_entry(const struct vms_kfe_args *e, int full)
{
    printf("   %-30s %s%s%s%s\n", e->name,
           (e->flags & VMS_KFE_F_OPEN) ? "Open " : "",
           (e->flags & VMS_KFE_F_HDRRES) ? "Hdr " : "",
           (e->flags & VMS_KFE_F_SHARED) ? "Shar " : "",
           (e->flags & VMS_KFE_F_PRIV) ? "Prv " : "");
    if (!full)
        return;
    printf("        Entry access count         = %u\n", (unsigned)e->access);
    if (e->flags & VMS_KFE_F_PRIV) {
        printf("        Privileges =");
        for (size_t k = 0; k < sizeof priv_names / sizeof priv_names[0]; k++)
            if (e->privs & priv_names[k].bit)
                printf(" %s", priv_names[k].name);
        printf(" \n");
    }
    printf("\n");
}

static int cmd_list(int argc, char *argv[])
{
    int full = 0;
    const char *only = NULL;
    for (int i = 0; i < argc; i++) {
        if (strcasecmp(argv[i], "/FULL") == 0)
            full = 1;
        else if (argv[i][0] != '/' && !only)
            only = argv[i];
        else {
            fprintf(stderr, "%%INSTALL-E-IVQUAL, unrecognized qualifier \"%s\"\n", argv[i]);
            return 1;
        }
    }
    struct vms_kfe_args a;
    if (only) {
        char soname[64] = {0}, path[256] = {0};
        if (resolve_filespec(only, soname, sizeof soname, path, sizeof path) != 0) {
            fprintf(stderr, "%%INSTALL-E-BADFILE, cannot resolve \"%s\"\n", only);
            return 1;
        }
        memset(&a, 0, sizeof a);
        a.op = VMS_KFE_OP_FIND_NAME;
        a.fd = -1;
        snprintf(a.name, sizeof a.name, "%s", soname);
        uint32_t st = vms_kif_kfe(&a);
        if (!(st & 1)) {
            fprintf(stderr, "%%INSTALL-W-FAIL, failed to LIST entry for %s\n", only);
            fprintf(stderr, "-INSTALL-E-NOKFEFND, Known File Entry not found\n");
            return 1;
        }
        printf("\n");
        print_entry(&a, full);
        return 0;
    }
    uint32_t idx = 0;
    int n = 0;
    printf("\n");
    for (int guard = 0; guard < VMS_KFE_MAX; guard++) {
        memset(&a, 0, sizeof a);
        a.op = VMS_KFE_OP_LIST;
        a.fd = -1;
        a.index = idx;
        uint32_t st = vms_kif_kfe(&a);
        if (st == ST_NOSUCHFILE)
            break;                         /* past the last entry */
        if (!(st & 1)) {
            report_failure("LIST", "the known file list", st);
            return 1;
        }
        print_entry(&a, full);
        idx = a.index;
        n++;
    }
    if (n == 0)
        printf("%%INSTALL-I-NOIMAGES, no images installed\n");
    return 0;
}

static void usage(void)
{
    fprintf(stderr,
        "Usage:\n"
        "  INSTALL ADD     <image-filespec> [/OPEN] [/SHARED] [/HEADER_RESIDENT] [/PRIVILEGED=(...)]\n"
        "  INSTALL REPLACE <image-filespec> [qualifiers as ADD]\n"
        "  INSTALL LIST    [<image-filespec>] [/FULL]\n"
        "  INSTALL REMOVE  <image-filespec>\n");
}

int main(int argc, char *argv[])
{
    if (argc < 2) {
        usage();
        return 1;
    }

    const char *verb = argv[1];
    if (strcasecmp(verb, "ADD") == 0)
        return cmd_add(argc - 2, argv + 2);
    if (strcasecmp(verb, "REPLACE") == 0)
        return cmd_replace(argc - 2, argv + 2);
    if (strcasecmp(verb, "LIST") == 0)
        return cmd_list(argc - 2, argv + 2);
    if (strcasecmp(verb, "REMOVE") == 0)
        return cmd_remove(argc - 2, argv + 2);

    fprintf(stderr, "%%INSTALL-E-SYNTAX, unrecognized command \"%s\"\n", verb);
    usage();
    return 1;
}
