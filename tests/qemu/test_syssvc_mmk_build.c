/*
 * test_syssvc_mmk_build.c - the shipped MMK.EXE drives the WHOLE OVMX-native
 * build chain -- TCC compile -> LIBRARIAN archive -> LINK to a runnable image --
 * of the REAL OVMX runtime component, through its persistent mailbox-driven DCL
 * subprocess against a real /dev/vms, and the harness then ACTIVATES the produced
 * image through IMGACT to its oracle exit -- BYTE-IDENTICAL across two in-guest
 * builds, ZERO bash in the build path (self-host spine #7: vms-6be compile+
 * archive, vms-725 LINK+activate; extends spine #6 vms-d1b compile-only; also
 * closes the MMK-driven-EXECUTION residual of spine #5, vms-fe4).
 *
 * THE FULL CHAIN (vms-725, the FINAL self-host rung). The driven MMS:
 *   1. TCC compiles TWO real src/libvmssys runtime TUs (VMS_STRING.C +
 *      VMS_SNPRINTF.C -- both tcc-compilable on x86_64; vms_math.c's SSE "x"
 *      inline asm is NOT, so it is excluded) AND the driver OVMXRTRUN.C;
 *   2. LIBRARIAN /CREATEs OVMXRT.OLB from the two runtime objects;
 *   3. LINK --executable --use DECC$SHR.EXE links OVMXRTRUN.OBJ + OVMXRT.OLB into
 *      OVMXRT.EXE, SELECTIVELY pulling VMS_STRING from the library (the driver
 *      references vms_strlen; VMS_SNPRINTF stays archived-but-unpulled -- TCC
 *      compiles its varargs to tinycc's __va_arg helper, which DECC$SHR does not
 *      yet export, so the runnable image exercises the string TU only. The
 *      archive still carries both, proven above).
 * The produced OVMXRT.EXE carries PT_INTERP=/vms/.../IMGACT.EXE; the harness
 * ACTIVATES it (fork+exec -> the kernel loads IMGACT.EXE, which maps DECC$SHR.EXE
 * from SYS$LIBRARY, binds the image's one import, and runs crt0 -> main), and
 * asserts it exits 216 (vms_strlen("OVMXRT")==6, 6*36==216 -- nowhere else for
 * 216 to come from). Every stage is asserted BYTE-IDENTICAL across two
 * independent in-guest MMK-driven builds. This is self-hosting's final rung:
 * MMK builds a real OVMX component to a running image entirely inside OVMX.
 *
 * ============================================================
 * THE POINT. Spine #4 (test_syssvc_mmk_drive.c) proved MMK.EXE drives a
 * DCL-COMPUTED action (6*7 -> 42) over its persistent mailbox DCL. This suite
 * turns the driven action into a REAL BUILD STEP: MMK reads a descrip.mms whose
 * action defines a foreign command TCC :== $<TCC.EXE> and then invokes
 *   TCC -x c -c -ffreestanding -fno-builtin -I <tccinc> -I . VMS_STRING.C -o VMS_STRING.OBJ
 * so the persistent DCL MMK spawned fork()+execve()s the static TCC.EXE
 * (dcl_cmd_process.c dcl_exec_foreign_command -> dcl_activate_image: a plain
 * static image is not in-process-eligible, so imgact_activate returns
 * SS$_UNSUPPORTED and DCL forks it -- exactly as it activates any real utility),
 * and TCC compiles the REAL src/libvmssys freestanding runtime TU vms_string.c
 * to an object. This is the FIRST-EVER TCC.EXE run inside QEMU, and the first
 * time MMK drives a real toolchain step (not a DCL builtin) end to end.
 *
 * INDEPENDENT ORACLE (Rule 11 / veracity). The parent never runs a compiler.
 * The only way VMS_STRING.OBJ appears -- a valid ELF relocatable that carries
 * the symbol "vms_strlen" -- is if the DCL MMK spawned actually READ the action
 * lines out of its SYS$INPUT mailbox, DEFINED the foreign command, ACTIVATED
 * TCC.EXE, and TCC actually COMPILED the staged VMS_STRING.C in the guest. There
 * is nowhere local for that object to come from (the parent stages only the .C
 * source + headers; the .OBJ is produced in-guest by the driven compiler). The
 * "OVMXD1B:COMPILED" marker MMK echoes proves the drive reached the end of the
 * action list with a success $STATUS (the $STATUS-capture half of the protocol,
 * as spine #4's completion assertion).
 *
 * DETERMINISM (byte-identical twice). MMK drives the SAME build TWICE, in two
 * independent work directories, and the parent cmp's the two objects -- the
 * byte-identical-twice bar for the build OUTPUT, now asserted end-to-end through
 * the mailbox drive (the compile determinism itself is proven on the host by
 * tests/toolchain/run_tcc_static_component.sh and run_tcc_selfhost.sh).
 *
 * SCOPE (Rule 9 -- the self-host chain, now COMPLETE in-guest). This drives the
 * full compile -> archive -> LINK -> activate chain against a real /dev/vms.
 * Only DECC$SHR.EXE is `--use`d (the component is freestanding: its sole external
 * symbol is vms_strlen, defined in the .OLB, so the executable's only cross-image
 * import is crt0/exit from DECC$SHR); the SYS$LIBRARY: logical-name form the
 * host-side OVMXRT.MMS uses is an authenticity nicety, not needed for a real
 * chain (the drive passes an ABSOLUTE --use path, the same way the compile/
 * archive steps pass absolute TCC/LIBRARIAN paths and mk_dcl.sh passes absolute
 * shareable paths). With this green, BUILD.COM is retired for the MMK path
 * (docs/design-self-host-spine5-mmk-component.md).
 *
 * NO EXECUTIVE (honest-failure branch, Rule 9 / INV-6): $CREMBX must fail
 * SS$_NOSUCHDEV, never fabricate a private per-process mailbox. Returns
 * EXIT_SKIP (77) there -- the contract ci.yml's kernel-executive-negative-
 * control job holds every test_syssvc_* suite to.
 *
 * NEGATIVE CONTROL (facility_defects.sh mmk-drive-command-not-sent, suites_red
 * extended to this suite): forcing ovmx_mmk_sp.c's sp_send to forward ZERO bytes
 * means the spawned DCL receives no action line, TCC is never defined or run, no
 * VMS_STRING.OBJ is produced and no marker returns -- this suite reddens -- while
 * a no-executive run still honest-skips.
 *
 * BOUNDED, NO SLEEPS. The parent poll()s MMK's stdout with a generous failure
 * bound and reaps MMK with a bounded loop, so a wedged drive is a NAMED FAIL
 * here rather than an unattributable VM-level timeout.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <stdint.h>
#include <time.h>

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/* MMK.EXE + TCC.EXE + LIBRARIAN.EXE are staged at SYS$SYSTEM (tests/qemu/Dockerfile). */
#define MMK_PATH_DEFAULT  "/vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE"
#define TCC_PATH_DEFAULT  "/vms/SYS0/SYSCOMMON/SYSEXE/TCC.EXE"
/* LIBRARIAN.EXE (self-host spine #7, vms-6be): the OVMX object-library utility
 * MMK drives to ARCHIVE the two driven objects into OVMXRT.OLB. Like TCC.EXE it
 * is a plain static (musl) foreign command DCL forks (dcl_activate_image ->
 * SS$_UNSUPPORTED -> fork+execve), so no IMGACT/shareable staging is needed. */
#define LIBR_PATH_DEFAULT "/vms/SYS0/SYSCOMMON/SYSEXE/LIBRARIAN.EXE"
/* LINK.EXE (self-host spine #7 FINAL rung, vms-725): the OVMX linker MMK drives
 * (after the archive) to LINK OVMXRT.OLB's members + the driver into a runnable
 * OVMX image OVMXRT.EXE. Also a plain static (musl) foreign command DCL forks.
 * The linked image carries PT_INTERP=/vms/.../IMGACT.EXE; the harness then
 * ACTIVATES it (exec -> kernel loads IMGACT.EXE, which maps DECC$SHR.EXE from
 * SYS$LIBRARY and binds the image's one import, process exit). */
#define LNK_PATH_DEFAULT  "/vms/SYS0/SYSCOMMON/SYSEXE/LINK.EXE"
/* DECC$SHR.EXE (vms-725): the OVMX C run-time shareable the executable link's
 * `--use` binds (crt0 + process exit); staged at SYS$LIBRARY, where LINK opens
 * it at link time and IMGACT resolves it at activation. */
#define DECCSHR_PATH_DEFAULT "/vms/SYS0/SYSCOMMON/SYSLIB/DECC$SHR.EXE"
/* The runnable image's oracle exit status: vms_strlen("OVMXRT")==6, 6*36==216.
 * There is nowhere else for 216 to come from -- it proves the driven LINK pulled
 * VMS_STRING out of the .OLB AND the produced image actually activated + ran. */
#define EXPECT_EXIT 216
/* tinycc's own headers (stddef.h/stdint.h/...) staged beside TCC.EXE so tcc's
 * default {tccdir}/include search AND the explicit -I both find them. */
#define TCC_INCLUDE_DEFAULT "/vms/SYS0/SYSCOMMON/SYSEXE/include"
/* The real component source + its headers, staged by the Dockerfile. */
#define COMPONENT_DEFAULT "/tests/component"

/* Failure bound (ms) for ONE drive: MMK spawns a DCL, drives the foreign-command
 * definition + the TCC compile (a real fork+execve of the compiler, which does
 * real work) + the marker echo, and EXITS. This is a CEILING, not pacing -- the
 * wait below returns the instant MMK exits, so a green drive costs only its real
 * runtime and this bound is only ever reached by a genuine hang. It is generous
 * because CI's TCG is much slower than a dev host: an earlier run compiled the
 * object and echoed the marker but MMK had not yet finished tearing down its DCL
 * subprocess within a tight 2s reap, reddening the run from clean (the whole
 * point of the vms-d1b gate is to pass from a clean CI build). Kept comfortably
 * under half run_tests.sh's 120s QEMU cap, and paired with the single-drive
 * short-circuit in main() (drive #2 is skipped only when drive #1 WEDGED), so
 * even a genuine hang costs one bound, not four. */
#define DRIVE_TIMEOUT_MS 40000

#define EXPECT_MARKER "OVMXD1B:COMPILED"

static int pass = 0, fail = 0;
#define CHECK(cond, msg) do { \
    if (cond) { printf("  PASS: %s\n", msg); pass++; } \
    else { printf("  FAIL: %s\n", msg); fail++; } \
} while (0)

static const char *envdef(const char *name, const char *dflt)
{
    const char *p = getenv(name);
    return (p && p[0]) ? p : dflt;
}

static struct dsc$descriptor_s mkdsc(const char *s)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(s);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)s;
    return d;
}

static int executive_present(void)
{
    int fd = vms_kif_open();
    if (fd < 0) return 0;
    vms_kif_close();
    return 1;
}

static int write_file(const char *path, const char *contents)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    size_t n = strlen(contents);
    int ok = (fwrite(contents, 1, n, f) == n);
    if (fclose(f) != 0) ok = 0;
    return ok ? 0 : -1;
}

/* Copy a file (source staging). Returns 0 on success. */
static int copy_file(const char *src, const char *dst)
{
    FILE *in = fopen(src, "rb");
    if (!in) return -1;
    FILE *out = fopen(dst, "wb");
    if (!out) { fclose(in); return -1; }
    char buf[8192];
    size_t n;
    int ok = 1;
    while ((n = fread(buf, 1, sizeof(buf), in)) > 0)
        if (fwrite(buf, 1, n, out) != n) { ok = 0; break; }
    if (fclose(out) != 0) ok = 0;
    fclose(in);
    return ok ? 0 : -1;
}

/* Read a whole file into a malloc'd buffer. Returns bytes read, or -1. */
static long read_file(const char *path, char **out)
{
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
    long sz = ftell(f);
    if (sz < 0) { fclose(f); return -1; }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return -1; }
    long got = (long)fread(buf, 1, (size_t)sz, f);
    fclose(f);
    if (got != sz) { free(buf); return -1; }
    buf[sz] = '\0';
    *out = buf;
    return sz;
}

/* Valid ELF relocatable (ET_REL)? */
static int is_elf_reloc(const char *buf, long len)
{
    if (len < 20) return 0;
    if (!(buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F'))
        return 0;
    /* e_type is a 2-byte field at offset 16 (little-endian on our targets). */
    uint16_t etype = (uint16_t)((unsigned char)buf[16] | ((unsigned char)buf[17] << 8));
    return etype == 1; /* ET_REL */
}

/* memmem is available in this libc; guard just in case. */
static int contains(const char *hay, long haylen, const char *needle)
{
    size_t nl = strlen(needle);
    if (haylen < (long)nl) return 0;
    for (long i = 0; i + (long)nl <= haylen; i++)
        if (memcmp(hay + i, needle, nl) == 0) return 1;
    return 0;
}

/*
 * Activate a produced OVMX image at `exepath` (fork+exec -> kernel PT_INTERP ->
 * IMGACT.EXE maps it + DECC$SHR, runs crt0 -> main) and return its exit status,
 * or -1 if it could not be run. Bounded wait so a wedged activation is a named
 * failure here, not a VM-level timeout.
 */
static int activate_image(const char *exepath)
{
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        execl(exepath, exepath, (char *)NULL);
        _exit(126);   /* exec failed -> not 216, a clear activation failure */
    }
    int status = 0, waited = 0;
    while (waited < 10000) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        if (r < 0) return -1;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        waited += 50;
    }
    kill(pid, SIGKILL); (void)waitpid(pid, &status, 0);
    return -1;   /* activation wedged */
}

/*
 * Drive ONE MMK build of OVMXRT.EXE in a fresh work directory. The driven MMS
 * compiles TWO real runtime TUs (VMS_STRING.C + VMS_SNPRINTF.C) + the driver
 * OVMXRTRUN.C with TCC.EXE, archives the two runtime objects into OVMXRT.OLB with
 * LIBRARIAN.EXE, and LINKs OVMXRTRUN.OBJ + OVMXRT.OLB into the runnable image
 * OVMXRT.EXE with LINK.EXE (--use DECC$SHR.EXE). On success (reaped set):
 * objbuf/objlen hold VMS_STRING.OBJ (the compile oracle), olbbuf/olblen hold
 * OVMXRT.OLB (the archive oracle), exebuf/exelen hold OVMXRT.EXE (the link
 * oracle); *activation_rc is the produced image's exit status (the harness
 * activates it before cleanup); saw_marker says whether the drive echoed the
 * completion marker. Caller frees objbuf/olbbuf/exebuf.
 */
static void drive_build(const char *mmk, const char *comp, const char *tcc,
                        const char *tccinc, const char *libr,
                        const char *lnk, const char *deccshr,
                        char **objbuf, long *objlen,
                        char **olbbuf, long *olblen,
                        char **exebuf, long *exelen,
                        int *activation_rc, int *saw_marker, int *reaped)
{
    *objbuf = NULL; *objlen = -1; *olbbuf = NULL; *olblen = -1;
    *exebuf = NULL; *exelen = -1; *activation_rc = -1;
    *saw_marker = 0; *reaped = 0;

    char workdir[] = "/tmp/mmk725_XXXXXX";
    if (!mkdtemp(workdir)) return;

    char path[512], src[512];

    /* Stage the REAL component sources (VMS-style upper-case .C) + their headers.
     * VMS_STRING.C and VMS_SNPRINTF.C are the two src/libvmssys runtime TUs that
     * ARE fully tcc-compilable on x86_64 (vms_math.c's SSE "x"-constraint inline
     * asm is not, so it is excluded from the in-guest 2-TU library); OVMXRTRUN.C
     * is the freestanding driver whose main() calls vms_strlen -> the LINK pulls
     * VMS_STRING from the .OLB and the activated image exits 216. */
    static const char *srcs[] = { "VMS_STRING.C", "VMS_SNPRINTF.C",
                                  "OVMXRTRUN.C", NULL };
    for (int i = 0; srcs[i]; i++) {
        snprintf(src,  sizeof(src),  "%s/%s", comp, srcs[i]);
        snprintf(path, sizeof(path), "%s/%s", workdir, srcs[i]);
        if (copy_file(src, path) != 0) return;
    }
    /* vms_string.h + vms_snprintf.h + vms_types.h are the TUs' + driver's only
     * component includes; stage the whole staged header set. */
    static const char *hdrs[] = { "vms_string.h", "vms_snprintf.h",
                                  "vms_types.h", NULL };
    for (int i = 0; hdrs[i]; i++) {
        snprintf(src,  sizeof(src),  "%s/%s", comp, hdrs[i]);
        snprintf(path, sizeof(path), "%s/%s", workdir, hdrs[i]);
        if (copy_file(src, path) != 0) return;
    }

    /* The descrip.mms: define the TCC + LIBRARIAN + LNK foreign commands (absolute
     * image paths, so they resolve in the spawned DCL without depending on
     * SYS$SYSTEM), compile the two real TUs + the driver, archive the two runtime
     * objects into OVMXRT.OLB, LINK the driver + .OLB into the runnable OVMXRT.EXE,
     * then echo the completion marker. Each TAB-indented line is one command record
     * MMK streams into the spawned DCL -- the same whole-line-raw delivery spine #6
     * proved for the single compile, extended to the full compile->archive->link
     * chain.
     *
     * WHY THE FOREIGN-COMMAND NAMES `LIBRARIAN` AND `LNK` (not `LIBR`/`LINK`). A
     * DCL foreign-command symbol cannot shadow a built-in DCL verb: `LIBR` is a
     * valid abbreviation of the built-in LIBRARY verb, and `LINK` IS the built-in
     * LINK verb (dcl_builtin.c min_abbrev=3 for both) -- either would run the
     * in-process cmd_library()/cmd_link() (which resolve file names against the
     * VMS default directory, NOT the Linux cwd where the forked toolchain wrote
     * them). `LIBRARIAN` (9 chars) and `LNK` (not a prefix of LINK: position 2 is
     * N vs I) are not prefixes of any built-in verb (dcl_match_command requires
     * typed_len <= verb_len), so they fall through to the foreign-command symbols
     * and fork the staged LIBRARIAN.EXE / LINK.EXE images -- which DO open the
     * files in the fork's cwd. OVMXRT.MMS already documents this trap for `LNK`;
     * vms-6be hit it for LIBRARY.
     *
     * The image paths are QUOTED so DCL's :== assignment preserves their case
     * (dcl_exec.c exec_assign: an unquoted :== value is upcased, which would turn
     * /vms into /VMS and miss the case-sensitive Linux mount). Each foreign-command
     * TAIL below is delivered raw (dcl_exec_foreign_command uses cmd->raw_tail), so
     * tcc's case-sensitive flags, the lowercase -I path, LIBRARIAN's /CREATE, and
     * LINK's --executable/--use flags + the absolute DECC$SHR.EXE path (the `$` is
     * an ordinary VMS filename character here, not an apostrophe substitution)
     * survive as-is -- the same whole-line-raw delivery native LINK relies on. */
    char mms[2560];
    snprintf(mms, sizeof(mms),
        "OVMXRT.EXE : VMS_STRING.C VMS_SNPRINTF.C OVMXRTRUN.C\n"
        "\tTCC :== \"$%s\"\n"
        "\tLIBRARIAN :== \"$%s\"\n"
        "\tLNK :== \"$%s\"\n"
        "\tTCC -x c -c -ffreestanding -fno-builtin -I %s -I . VMS_STRING.C -o VMS_STRING.OBJ\n"
        "\tTCC -x c -c -ffreestanding -fno-builtin -I %s -I . VMS_SNPRINTF.C -o VMS_SNPRINTF.OBJ\n"
        "\tTCC -x c -c -ffreestanding -fno-builtin -I %s -I . OVMXRTRUN.C -o OVMXRTRUN.OBJ\n"
        "\tLIBRARIAN /CREATE OVMXRT.OLB VMS_STRING.OBJ VMS_SNPRINTF.OBJ\n"
        "\tLNK --executable --use %s -o OVMXRT.EXE OVMXRTRUN.OBJ OVMXRT.OLB\n"
        "\tWRITE SYS$OUTPUT \"%s\"\n",
        tcc, libr, lnk, tccinc, tccinc, tccinc, deccshr, EXPECT_MARKER);
    snprintf(path, sizeof(path), "%s/725.MMS", workdir);
    if (write_file(path, mms) != 0) return;
    /* /RULES defaults to MMS$RULES; an empty one keeps the run's status clean. */
    snprintf(path, sizeof(path), "%s/MMS$RULES", workdir);
    if (write_file(path, "! empty default rules (vms-725 build drive)\n") != 0) return;

    int outpipe[2];
    if (pipe(outpipe) < 0) return;

    pid_t pid = fork();
    if (pid < 0) { close(outpipe[0]); close(outpipe[1]); return; }
    if (pid == 0) {
        if (chdir(workdir) != 0) _exit(120);
        dup2(outpipe[1], STDOUT_FILENO);
        dup2(outpipe[1], STDERR_FILENO);
        close(outpipe[0]); close(outpipe[1]);
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); close(devnull); }
        setenv("VMS_FOREIGN_CMD", "/DESCRIPTION=725.MMS OVMXRT.EXE", 1);
        execl(mmk, mmk, (char *)NULL);
        _exit(127);
    }
    close(outpipe[1]);

    /* The runnable image the drive is meant to produce (the FINAL artifact of the
     * compile->archive->link chain), plus the string object (compile oracle) and
     * the .OLB (archive oracle). */
    char objpath[512], olbpath[512], exepath[512];
    snprintf(objpath, sizeof(objpath), "%s/VMS_STRING.OBJ", workdir);
    snprintf(olbpath, sizeof(olbpath), "%s/OVMXRT.OLB",     workdir);
    snprintf(exepath, sizeof(exepath), "%s/OVMXRT.EXE",     workdir);

    /* ONE bounded wait that stops as soon as THE PROOF IS CAPTURED -- the DCL has
     * echoed the completion marker AND the final archive OVMXRT.OLB exists on disk
     * -- rather than waiting on MMK's own process EXIT. This is deliberate and is
     * what makes the suite robust under slow/contended TCG (CI, or a busy dev
     * host): MMK's teardown-and-exit AFTER the build can take much longer than the
     * build itself under load, but the veracity the suite asserts (a byte-
     * identical real image, driven over the mailbox to the marker) is already on
     * disk by then. Keying on OVMXRT.EXE (produced LAST, by the LINK step) --
     * not an intermediate object/archive -- guarantees the drive is not killed as
     * cleanup until the LINK has actually completed. Once both are present, MMK is
     * killed as CLEANUP -- its self-exit timing is NOT a pass condition (that made
     * the gate flaky: an earlier run built the artifact and echoed the marker but
     * had not finished exiting within the bound). A genuine mid-drive $HIBER
     * deadlock still fails HARD: no marker is ever echoed, so *saw_marker stays 0
     * and the bound elapses. */
    char acc[16384];
    size_t acclen = 0;
    int waited = 0;
    int wstatus = 0;
    struct stat exest;
    acc[0] = '\0';
    while (waited < DRIVE_TIMEOUT_MS) {
        struct pollfd pfd = { .fd = outpipe[0], .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr > 0) {
            ssize_t n = read(outpipe[0], acc + acclen,
                             (acclen < sizeof(acc) - 1) ? (sizeof(acc) - 1 - acclen) : 0);
            if (n > 0) {
                acclen += (size_t)n;
                acc[acclen] = '\0';
                if (strstr(acc, EXPECT_MARKER) != NULL) *saw_marker = 1;
            }
        }
        /* Proof captured: marker echoed AND the final runnable image is on disk. */
        if (*saw_marker && stat(exepath, &exest) == 0 && exest.st_size > 0)
            break;
        pid_t r = waitpid(pid, &wstatus, WNOHANG);
        if (r == pid) { *reaped = 1; break; }   /* MMK exited on its own */
        if (r < 0) break;
        waited += 200;
    }
    /* Kill MMK if still running -- CLEANUP, not a verdict (see above). */
    if (waitpid(pid, &wstatus, WNOHANG) == 0) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, &wstatus, 0);
    } else {
        *reaped = 1;
    }
    close(outpipe[0]);

    /* Read the produced artifacts (the independent oracles): VMS_STRING.OBJ (the
     * compile oracle), OVMXRT.OLB (the archive oracle), OVMXRT.EXE (the link
     * oracle). */
    *objlen = read_file(objpath, objbuf);
    *olblen = read_file(olbpath, olbbuf);
    *exelen = read_file(exepath, exebuf);

    /* ACTIVATE the produced image BEFORE cleanup (the activation oracle): the
     * harness fork+execs OVMXRT.EXE, whose PT_INTERP=/vms/.../IMGACT.EXE makes the
     * kernel activate it through IMGACT (which maps DECC$SHR from SYS$LIBRARY and
     * binds the one cross-image import), and captures its exit status -- 216 iff
     * the driven LINK produced a real image that really runs. */
    if (*exelen > 0) {
        (void)chmod(exepath, 0755);   /* defensive: ensure exec bit for the fork+exec */
        *activation_rc = activate_image(exepath);
    }

    /* On a failed drive, surface the driven-DCL transcript so a regression is
     * attributable (a cwd / path / link error is named here rather than showing
     * only "artifact never appeared"). Bounded: acc is capped above. */
    if (*exelen <= 0 || *olblen <= 0 || *objlen <= 0) {
        printf("  (drive did not produce OVMXRT.EXE; driven-DCL transcript follows)\n");
        printf("----8<----\n%s\n---->8----\n", acc);
    }

    /* Cleanup. */
    const char *rm[] = { "VMS_STRING.C", "VMS_SNPRINTF.C", "OVMXRTRUN.C",
                         "VMS_STRING.OBJ", "VMS_SNPRINTF.OBJ", "OVMXRTRUN.OBJ",
                         "OVMXRT.OLB", "OVMXRT.EXE", "vms_string.h",
                         "vms_snprintf.h", "vms_types.h", "725.MMS", "MMS$RULES",
                         NULL };
    for (int i = 0; rm[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", workdir, rm[i]);
        unlink(path);
    }
    rmdir(workdir);
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_mmk_build (MMK.EXE drives TCC compile + LIBRARIAN archive + LINK to a runnable image, IMGACT-activated to 216, in QEMU byte-identical twice, vms-725) ===\n");

    if (!vms_pcb_init(0xFFFFFFFFFFFFFFFFULL)) {
        printf("  FAIL: vms_pcb_init() failed\n");
        return 1;
    }

    if (!executive_present()) {
        uint16_t chan = 0;
        struct dsc$descriptor_s lognam = mkdsc("SYS$INPUT");
        uint32_t st = sys$crembx(0, &chan, 0, 0, 0, 0, &lognam);
        CHECK(st == SS$_NOSUCHDEV,
              "no executive: $CREMBX fails SS$_NOSUCHDEV, never a local per-process fallback");
        printf("=== test_syssvc_mmk_build: %d passed, %d failed (SKIPPED: no /dev/vms -- MMK build drive not exercised) ===\n",
               pass, fail);
        return fail > 0 ? 1 : EXIT_SKIP;
    }

    const char *mmk     = envdef("OVMX_MMK", MMK_PATH_DEFAULT);
    const char *tcc     = envdef("OVMX_TCC", TCC_PATH_DEFAULT);
    const char *libr    = envdef("OVMX_LIBR", LIBR_PATH_DEFAULT);
    const char *lnk     = envdef("OVMX_LNK", LNK_PATH_DEFAULT);
    const char *deccshr = envdef("OVMX_DECCSHR", DECCSHR_PATH_DEFAULT);
    const char *tccinc  = envdef("OVMX_TCC_INCLUDE", TCC_INCLUDE_DEFAULT);
    const char *comp    = envdef("OVMX_COMPONENT", COMPONENT_DEFAULT);

    struct stat sb;
    if (stat(mmk, &sb)     != 0) { printf("  FAIL: shipped MMK image not found at %s\n", mmk); return 1; }
    if (stat(tcc, &sb)     != 0) { printf("  FAIL: static TCC.EXE not found at %s\n", tcc); return 1; }
    if (stat(libr, &sb)    != 0) { printf("  FAIL: static LIBRARIAN.EXE not found at %s\n", libr); return 1; }
    if (stat(lnk, &sb)     != 0) { printf("  FAIL: static LINK.EXE not found at %s\n", lnk); return 1; }
    if (stat(deccshr, &sb) != 0) { printf("  FAIL: DECC$SHR.EXE not found at %s\n", deccshr); return 1; }

    /* Build #1: an independent MMK drive in its own work dir. */
    char *obj1 = NULL, *obj2 = NULL, *olb1 = NULL, *olb2 = NULL, *exe1 = NULL, *exe2 = NULL;
    long  objlen1 = -1, objlen2 = -1, olblen1 = -1, olblen2 = -1, exelen1 = -1, exelen2 = -1;
    int   act1 = -1, act2 = -1, mark1 = 0, mark2 = 0, reap1 = 0, reap2 = 0;

    drive_build(mmk, comp, tcc, tccinc, libr, lnk, deccshr,
                &obj1, &objlen1, &olb1, &olblen1, &exe1, &exelen1,
                &act1, &mark1, &reap1);

    int obj1_valid = (objlen1 > 0 && obj1 != NULL && is_elf_reloc(obj1, objlen1));
    int olb1_valid = (olblen1 > 8 && olb1 != NULL && memcmp(olb1, "!<arch>\n", 8) == 0);
    /* A valid OVMX image is an ELF ET_DYN (executables are position-independent
     * ET_DYN carrying PT_INTERP=IMGACT.EXE) -- e_type==3 at offset 16. */
    int exe1_valid = (exelen1 > 20 && exe1 != NULL && exe1[0] == 0x7f &&
                      exe1[1] == 'E' && exe1[2] == 'L' && exe1[3] == 'F' &&
                      ((uint16_t)((unsigned char)exe1[16] | ((unsigned char)exe1[17] << 8))) == 3);

    /* Build #2 runs only if drive #1 produced a valid runnable image. Keying the
     * short-circuit on the EXE -- not on MMK's exit -- is robust under load
     * (MMK's slow self-exit does not skip a real second build) AND keeps a failed
     * drive to ONE marker bound: mmk-build-image-not-activated (the driven
     * toolchain never runs -> no object -> no .OLB -> no image) fails FAST on
     * drive #1 and skips #2, and a genuine mid-drive wedge (no marker, no
     * artifact) likewise costs one bound, well inside run_tests.sh's 120s VM
     * budget. */
    if (obj1_valid && olb1_valid && exe1_valid)
        drive_build(mmk, comp, tcc, tccinc, libr, lnk, deccshr,
                    &obj2, &objlen2, &olb2, &olblen2, &exe2, &exelen2,
                    &act2, &mark2, &reap2);
    (void)mark2; (void)reap1; (void)reap2; (void)act2;  /* set for diagnostics, not asserted */

    /* Completion: the drive reached its end and the spawned DCL echoed
     * OVMXD1B:COMPILED back over the mailbox -- proving MMK did NOT $HIBER-
     * deadlock mid-drive (a deadlock echoes no marker). MMK's own process EXIT is
     * deliberately NOT a pass condition here: its teardown timing under contended
     * TCG is not a property this suite tests -- see drive_build. */
    CHECK(mark1,
          "MMK.EXE drove the build to completion: its spawned DCL received the action lines and echoed OVMXD1B:COMPILED (spawn + mailbox + write-attention AST + $HIBER + IO$M_NOW + $STATUS marker)");

    /* --- COMPILE stage (spine #6): the object the DRIVEN TCC produced. --- */
    /* negctl: mmk-build-image-not-activated */
    CHECK(objlen1 > 0 && obj1 != NULL,
          "the MMK-driven TCC.EXE produced VMS_STRING.OBJ in the guest (build #1)");
    CHECK(obj1_valid,
          "the driven object is a valid ELF relocatable (ET_REL) -- TCC really compiled it in QEMU");
    CHECK(objlen1 > 0 && obj1 != NULL && contains(obj1, objlen1, "vms_strlen"),
          "the driven object carries the runtime symbol vms_strlen -- it is a compile of the REAL src/libvmssys/vms_string.c, not a stand-in");
    CHECK(objlen2 > 0 && obj2 != NULL,
          "the MMK-driven TCC.EXE produced VMS_STRING.OBJ in the guest (build #2)");

    /* byte-identical across two independent in-guest MMK-driven builds. */
    CHECK(objlen1 > 0 && objlen1 == objlen2 && obj1 && obj2 && memcmp(obj1, obj2, (size_t)objlen1) == 0,
          "VMS_STRING.OBJ is BYTE-IDENTICAL across two independent MMK-driven in-guest builds (deterministic, zero bash)");

    /* --- ARCHIVE stage (spine #7, vms-6be): the .OLB the DRIVEN LIBRARIAN made.
     * These redden under the SAME mmk-build-image-not-activated root as the
     * compile assertions above (no driven TCC -> no objects -> LIBRARIAN has
     * nothing to archive -> no OVMXRT.OLB), so they are declared in that control's
     * knock_on_fail. --- */
    CHECK(olblen1 > 0 && olb1 != NULL,
          "the MMK-driven LIBRARIAN.EXE produced OVMXRT.OLB in the guest (build #1)");
    CHECK(olb1_valid,
          "OVMXRT.OLB is a valid ar-format object library (!<arch> magic) -- LIBRARIAN really archived it in QEMU");
    CHECK(olblen1 > 0 && olb1 != NULL && contains(olb1, olblen1, "VMS_STRING") && contains(olb1, olblen1, "VMS_SNPRINTF"),
          "OVMXRT.OLB carries both archived members VMS_STRING and VMS_SNPRINTF -- a real 2-TU library, not a single object");
    CHECK(olblen1 > 0 && olb1 != NULL && contains(olb1, olblen1, "vms_strlen") && contains(olb1, olblen1, "vms_snprintf"),
          "OVMXRT.OLB embeds the runtime symbols vms_strlen + vms_snprintf -- its members are compiles of the REAL src/libvmssys TUs");
    CHECK(olblen2 > 0 && olb2 != NULL,
          "the MMK-driven LIBRARIAN.EXE produced OVMXRT.OLB in the guest (build #2)");

    /* byte-identical across two independent in-guest MMK-driven builds -- the
     * `ar`-container mtime/uid/gid reproducibility trap LIBRARIAN zeroes. */
    CHECK(olblen1 > 0 && olblen1 == olblen2 && olb1 && olb2 && memcmp(olb1, olb2, (size_t)olblen1) == 0,
          "OVMXRT.OLB is BYTE-IDENTICAL across two independent MMK-driven in-guest builds (deterministic archive, zero bash)");

    /* --- LINK + ACTIVATE stage (spine #7 FINAL rung, vms-725): the runnable image
     * the DRIVEN LINK made, and the exit status it yields when IMGACT activates
     * it. These redden under the SAME mmk-build-image-not-activated root as the
     * compile/archive assertions (no driven toolchain -> no objects -> no .OLB ->
     * no OVMXRT.EXE -> nothing to activate), so they are declared in that control's
     * knock_on_fail. --- */
    CHECK(exelen1 > 0 && exe1 != NULL,
          "the MMK-driven LINK.EXE produced OVMXRT.EXE in the guest (build #1)");
    CHECK(exe1_valid,
          "OVMXRT.EXE is a valid OVMX image (ELF ET_DYN) -- LINK really linked it in QEMU");
    CHECK(exelen1 > 0 && exe1 != NULL && contains(exe1, exelen1, "/vms/SYS0/SYSCOMMON/SYSEXE/IMGACT.EXE"),
          "OVMXRT.EXE carries PT_INTERP=IMGACT.EXE -- it is an image the kernel activates through IMGACT, not a bare ELF");
    CHECK(act1 == EXPECT_EXIT,
          "IMGACT activated the MMK-driven OVMXRT.EXE and it RAN to exit 216 (vms_strlen(\"OVMXRT\")*36) -- the LINK pulled VMS_STRING from the .OLB and the image really runs");
    CHECK(exelen2 > 0 && exe2 != NULL,
          "the MMK-driven LINK.EXE produced OVMXRT.EXE in the guest (build #2)");

    /* byte-identical across two independent in-guest MMK-driven builds -- the
     * whole compile->archive->link chain is deterministic end to end. */
    CHECK(exelen1 > 0 && exelen1 == exelen2 && exe1 && exe2 && memcmp(exe1, exe2, (size_t)exelen1) == 0,
          "OVMXRT.EXE is BYTE-IDENTICAL across two independent MMK-driven in-guest builds (deterministic link, zero bash)");

    free(obj1); free(obj2); free(olb1); free(olb2); free(exe1); free(exe2);

    printf("=== test_syssvc_mmk_build: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
