/*
 * test_syssvc_dcl_image_sysout.c - what an IMAGE that DCL runs writes is a
 * record of DCL's own SYS$OUTPUT, read here from that SYS$OUTPUT mailbox
 * (rd vms-380f; harness rd vms-2f39).
 *
 * WHAT VMS DOES. An image DCL activates -- RUN, a utility verb (SYSGEN,
 * INSTALL, PRODUCT) -- inherits the process-permanent
 * SYS$OUTPUT/SYS$ERROR; what it writes lands in the same stream as DCL's own
 * records, in order (VSI OpenVMS DCL Dictionary, SYS$OUTPUT; OpenVMS User's
 * Manual, "Process-permanent files"). On the host there is no executive, so a
 * host image cannot learn SYS$OUTPUT: these probes lived on DCL.EXE's stdio
 * there. They live here now, in the one runtime, where SYS$OUTPUT is real.
 *
 * MOVED FROM tests/dcl/ (rd vms-380f), each assertion kept or made stricter:
 *   misc      <- test_misc_commands.sh            (ATTACH, PHONE, PRODUCT.EXE)
 *   reloc     <- test_utility_sys_system_reloc.sh (SYS$SYSTEM relocates a verb)
 *   sysin     <- test_run_sysinput_procedure.sh   (RUN data lines of an @proc)
 *   apos      <- test_run_sysinput_apostrophe.sh  ('sym' in those data lines)
 *   cclan     <- test_cluster_config_lan.sh       (CLUSTER_CONFIG_LAN -> SYSGEN)
 * (test_mail_qualifier_validation.sh moved to test_syssvc_dcl_mail_quals.c.
 * test_copy_dnet_local_default.sh stays a host probe: COPY node:: is RMS
 * remote access and runs no image (rd vms-0a7).)
 *
 * (test_install_command.sh moved to test_syssvc_dcl_install.c with the fix
 * that made its messages visible, rd vms-d41.)
 *
 * STAND-INS. Where the host probe used a #!/bin/sh or cc-built stand-in
 * (a marker SYSGEN.EXE, the SYS$INPUT echo image), this binary
 * is copied under that name and recognises it from /proc/self/exe.
 *
 * Fixtures DCL reads as procedures are laid down on VDA0: through RMS: DCL
 * reaches a procedure only through RMS (rd vms-20c94).
 *
 * NEGATIVE CONTROL: tests/qemu/defects.d/dcl-data-line-not-substituted.defect.
 * NO EXECUTIVE: EXIT_SKIP (77).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <sys/wait.h>
#include <sys/stat.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "rms/rms.h"
#include "vms_kif.h"
#include "ovmx_test_fork.h"
#include "ovmx_dcl_sysout.h"
#include "vmsfs/ods2.h"
#include "sysvol_stage.h"

#define EXIT_SKIP 77
#define DCL_PATH  "/bin/DCL.EXE"
#define SYSEXE    "/vms/SYS0/SYSCOMMON/SYSEXE"
#define SYSLIB    "/vms/SYS0/SYSCOMMON/SYSLIB"
#define SYSMGR    "/vms/SYS0/SYSCOMMON/SYSMGR"
#define PROC_UNIT "VDA0:"
#define PROC_DIR  PROC_UNIT "[OVMXDIR]"
#define WORK      "/tmp/ovmx380f"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else      { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static char out[65536];

/* ---- the stand-in images: this binary, under the name DCL activates ---- */

static const char *self_base(char *buf, size_t sz)
{
    ssize_t n = readlink("/proc/self/exe", buf, sz - 1);
    if (n <= 0) return "";
    buf[n] = '\0';
    char *semi = strchr(buf, ';');
    if (semi) *semi = '\0';
    const char *b = strrchr(buf, '/');
    return b ? b + 1 : buf;
}

static int standin(const char *base, const char *path, int argc, char **argv)
{
    if (strcmp(base, "SYSGEN.EXE") == 0) {         /* the relocated marker */
        printf("OVMX-VMS7D8-UTILITY-VIA-SYS$SYSTEM activated-from=%s\n", path);
        return 0;
    }
    if (strcmp(base, "ECHOIN.EXE") == 0) {         /* reads its SYS$INPUT */
        char line[4096];
        while (fgets(line, sizeof line, stdin)) {
            size_t n = strlen(line);
            if (n && line[n - 1] == '\n') line[--n] = '\0';
            printf("SAW:%s\n", line);
        }
        printf("IMGEOF\n");
        fflush(stdout);
        return 0;
    }
    return -1;
}

/* ---- fixtures ---- */

static int copy_file(const char *src, const char *dst, mode_t mode)
{
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int o = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode);
    if (o < 0) { close(in); return -1; }
    char buf[65536];
    ssize_t n;
    int rc = 0;
    while ((n = read(in, buf, sizeof buf)) > 0)
        if (write(o, buf, (size_t)n) != n) { rc = -1; break; }
    close(in);
    if (close(o) != 0) rc = -1;
    chmod(dst, mode);
    return rc;
}

static int stage_self(const char *dst)
{
    return copy_file("/proc/self/exe", dst, 0755);
}

/* A procedure as variable-length carriage-return records, one $PUT a line. */
static int write_com_rms(const char *spec, const char *text)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_VAR;
    fab.fab$b_rat = FAB$M_CR;
    fab.fab$b_fac = FAB$M_PUT;
    if (!(sys$create(&fab, 0, 0) & 1)) return -1;
    struct RAB rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    if (!(sys$connect(&rab, 0, 0) & 1)) { sys$close(&fab, 0, 0); return -1; }
    uint32_t st = RMS$_NORMAL;
    while (*text && (st & 1)) {
        const char *nl = strchr(text, '\n');
        size_t n = nl ? (size_t)(nl - text) : strlen(text);
        rab.rab$l_rbf = (char *)text;
        rab.rab$w_rsz = (uint16_t)n;
        st = sys$put(&rab, 0, 0);
        text += n + (nl ? 1 : 0);
    }
    uint32_t cst = sys$close(&fab, 0, 0);
    return ((st & 1) && (cst & 1)) ? 0 : -1;
}

static void erase_rms(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

/* ---- the drive: DCL.EXE, its SYS$OUTPUT/SYS$ERROR the suite's mailbox ---- */

static int run_dcl(const char *proc, const char *script, char *const env[],
                   char *buf, size_t bufsz)
{
    int in_pipe[2];
    struct ovmx_dcl_sysout so;
    buf[0] = '\0';
    if (!(ovmx_dcl_sysout_open(&so) & 1)) return -1;
    if (pipe(in_pipe) < 0) { ovmx_dcl_sysout_collect(&so, NULL, 0); return -1; }
    fflush(NULL);
    pid_t pid = ovmx_fork_login();
    if (pid < 0) {
        close(in_pipe[0]); close(in_pipe[1]);
        ovmx_dcl_sysout_collect(&so, NULL, 0);
        return -1;
    }
    if (pid == 0) {
        ovmx_dcl_sysout_child(&so);
        dup2(in_pipe[0], STDIN_FILENO);
        close(in_pipe[0]); close(in_pipe[1]);
        if (proc)
            execle(DCL_PATH, "DCL.EXE", proc, (char *)NULL, env);
        else
            execle(DCL_PATH, "DCL.EXE", (char *)NULL, env);
        ovmx_dcl_sysout_note(&so, "EXEC-OF-DCL-FAILED\n");
        _exit(127);
    }
    close(in_pipe[0]);
    if (write(in_pipe[1], script, strlen(script)) < 0) { /* DCL reads EOF */ }
    close(in_pipe[1]);
    int st;
    while (waitpid(pid, &st, 0) < 0 && errno == EINTR)
        ;
    ovmx_dcl_sysout_collect(&so, buf, bufsz);
    return 0;
}

static void show(const char *what)
{
    printf("  ---- %s: DCL's SYS$OUTPUT ----\n%s  ---- end ----\n", what, out);
}

static int has(const char *s) { return strstr(out, s) != NULL; }

/* A whole line of `out` equals `want` (record terminators stripped). */
static const char *line_eq(const char *want)
{
    size_t wl = strlen(want);
    for (const char *p = out; p && *p; ) {
        const char *e = strchr(p, '\n');
        size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && (p[l - 1] == '\r')) l--;
        if (l == wl && memcmp(p, want, wl) == 0) return p;
        p = e ? e + 1 : NULL;
    }
    return NULL;
}

/* A line holding `a` and, after it on the same line, `b`. */
static int same_line(const char *a, const char *b)
{
    for (const char *p = strstr(out, a); p; p = strstr(p + 1, a)) {
        const char *e = strchr(p, '\n');
        const char *q = strstr(p + strlen(a), b);
        if (q && (!e || q < e)) return 1;
    }
    return 0;
}

static char *const base_env[] = { (char *)"PATH=/bin", NULL };

/* ---- misc <- tests/dcl/test_misc_commands.sh ---- */
static void case_misc(void)
{
    CHECK(access(SYSEXE "/PRODUCT.EXE", X_OK) == 0,
          "misc: PRODUCT.EXE is staged at SYS$SYSTEM in the initramfs");
    CHECK(run_dcl(NULL, "ATTACH\nPHONE\nPRODUCT SHOW PRODUCT\n", base_env, out, sizeof out) == 0,
          "misc: DCL ran ATTACH, PHONE, PRODUCT SHOW PRODUCT");
    show("misc");
    CHECK(has("ATTFAIL"), "misc: ATTACH with nothing to attach to is ATTFAIL");
    CHECK(has("NOTAVAIL"), "misc: PHONE is NOTAVAIL");
    CHECK(same_line("OVMX", "Installed"),
          "misc: PRODUCT.EXE's SHOW PRODUCT row (OVMX ... Installed) reached DCL's SYS$OUTPUT");
}

/* ---- reloc <- tests/dcl/test_utility_sys_system_reloc.sh ---- */
#define RELOC_DIR WORK "_reloc"
static void case_reloc(void)
{
    mkdir(RELOC_DIR, 0755);
    CHECK(stage_self(RELOC_DIR "/SYSGEN.EXE") == 0,
          "reloc: a marker SYSGEN.EXE is staged in a relocated SYS$SYSTEM");
    /* PATH pruned: the only way to this SYSGEN.EXE is the SYS$SYSTEM translation. */
    char *const env[] = { (char *)"PATH=/usr/bin:/bin", NULL };
    CHECK(run_dcl(NULL, "DEFINE SYS$SYSTEM \"" RELOC_DIR "\"\nSYSGEN\nEXIT\n",
                  env, out, sizeof out) == 0,
          "reloc: DCL ran the SYSGEN verb after DEFINE SYS$SYSTEM");
    show("reloc");
    CHECK(has("OVMX-VMS7D8-UTILITY-VIA-SYS$SYSTEM"),
          "reloc: the SYSGEN verb activated SYS$SYSTEM:SYSGEN.EXE through the logical");
    CHECK(has("activated-from=" RELOC_DIR "/SYSGEN.EXE"),
          "reloc: the image that ran is the one in the relocated directory");
    CHECK(!has("-F-NOIMG"), "reloc: no NOIMG");
    unlink(RELOC_DIR "/SYSGEN.EXE");
    rmdir(RELOC_DIR);
}

/* ---- sysin / apos <- test_run_sysinput_{procedure,apostrophe}.sh ---- */
#define ECHOIN WORK "/ECHOIN.EXE"
#define SYSIN_COM PROC_DIR "OVMX380F_SYSIN.COM"
#define APOS_COM  PROC_DIR "OVMX380F_APOS.COM"
static void case_sysin(void)
{
    CHECK(write_com_rms(SYSIN_COM,
                        "$ RUN \"" ECHOIN "\"\n"
                        "SYSINLINE_ALPHA\n"
                        "SYSINLINE_BETA\n"
                        "$ WRITE SYS$OUTPUT \"RESUMED_AT_DOLLAR\"\n"
                        "$ EXIT\n") == 0,
          "sysin: the procedure (RUN, two data lines, a WRITE) is laid down through RMS");
    CHECK(run_dcl(NULL, "SET DEFAULT " PROC_DIR "\n@OVMX380F_SYSIN.COM\n",
                  base_env, out, sizeof out) == 0, "sysin: DCL ran the procedure");
    show("sysin");
    const char *a = line_eq("SAW:SYSINLINE_ALPHA"), *b = line_eq("SAW:SYSINLINE_BETA");
    const char *eof = line_eq("IMGEOF"), *res = strstr(out, "RESUMED_AT_DOLLAR");
    CHECK(a && b, "sysin: the RUN image read the two data lines that follow RUN");
    CHECK(eof, "sysin: the image saw end of file after the block");
    CHECK(res, "sysin: DCL resumed at the first '$' line after the block");
    CHECK(a && b && eof && res && a < b && b < eof && eof < res,
          "sysin: the image's records and DCL's are in SYS$OUTPUT in the order they were written");
    CHECK(!has("SAW:$"), "sysin: the '$' line was not fed to the image");
    CHECK(!has("IVVERB"), "sysin: no data line was executed as a DCL command");
    erase_rms(SYSIN_COM);
}
static void case_apos(void)
{
    CHECK(write_com_rms(APOS_COM,
                        "$ PWSYM = \"INTERP_SECRET_OK\"\n"
                        "$ RUN \"" ECHOIN "\"\n"
                        "MODIFY SYSTEM/PASSWORD='PWSYM'\n"
                        "PLAINVERBATIM_NOAPOS\n"
                        "LONE ' KEPT\n"
                        "$ WRITE SYS$OUTPUT \"RESUMED_AT_DOLLAR\"\n"
                        "$ EXIT\n") == 0,
          "apos: the procedure is laid down through RMS");
    CHECK(run_dcl(NULL, "SET DEFAULT " PROC_DIR "\n@OVMX380F_APOS.COM\n",
                  base_env, out, sizeof out) == 0, "apos: DCL ran the procedure");
    show("apos");
    /* negctl: dcl-data-line-not-substituted */
    CHECK(line_eq("SAW:MODIFY SYSTEM/PASSWORD=INTERP_SECRET_OK") != NULL,
          "apos: 'PWSYM' in a RUN data line reaches the image as the symbol's value");
    CHECK(line_eq("SAW:PLAINVERBATIM_NOAPOS") != NULL,
          "apos: a data line with no marker reaches the image byte for byte");
    CHECK(line_eq("SAW:LONE ' KEPT") != NULL,
          "apos: a lone apostrophe is kept");
    /* negctl: dcl-data-line-not-substituted */
    CHECK(!has("PASSWORD='PWSYM'"),
          "apos: the unsubstituted marker never reaches the image");
    CHECK(!has("IVVERB") && has("RESUMED_AT_DOLLAR"),
          "apos: the data lines went to the image, and DCL resumed after them");
    erase_rms(APOS_COM);
}

/* ---- cclan <- tests/dcl/test_cluster_config_lan.sh ---- */
#define CCL_SRC   SYSMGR "/CLUSTER_CONFIG_LAN.COM"
#define CCL_COM   PROC_DIR "OVMX380F_CCL.COM"
#define CCL_SEED  PROC_DIR "OVMX380F_CCLSEED.COM"
#define CCL_RB    PROC_DIR "OVMX380F_CCLRB.COM"
#define CCL_STORE WORK "/OVMXVMSSYS.PAR"

/* `key` then spaces, one character, spaces, then `val` (the confirmation
 * screen's "SCSNODE         = OVMXA"). */
static int kv_line(const char *key, const char *val)
{
    for (const char *p = strstr(out, key); p; p = strstr(p + 1, key)) {
        const char *q = p + strlen(key);
        if (*q != ' ') continue;
        while (*q == ' ') q++;
        if (!*q || *q == '\n') continue;
        q++;
        if (*q != ' ') continue;
        while (*q == ' ') q++;
        if (strncmp(q, val, strlen(val)) == 0) return 1;
    }
    return 0;
}

/* SYSGEN SHOW's row for `param` ("  SCSNODE   "OVMXA " ..."): the value,
 * display framing (quotes, padding) stripped. */
static int sysgen_value(const char *param, char *v, size_t vsz)
{
    char key[64];
    snprintf(key, sizeof key, "\n  %s ", param);
    const char *p = strstr(out, key);
    v[0] = '\0';
    if (!p) return 0;
    p += strlen(key);
    while (*p == ' ') p++;
    size_t n = 0;
    if (*p == '"') {
        p++;
        while (*p && *p != '"' && *p != '\n' && n + 1 < vsz)
            if (*p++ != ' ') v[n++] = p[-1];
    } else {
        while (*p && *p != ' ' && *p != '\n' && n + 1 < vsz) v[n++] = *p++;
    }
    v[n] = '\0';
    return 1;
}

static int load_text(const char *path, char *buf, size_t sz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t k = 0;
    ssize_t r;
    while (k + 1 < sz && (r = read(fd, buf + k, sz - 1 - k)) > 0) k += (size_t)r;
    close(fd);
    buf[k] = '\0';
    return k > 0 && k + 1 < sz ? 0 : -1;
}

static void case_cclan(void)
{
    static char com[65536];
    /* `RUN SYS$SYSTEM:SYSGEN.EXE` reads the image off the system volume over
     * the ACP, as VMS does (never a /vms POSIX path, INV-6): SYSGEN.EXE is
     * staged onto VDA300:[SYS0.SYSCOMMON.SYSEXE] (sysvol_stage.h) and DCL's
     * SYS$SYSDEVICE is that volume (OVMX_SYSDEVICE, as the boot chain sets it). */
    char *const env[] = { (char *)"PATH=/bin",
                          (char *)"OVMX_SYSGEN_PATH=" CCL_STORE,
                          (char *)"OVMX_SYSDEVICE=" SYSVOL_UNIT, NULL };
    CHECK(access(SYSEXE "/SYSGEN.EXE", X_OK) == 0,
          "cclan: SYSGEN.EXE is staged at SYS$SYSTEM in the initramfs");
    CHECK(sysvol_stage_subject(SYSEXE "/SYSGEN.EXE", "SYSGEN.EXE") == 0,
          "cclan: SYSGEN.EXE is staged on the system volume " SYSVOL_UNIT "[SYS0.SYSCOMMON.SYSEXE] over the ACP");
    CHECK(load_text(CCL_SRC, com, sizeof com) == 0 && write_com_rms(CCL_COM, com) == 0,
          "cclan: the shipped CLUSTER_CONFIG_LAN.COM is laid down through RMS");
    unlink(CCL_STORE);
    CHECK(write_com_rms(CCL_SEED, "$ RUN SYS$SYSTEM:SYSGEN.EXE\nUSE DEFAULT\nWRITE CURRENT\nEXIT\n$ EXIT\n") == 0 &&
          write_com_rms(CCL_RB, "$ WRITE SYS$OUTPUT \"OVMX380F-READBACK\"\n"
                                "$ RUN SYS$SYSTEM:SYSGEN.EXE\nUSE CURRENT\nSHOW SCSNODE\nSHOW SCSSYSTEMID\nEXIT\n$ EXIT\n") == 0,
          "cclan: the seed and read-back procedures are laid down through RMS");
    CHECK(run_dcl(NULL, "@" CCL_SEED "\n", env, out, sizeof out) == 0 &&
          access(CCL_STORE, F_OK) == 0,
          "cclan: a current parameter store is seeded from the factory defaults");

    /* menu 2 (CHANGE); a too-long name is rejected then OVMXA accepted;
     * SCSSYSTEMID 1030; ALLOCLASS/VOTES/EXPECTED_VOTES defaulted; confirm Y;
     * menu 3 (REMOVE, honest decline); menu 5 (EXIT). */
    /* As an operator runs it: @ the procedure at the DCL prompt; its INQUIREs
     * read the answers that follow from SYS$COMMAND. (DCL.EXE's host-only
     * "DCL.EXE file" script mode is not used: it binds no SYS$OUTPUT mailbox,
     * rd vms-380f trail.) */
    CHECK(run_dcl(NULL, "@" CCL_COM "\n2\nTOOLONGNAME\nOVMXA\n1030\n\n\n\nY\n3\n5\nEXIT\n",
                  env, out, sizeof out) == 0, "cclan: DCL ran CLUSTER_CONFIG_LAN.COM");
    show("cclan");
    CHECK(kv_line("SCSNODE", "OVMXA"), "cclan: the confirmation screen echoes SCSNODE = OVMXA");
    CHECK(kv_line("SCSSYSTEMID", "1030"), "cclan: the confirmation screen echoes SCSSYSTEMID = 1030");
    /* negctl-knockon: dcl-data-line-not-substituted */
    CHECK(has("SCSNODE changed from OVMX to OVMXA"),
          "cclan: SYSGEN, driven by the procedure's RUN data lines, set SCSNODE");
    /* negctl-knockon: dcl-data-line-not-substituted */
    CHECK(has("SCSSYSTEMID changed from 0 to 1030"),
          "cclan: SYSGEN, driven by the procedure's RUN data lines, set SCSSYSTEMID");
    CHECK(has("CLUSTER_CONFIG-E-BADNODE"), "cclan: an over-long SCSNODE is rejected, not written");
    CHECK(has("not available at this OpenVMX"), "cclan: REMOVE is honestly declined");

    CHECK(run_dcl(NULL, "@" CCL_RB "\n", env, out, sizeof out) == 0,
          "cclan: DCL ran the SYSGEN read-back");
    show("cclan read-back");
    char node[64], sid[64];
    char *rb = strstr(out, "OVMX380F-READBACK");
    if (rb) memmove(out, rb, strlen(rb) + 1);
    sysgen_value("SCSNODE", node, sizeof node);
    sysgen_value("SCSSYSTEMID", sid, sizeof sid);
    printf("  info: READBACK SCSNODE=%s SCSSYSTEMID=%s\n", node, sid);
    /* negctl-knockon: dcl-data-line-not-substituted */
    CHECK(strcmp(node, "OVMXA") == 0,
          "cclan: the store holds SCSNODE=OVMXA, no quote characters");
    /* negctl-knockon: dcl-data-line-not-substituted */
    CHECK(strcmp(sid, "1030") == 0, "cclan: the store holds SCSSYSTEMID=1030");
    erase_rms(CCL_COM);
    erase_rms(CCL_SEED);
    erase_rms(CCL_RB);
    unlink(CCL_STORE);
}

int main(int argc, char **argv)
{
    char selfbuf[512];
    const char *base = self_base(selfbuf, sizeof selfbuf);
    int sr = standin(base, selfbuf, argc, argv);
    if (sr >= 0)
        return sr;

    setvbuf(stdout, NULL, _IOLBF, 0);
    printf("=== test_syssvc_dcl_image_sysout (an image DCL runs writes DCL's SYS$OUTPUT, vms-380f) ===\n");

    if (vms_kif_open() < 0) {
        printf("=== test_syssvc_dcl_image_sysout: SKIPPED: no /dev/vms -- the executive is absent, nothing proved ===\n");
        return EXIT_SKIP;
    }
    if (access(DCL_PATH, X_OK) != 0) {
        printf("  FAIL: %s is not present/executable in the initramfs\n", DCL_PATH);
        return 1;
    }
    CHECK($VMS_STATUS_SUCCESS(vms_kif_acp_mount(PROC_UNIT)),
          "MOUNT VDA0:, the volume the procedures are laid down on");
    mkdir(WORK, 0755);
    CHECK(stage_self(ECHOIN) == 0, "the SYS$INPUT echo image is staged");

    case_misc();
    case_reloc();
    case_sysin();
    case_apos();
    case_cclan();

    unlink(ECHOIN);
    rmdir(WORK);
    printf("=== test_syssvc_dcl_image_sysout: %d passed, %d failed ===\n", pass, fail);
    return fail ? 1 : 0;
}
