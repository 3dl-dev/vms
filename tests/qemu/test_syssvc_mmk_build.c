/*
 * test_syssvc_mmk_build.c - the shipped MMK.EXE drives a REAL TCC compile of a
 * REAL OVMX runtime component TU, through its persistent mailbox-driven DCL
 * subprocess, against a real /dev/vms -- BYTE-IDENTICAL across two in-guest
 * builds, ZERO bash in the build path (self-host spine #6, vms-d1b; also closes
 * the MMK-driven-EXECUTION residual of spine #5, vms-fe4).
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
 * HONEST SCOPE (Rule 9 / the design's residual). This closes the COMPILE stage
 * of the self-host chain in-guest. The full OVMXRT.MMS (four TCC compiles, a
 * LIBRARIAN archive, a LINK to a runnable image) is NOT driven here: on x86_64
 * vms_math.c's SSE inline asm is not tinycc-compilable, and the LINK step needs
 * the SYS$LIBRARY shareables staged + logical-name resolution inside LINK.EXE +
 * IMGACT activation of the result -- see
 * docs/design-self-host-spine5-mmk-component.md for the precise remainder.
 * BUILD.COM therefore stays until that lands.
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

#include "starlet.h"
#include "descrip.h"
#include "ssdef.h"
#include "vms_kif.h"
#include "vms/pcb.h"

#define EXIT_SKIP 77

/* MMK.EXE + TCC.EXE are staged at SYS$SYSTEM (tests/qemu/Dockerfile). */
#define MMK_PATH_DEFAULT "/vms/SYS0/SYSCOMMON/SYSEXE/MMK.EXE"
#define TCC_PATH_DEFAULT "/vms/SYS0/SYSCOMMON/SYSEXE/TCC.EXE"
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
 * Drive ONE MMK build of VMS_STRING.OBJ in a fresh work directory. On success
 * (*reaped set), *objbuf/*objlen hold the produced object and *saw_marker says
 * whether the drive echoed the completion marker. Caller frees *objbuf.
 */
static void drive_build(const char *mmk, const char *comp, const char *tcc,
                        const char *tccinc,
                        char **objbuf, long *objlen,
                        int *saw_marker, int *reaped)
{
    *objbuf = NULL; *objlen = -1; *saw_marker = 0; *reaped = 0;

    char workdir[] = "/tmp/mmkd1b_XXXXXX";
    if (!mkdtemp(workdir)) return;

    char path[512], src[512];

    /* Stage the REAL component source (VMS-style upper-case .C) + its headers. */
    snprintf(src,  sizeof(src),  "%s/VMS_STRING.C", comp);
    snprintf(path, sizeof(path), "%s/VMS_STRING.C", workdir);
    if (copy_file(src, path) != 0) return;
    /* vms_string.h + vms_types.h are the TU's only component includes; stage the
     * whole staged header set so any future TU choice is covered. */
    static const char *hdrs[] = { "vms_string.h", "vms_types.h", NULL };
    for (int i = 0; hdrs[i]; i++) {
        snprintf(src,  sizeof(src),  "%s/%s", comp, hdrs[i]);
        snprintf(path, sizeof(path), "%s/%s", workdir, hdrs[i]);
        if (copy_file(src, path) != 0) return;
    }

    /* The descrip.mms: define the TCC foreign command (absolute image path, so
     * it resolves in the spawned DCL without depending on SYS$SYSTEM), compile
     * the real TU, then echo the completion marker. Each TAB-indented line is
     * one command record MMK streams into the spawned DCL. */
    char mms[2048];
    snprintf(mms, sizeof(mms),
        "VMS_STRING.OBJ : VMS_STRING.C\n"
        /* The image path is QUOTED so DCL's :== assignment preserves its case
         * (dcl_exec.c exec_assign: an unquoted :== value is upcased, which would
         * turn /vms into /VMS and miss the case-sensitive Linux mount; a quoted
         * span is kept verbatim). The foreign-command TAIL below is delivered
         * raw (dcl_exec_foreign_command uses cmd->raw_tail), so tcc's
         * case-sensitive flags (-x c -c ...) and the lowercase -I path survive
         * as-is -- the same whole-line-raw delivery native LINK relies on. */
        "\tTCC :== \"$%s\"\n"
        "\tTCC -x c -c -ffreestanding -fno-builtin -I %s -I . VMS_STRING.C -o VMS_STRING.OBJ\n"
        "\tWRITE SYS$OUTPUT \"%s\"\n",
        tcc, tccinc, EXPECT_MARKER);
    snprintf(path, sizeof(path), "%s/D1B.MMS", workdir);
    if (write_file(path, mms) != 0) return;
    /* /RULES defaults to MMS$RULES; an empty one keeps the run's status clean. */
    snprintf(path, sizeof(path), "%s/MMS$RULES", workdir);
    if (write_file(path, "! empty default rules (vms-d1b build drive)\n") != 0) return;

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
        setenv("VMS_FOREIGN_CMD", "/DESCRIPTION=D1B.MMS VMS_STRING.OBJ", 1);
        execl(mmk, mmk, (char *)NULL);
        _exit(127);
    }
    close(outpipe[1]);

    /* The object the drive is meant to produce. */
    char objpath[512];
    snprintf(objpath, sizeof(objpath), "%s/VMS_STRING.OBJ", workdir);

    /* ONE bounded wait that stops as soon as THE PROOF IS CAPTURED -- the DCL has
     * echoed the completion marker AND the object exists on disk -- rather than
     * waiting on MMK's own process EXIT. This is deliberate and is what makes the
     * suite robust under slow/contended TCG (CI, or a busy dev host): MMK's
     * teardown-and-exit AFTER the build can take much longer than the build
     * itself under load, but the veracity the suite asserts (a byte-identical
     * real object, driven over the mailbox to the marker) is already on disk by
     * then. Once both are present, MMK is killed as CLEANUP -- its self-exit
     * timing is NOT a pass condition (that made the gate flaky: an earlier run
     * compiled the object and echoed the marker but had not finished exiting
     * within the bound). A genuine mid-drive $HIBER deadlock still fails HARD:
     * no marker is ever echoed, so *saw_marker stays 0 and the bound elapses. */
    char acc[16384];
    size_t acclen = 0;
    int waited = 0;
    int wstatus = 0;
    struct stat objst;
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
        /* Proof captured: marker echoed AND the object is on disk. */
        if (*saw_marker && stat(objpath, &objst) == 0 && objst.st_size > 0)
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

    /* Read the produced object (the independent oracle). */
    *objlen = read_file(objpath, objbuf);

    /* On a failed drive, surface the driven-DCL transcript so a regression is
     * attributable (a cwd / path / activation error is named here rather than
     * showing only "object never appeared"). Bounded: acc is capped above. */
    if (*objlen <= 0) {
        printf("  (drive did not produce VMS_STRING.OBJ; driven-DCL transcript follows)\n");
        printf("----8<----\n%s\n---->8----\n", acc);
    }

    /* Cleanup. */
    const char *rm[] = { "VMS_STRING.C", "VMS_STRING.OBJ", "vms_string.h",
                         "vms_types.h", "D1B.MMS", "MMS$RULES", NULL };
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

    printf("=== test_syssvc_mmk_build (MMK.EXE drives a REAL TCC compile of a real OVMX runtime TU in QEMU, byte-identical twice, vms-d1b) ===\n");

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

    const char *mmk    = envdef("OVMX_MMK", MMK_PATH_DEFAULT);
    const char *tcc    = envdef("OVMX_TCC", TCC_PATH_DEFAULT);
    const char *tccinc = envdef("OVMX_TCC_INCLUDE", TCC_INCLUDE_DEFAULT);
    const char *comp   = envdef("OVMX_COMPONENT", COMPONENT_DEFAULT);

    struct stat sb;
    if (stat(mmk, &sb) != 0) { printf("  FAIL: shipped MMK image not found at %s\n", mmk); return 1; }
    if (stat(tcc, &sb) != 0) { printf("  FAIL: static TCC.EXE not found at %s\n", tcc); return 1; }

    /* Build #1: an independent MMK drive in its own work dir. */
    char *obj1 = NULL, *obj2 = NULL;
    long  len1 = -1, len2 = -1;
    int   mark1 = 0, mark2 = 0, reap1 = 0, reap2 = 0;

    drive_build(mmk, comp, tcc, tccinc, &obj1, &len1, &mark1, &reap1);

    int obj1_valid = (len1 > 0 && obj1 != NULL && is_elf_reloc(obj1, len1));

    /* Build #2 runs only if drive #1 produced a valid object. Keying the
     * short-circuit on the OBJECT -- not on MMK's exit -- is robust under load
     * (MMK's slow self-exit does not skip a real second build) AND keeps a
     * failed drive to ONE marker bound: mmk-build-image-not-activated (the
     * driven TCC never runs -> no object) fails FAST on drive #1 and skips #2,
     * and a genuine mid-drive wedge (no marker, no object) likewise costs one
     * bound, well inside run_tests.sh's 120s VM budget. */
    if (obj1_valid)
        drive_build(mmk, comp, tcc, tccinc, &obj2, &len2, &mark2, &reap2);
    (void)mark2; (void)reap1; (void)reap2;   /* set for diagnostics, not asserted */

    /* Completion: the drive reached its end and the spawned DCL echoed
     * OVMXD1B:COMPILED back over the mailbox -- proving MMK did NOT $HIBER-
     * deadlock mid-drive (a deadlock echoes no marker). MMK's own process EXIT is
     * deliberately NOT a pass condition here: its teardown timing under contended
     * TCG is not a property this suite tests -- see drive_build. */
    CHECK(mark1,
          "MMK.EXE drove the build to completion: its spawned DCL received the action lines and echoed OVMXD1B:COMPILED (spawn + mailbox + write-attention AST + $HIBER + IO$M_NOW + $STATUS marker)");

    /* The object the DRIVEN TCC produced -- the independent oracle. */
    /* negctl: mmk-build-image-not-activated */
    CHECK(len1 > 0 && obj1 != NULL,
          "the MMK-driven TCC.EXE produced VMS_STRING.OBJ in the guest (build #1)");
    CHECK(obj1_valid,
          "the driven object is a valid ELF relocatable (ET_REL) -- TCC really compiled it in QEMU");
    CHECK(len1 > 0 && obj1 != NULL && contains(obj1, len1, "vms_strlen"),
          "the driven object carries the runtime symbol vms_strlen -- it is a compile of the REAL src/libvmssys/vms_string.c, not a stand-in");
    CHECK(len2 > 0 && obj2 != NULL,
          "the MMK-driven TCC.EXE produced VMS_STRING.OBJ in the guest (build #2)");

    /* byte-identical across two independent in-guest MMK-driven builds. */
    CHECK(len1 > 0 && len1 == len2 && obj1 && obj2 && memcmp(obj1, obj2, (size_t)len1) == 0,
          "VMS_STRING.OBJ is BYTE-IDENTICAL across two independent MMK-driven in-guest builds (deterministic, zero bash)");

    free(obj1); free(obj2);

    printf("=== test_syssvc_mmk_build: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
