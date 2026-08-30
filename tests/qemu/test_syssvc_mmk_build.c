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
 *   1. TCC compiles the real src/libvmssys runtime TU VMS_STRING.C AND the driver
 *      OVMXRTRUN.C (only ONE runtime TU per drive, to stay under the whole-VM 120s
 *      QEMU budget on CI's slower runner);
 *   2. LIBRARIAN /CREATEs OVMXRT.OLB from the runtime object;
 *   3. LINK --executable --use DECC$SHR.EXE links OVMXRTRUN.OBJ + OVMXRT.OLB into
 *      OVMXRT.EXE, pulling VMS_STRING from the library (the driver references
 *      vms_strlen).
 * The produced OVMXRT.EXE carries PT_INTERP=/vms/.../IMGACT.EXE; the harness
 * ACTIVATES it (fork+exec -> the kernel loads IMGACT.EXE, which maps DECC$SHR.EXE
 * from SYS$LIBRARY, binds the image's one import, and runs crt0 -> main), and
 * asserts it exits 216 (vms_strlen("OVMXRT")==6, 6*36==216 -- nowhere else for
 * 216 to come from).
 *
 * TWO DRIVES; the compile+archive OUTPUTS are asserted BYTE-IDENTICAL across the
 * two independent in-guest MMK drives (determinism). The LINK+activate runs on
 * drive #2 only (drive #1 is the lighter compile+archive drive); the LINK
 * OUTPUT's byte-identity is host-proven (run_mmk_component_build.sh links twice,
 * cmp-clean). LIBRARIAN archiving MULTIPLE members is likewise host-proven there;
 * in-guest this suite proves the compile->archive->LINK->activate chain end to
 * end. This is self-hosting's final rung: MMK builds a real OVMX component to a
 * running image entirely inside OVMX.
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
 * shareable paths). This green is what let BUILD.COM be retired for the MMK path
 * (vms-e49; docs/design-self-host-spine5-mmk-component.md).
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
#include "rmsdef.h"
#include "rms/rms.h"
#include "vms_kif.h"
#include "vms/pcb.h"
#include "vmsfs/ods2.h"   /* ODS2_FK_* file-kind selectors for IO$_CREATE */

#define EXIT_SKIP 77

/* THE BUILD VOLUME (vms-dff). The atomic flip (epic vms-208) made OVMX RMS
 * ACP-ONLY when /dev/vms is present -- sys$open rides the Files-11 ODS-2 ACP over
 * /dev/vms with NO /vms POSIX passthrough (Rule 9 / INV-6; src/vmsrms/rms_core.c
 * rms_acp_absent()). MMK.EXE opens its description / rules through that SAME RMS
 * (readdesc.c file_open -> sys$open), so those files must live on a genuine
 * mounted ODS-2 volume, not a Linux /tmp tmpfs (MMK's $OPEN would walk the volume
 * and honestly miss them -> RMS$_FNF -> MMK__NOOPNDSC). This suite authors the
 * description + rules on the harness's writable VDA0: fixture (the same volume
 * test_syssvc_rms_acp.c uses) through the public RMS services, and hands MMK a
 * full ODS-2 /DESCRIPTION= spec. The DRIVEN TOOLCHAIN is unaffected: TCC.EXE /
 * LIBRARIAN.EXE / LINK.EXE are fork()+execve()'d native images doing their own
 * POSIX file I/O in the Linux work directory, exactly as before -- only MMK's own
 * RMS reach into its description moved onto the volume. The rule is dependency-
 * free and its target is a BARE name (all MMK's lib$tparse accepts; a device or
 * directory token reddens MMK__PARSERR), so MMK just builds it -- its MFD stat
 * misses (the MFD is read-only, but a stat is a READ) -> it runs the action. */
#define ODS2_UNIT  "VDA0:"
#define ODS2_DIR   "VDA0:[OVMXDIR]"

/* THE SYSTEM VOLUME the produced image is ACTIVATED off (vms-104 rungs
 * ii/iii/iv). The atomic flip made IMGACT read every image (the main image + its
 * DECC$SHR shareable) over the Files-11 ACP from the DISCOVERED system device --
 * no /vms POSIX read (Rule 9 / INV-6). The MMK-driven LINK writes OVMXRT.EXE into
 * a Linux work directory, so to activate it the harness (1) WRITES the produced
 * bytes onto this generated ODS-2 system volume at [SYS0.SYSCOMMON.SYSEXE]
 * OVMXRT.EXE over the ACP (create + IO$_WRITEVBLK), (2) STAGES a POSIX copy at
 * OVMX_BOOT_STAGE_DIR (the Linux-exec handoff the kernel maps), and (3) execs the
 * staged copy with OVMX_SYSDEVICE pointing IMGACT at THIS volume -- IMGACT's
 * imgsrc_map_staged() rewrites the staged path back to the SYS$SYSTEM volume
 * location and reads the GENUINE bytes over the ACP. DECC$SHR.EXE lives on this
 * volume too (mastered by mkimage_ods2_sysvol). The clean-room real-VAX VDA0:
 * fixture is NEVER mutated with OVMX toolchain/image files (vms-29ff). */
#define SYSVOL_UNIT       "VDA300:"
#define SYSVOL_IMAGE_NAME "OVMXRT.EXE"
/* The staged POSIX copy the kernel execs; imgsrc_map_staged() maps the
 * OVMX_BOOT_STAGE_PREFIX back to /vms/SYS0/SYSCOMMON/SYSEXE/ and IMGACT then
 * reads the on-volume copy over the ACP. Must match src/imgact/imgact.c's
 * IMGACT_BOOT_STAGE_PREFIX. */
#define STAGE_DIR         "/run/ovmx-boot"
#define STAGED_IMAGE_PATH STAGE_DIR "/" SYSVOL_IMAGE_NAME

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

/*
 * THE VMS FILESPECS THE DRIVEN DCL RESOLVES OVER THE ACP (vms-104, final rung).
 *
 * The descrip.mms names the toolchain + the C run-time shareable by VMS logical
 * spec -- NOT the /vms POSIX passthrough the atomic flip retired. SYS$SYSTEM: /
 * SYS$SHARE: resolve to the mounted ODS-2 system volume (OVMX_SYSDEVICE=VDA300:,
 * where rung ii mastered them), and:
 *   - a foreign-command TOOL (TCC/LIBRARIAN/LINK.EXE) is resolved by the DCL
 *     foreign-command resolver, which reads its GENUINE bytes off the volume
 *     THROUGH the Files-11 ACP (IO$_READVBLK) and stages them to a POSIX home
 *     the kernel execve()s -- dcl_cmd_process.c dcl_resolve_activatable_acp;
 *   - the --use DECC$SHR.EXE producer is resolved by native LINK.EXE to its
 *     boot-staged (ACP-read) copy in /run/ovmx-boot (staged below by
 *     stage_shareable_over_acp), so its bytes also come off the volume, not /vms.
 *
 * The POSIX *_PATH_DEFAULT paths above stay for the harness's OWN first-hop
 * execs (execl(MMK), the calibration TCC compile) and presence checks -- that is
 * the harness bootstrapping, like the kernel execve'ing PID 1, not the drive's
 * ACP-resolved path under test.
 */
#define TCC_VMSSPEC     "SYS$SYSTEM:TCC.EXE"
#define LIBR_VMSSPEC    "SYS$SYSTEM:LIBRARIAN.EXE"
#define LNK_VMSSPEC     "SYS$SYSTEM:LINK.EXE"
#define DECCSHR_VMSSPEC "SYS$SHARE:DECC$SHR.EXE"

/*
 * DRIVE BUDGET (vms-9d4f: machine-relative, NOT a wall bump).
 *
 * This USED to be one fixed host-wall constant (DRIVE_TIMEOUT_MS = 60000,
 * raised once already from 40000 -- see git history) covering a 300 x
 * poll(200ms) wait for EITHER drive. That is a HOST-WALL budget for work
 * whose cost is dominated by an IN-GUEST TCC compile of the real
 * src/libvmssys/vms_string.c, and under QEMU TCG (no -icount, no KVM -- Rule
 * 9's faithful gate is `docker run`, plain TCG) the guest's own poll-timer
 * tracks host wall while the compile's actual cost tracks host CPU speed.
 * GitHub's TCG runner has been observed ~10x slower than the dev host's, so
 * a fixed 60s constant that is comfortable here can be blown by the SAME
 * compile there -- an emulation-speed flake, not a code regression (bisect +
 * peer review both converged on this; see vms-9d4f). Raising the constant
 * again would only move the same cliff further out (Rule 8: no wall bump).
 *
 * The fix is to stop budgeting in host-wall at all and budget instead
 * relative to a MEASUREMENT of this run's own in-guest compile speed:
 * calibrate_tcc_ms() (below) times a direct (non-DCL) TCC compile of the
 * SAME reference TU (VMS_STRING.C, the SAME flags both drives use for their
 * dominant compile) right before the drives run, and drive_budget_ms() turns
 * that measurement into a per-drive budget with headroom. On a fast host
 * this reproduces the historical ~60s neighborhood (no behavior change); on
 * a slow host the budget grows with the SAME multiplier the guest's own
 * compiler is running under, so it cannot be outrun by TCG speed the way a
 * constant can.
 *
 * Model (see drive_budget_ms): budget = margin(do_link) * t_cal +
 * DRIVE_FIXED_OVERHEAD_MS, floored at CAL_FALLBACK_MS. Both drives compile
 * VMS_STRING.C (398 lines) as their dominant cost, so DRIVE_COMPILE_MARGIN
 * covers drive #1 (compile + archive). do_link additionally compiles the
 * tiny 38-line OVMXRTRUN.C driver, then LINKs and the harness ACTIVATES the
 * result -- real in-guest CPU work under the SAME TCG slowdown the reference
 * compile measures (this file's own history says so: the 40000->60000 raise
 * for this drive was explicitly because "under slow/contended TCG the whole
 * chain can exceed the old bound", i.e. LINK+activate cost is
 * TCG-speed-dependent too, not a fixed add-on). So do_link uses a LARGER
 * margin, DRIVE_COMPILE_MARGIN_LINK, applied to the SAME t_cal -- scaling
 * proportionally with measured guest speed instead of adding a flat
 * constant that (like the old fixed budget itself) cannot adapt to a run
 * slower than whatever host it was tuned on.
 */

/* Ceiling for the calibration compile itself (a single, light, non-DCL
 * fork+exec of TCC on one TU) -- generous so a slow run's calibration is not
 * itself starved, but still bounded so a wedged TCC.EXE is a named failure
 * (calibrate_tcc_ms returns -1) rather than an unattributable hang. */
#define CAL_TIMEOUT_MS 180000

/* If calibration cannot produce a measurement (TCC/staging missing, wedged,
 * non-zero exit), drive_budget_ms falls back to this floor -- the historical
 * fixed bound -- so a calibration hiccup never SHRINKS the real drives'
 * budget below what has always worked here. This is the one place the old
 * constant survives, and only as a floor, never as the ceiling a slow run is
 * held to. */
#define CAL_FALLBACK_MS 60000

/* Multiplier applied to the measured calibration compile time to get
 * drive #1's (compile + archive) budget. Generous (not tight): covers
 * run-to-run compile-time jitter and the small extra cost of compiling
 * through DCL's foreign-command dispatch (fork+execve is the same either
 * way; only DCL's own parse/dispatch sits on top) rather than
 * calibrate_tcc_ms's direct fork+exec, plus the archive step. */
#define DRIVE_COMPILE_MARGIN 3

/* Multiplier for the do_link=1 drive: the same compile, PLUS the tiny
 * OVMXRTRUN.C compile, PLUS LINK (real relocation/symbol-resolution work),
 * PLUS the harness's post-drive activation -- all real in-guest work, all
 * under the same TCG slowdown t_cal measures. Set well above
 * DRIVE_COMPILE_MARGIN (not just +overhead) so this drive's budget keeps
 * scaling with guest speed the way its own history shows it needs to. */
#define DRIVE_COMPILE_MARGIN_LINK 6

/* Fixed per-drive overhead: lib$spawn of the persistent DCL, $CREMBX x2,
 * foreign-command definition + dispatch, and the marker echo. Small and
 * roughly constant relative to the compile itself -- the vms-9d4f diagnosis
 * is explicit that this overhead was never what blew the old 60s bound, the
 * compile was. */
#define DRIVE_FIXED_OVERHEAD_MS 8000

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

/* now_ms - CLOCK_MONOTONIC in milliseconds, for calibration timing. */
static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
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

/* Author a STMLF text file on the mounted ODS-2 volume through the public RMS
 * services ($CREATE + $PUT one record per line -> the Files-11 ACP). STMLF is the
 * flip's text convention (matches the stream-LF file MMK read pre-flip); a VAR
 * file reddens MMK's lib$tparse. Used for MMK's description + rules, which MMK
 * opens through RMS. Returns 0 on success. */
static int create_ods2_text(const char *spec, const char *const *lines)
{
    struct FAB fab = cc$rms_fab;
    struct RAB rab;
    uint32_t st;

    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    fab.fab$b_org = FAB$C_SEQ;
    fab.fab$b_rfm = FAB$C_STMLF;   /* the flip's text convention */
    fab.fab$b_rat = FAB$M_CR;
    fab.fab$w_mrs = 0;
    fab.fab$b_fac = FAB$M_PUT | FAB$M_GET;

    st = sys$create(&fab, 0, 0);
    if (st != RMS$_NORMAL)
        return -1;

    rab = cc$rms_rab;
    rab.rab$l_fab = &fab;
    st = sys$connect(&rab, 0, 0);
    if (st != RMS$_NORMAL) { sys$close(&fab, 0, 0); return -1; }

    for (int i = 0; lines[i]; i++) {
        rab.rab$l_rbf = (char *)lines[i];
        rab.rab$w_rsz = (uint16_t)strlen(lines[i]);
        st = sys$put(&rab, 0, 0);
        if (st != RMS$_NORMAL) { sys$close(&fab, 0, 0); return -1; }
    }

    st = sys$close(&fab, 0, 0);
    return (st == RMS$_NORMAL) ? 0 : -1;
}

/* $ERASE (IO$_DELETE) a file off the ODS-2 volume; best-effort cleanup. */
static void erase_ods2(const char *spec)
{
    struct FAB fab = cc$rms_fab;
    fab.fab$l_fna = (char *)spec;
    fab.fab$b_fns = (uint8_t)strlen(spec);
    (void)sys$erase(&fab, 0, 0);
}

/* Author a text file on the ODS-2 volume from a '\n'-delimited BLOB (each line
 * becomes one VAR record). A trailing '\n' does not emit an empty final record.
 * Returns 0 on success. */
static int create_ods2_blob(const char *spec, const char *blob)
{
    const char *lines[64];
    static char buf[4096];
    size_t bl = strlen(blob);
    if (bl >= sizeof(buf)) return -1;
    memcpy(buf, blob, bl + 1);

    int n = 0;
    char *p = buf;
    while (*p && n < (int)(sizeof(lines) / sizeof(lines[0])) - 1) {
        lines[n++] = p;
        char *nl = strchr(p, '\n');
        if (!nl) break;
        *nl = '\0';
        p = nl + 1;
    }
    lines[n] = NULL;
    return create_ods2_text(spec, lines);
}

/* The [SYS0.SYSCOMMON.SYSEXE] directory FID on SYSVOL_UNIT, resolved once in
 * main() by walking the ACP directory tree. write_produced_image() creates the
 * produced OVMXRT.EXE under this DID. */
static uint16_t g_sysexe_num = 0, g_sysexe_seq = 0;
static uint8_t  g_sysexe_rvn = 0, g_sysexe_nmx = 0;
static int      g_sysvol_ready = 0;

/* Walk MFD -> <dirs[0]>.DIR -> <dirs[1]>.DIR -> ... over the ACP on `chan`,
 * returning the FINAL directory's FID. dirs[] is NULL-terminated (names without
 * the ".DIR" type). Leaves the channel with no accessed file. Returns a VMS
 * status. */
static uint32_t resolve_dir_fid(uint32_t chan, const char *const *dirs,
                                uint16_t *num, uint16_t *seq,
                                uint8_t *rvn, uint8_t *nmx)
{
    uint16_t d_num = 0, d_seq = 0;
    uint8_t  d_rvn = 0, d_nmx = 0;   /* 0/0/0 => MFD */
    for (int i = 0; dirs[i]; i++) {
        struct vms_acp_access_args a;
        memset(&a, 0, sizeof(a));
        a.chan = chan;
        a.did_num = d_num; a.did_seq = d_seq; a.did_rvn = d_rvn; a.did_nmx = d_nmx;
        a.version = 0;
        snprintf(a.name, VMS_ACP_NAME_SIZE, "%s.DIR", dirs[i]);
        uint32_t st = vms_kif_acp_access(&a);
        if (!$VMS_STATUS_SUCCESS(st))
            return st;
        d_num = a.fid_num; d_seq = a.fid_seq; d_rvn = a.fid_rvn; d_nmx = a.fid_nmx;
        (void)vms_kif_acp_deaccess(chan);
    }
    *num = d_num; *seq = d_seq; *rvn = d_rvn; *nmx = d_nmx;
    return SS$_NORMAL;
}

/* Mount SYSVOL_UNIT and resolve [SYS0.SYSCOMMON.SYSEXE]'s FID (into the g_sysexe_*
 * globals). Idempotent; sets g_sysvol_ready on success. Returns 0 on success.
 * The volume is LEFT MOUNTED so IMGACT can $ASSIGN + IO$_ACCESS the produced
 * image off it at activation time. */
static int sysvol_prepare(void)
{
    if (g_sysvol_ready)
        return 0;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_mount(SYSVOL_UNIT)))
        return -1;
    uint32_t chan = 0;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_assign(SYSVOL_UNIT, &chan)) || chan == 0)
        return -1;
    static const char *const tree[] = { "SYS0", "SYSCOMMON", "SYSEXE", NULL };
    uint32_t st = resolve_dir_fid(chan, tree, &g_sysexe_num, &g_sysexe_seq,
                                  &g_sysexe_rvn, &g_sysexe_nmx);
    (void)vms_kif_dassgn(chan);
    if (!$VMS_STATUS_SUCCESS(st) || g_sysexe_num == 0)
        return -1;
    g_sysvol_ready = 1;
    return 0;
}

/* Write `bytes[0..len)` as [SYS0.SYSCOMMON.SYSEXE]<SYSVOL_IMAGE_NAME> on
 * SYSVOL_UNIT, byte-exact, over the executive Files-11 ACP: delete any prior
 * version, IO$_CREATE + IO$_ACCESS(write), IO$_WRITEVBLK the bytes block by block
 * (implicit extend allocates from BITMAP.SYS), padding the final block with
 * zeros. The produced image's valid byte count becomes a whole number of blocks
 * (>= len), which is all IMGACT needs -- it reads header/phdrs/sections/PT_LOAD
 * at offsets < len (imgact_acp_pread clamps at f.valid). Returns 0 on success. */
static int write_produced_image(const uint8_t *bytes, long len)
{
    if (!g_sysvol_ready || len <= 0)
        return -1;
    uint32_t chan = 0;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_assign(SYSVOL_UNIT, &chan)) || chan == 0)
        return -1;

    /* Best-effort: delete any prior versions from an earlier drive in this VM. */
    for (int v = 0; v < 8; v++) {
        struct vms_acp_fileop_args df;
        memset(&df, 0, sizeof(df));
        df.chan = chan; df.func = VMS_ACP_FOP_DELETE; df.modifiers = VMS_ACP_M_DELETE;
        df.did_num = g_sysexe_num; df.did_seq = g_sysexe_seq;
        df.did_rvn = g_sysexe_rvn; df.did_nmx = g_sysexe_nmx;
        df.version = 0;   /* highest */
        strncpy(df.name, SYSVOL_IMAGE_NAME, VMS_ACP_NAME_SIZE - 1);
        if (!$VMS_STATUS_SUCCESS(vms_kif_acp_fileop(&df)))
            break;
    }

    /* IO$_CREATE a fresh ;1 (dir entry + real FID), then IO$_ACCESS it for
     * WRITE by name -- the exact create->access(write)->writevb pattern
     * test_syssvc_acp_create.c proves. */
    struct vms_acp_fileop_args f;
    memset(&f, 0, sizeof(f));
    f.chan = chan; f.func = VMS_ACP_FOP_CREATE; f.modifiers = VMS_ACP_M_CREATE;
    f.kind = ODS2_FK_DATA_FIX;
    f.did_num = g_sysexe_num; f.did_seq = g_sysexe_seq;
    f.did_rvn = g_sysexe_rvn; f.did_nmx = g_sysexe_nmx;
    f.version = 1;
    strncpy(f.name, SYSVOL_IMAGE_NAME, VMS_ACP_NAME_SIZE - 1);
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_fileop(&f))) {
        (void)vms_kif_dassgn(chan);
        return -1;
    }

    struct vms_acp_access_args a;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = g_sysexe_num; a.did_seq = g_sysexe_seq;
    a.did_rvn = g_sysexe_rvn; a.did_nmx = g_sysexe_nmx;
    a.version = 0;   /* highest */
    a.acctl = VMS_ACP_ACCTL_WRITE;
    strncpy(a.name, SYSVOL_IMAGE_NAME, VMS_ACP_NAME_SIZE - 1);
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_access(&a))) {
        (void)vms_kif_dassgn(chan);
        return -1;
    }

    /* IO$_WRITEVBLK block by block (last block zero-padded to 512). */
    static uint8_t blk[512];
    long off = 0;
    uint32_t vbn = 1;
    int ok = 1;
    while (off < len) {
        uint32_t chunk = (len - off > 512) ? 512u : (uint32_t)(len - off);
        if (chunk < 512)
            memset(blk, 0, sizeof(blk));
        memcpy(blk, bytes + off, chunk);
        struct vms_acp_rw_args r;
        memset(&r, 0, sizeof(r));
        r.chan = chan; r.vbn = vbn; r.offset = 0; r.length = 512;
        r.buffer = (uint64_t)(uintptr_t)blk;
        uint32_t st = vms_kif_acp_writevb(&r);
        if (!$VMS_STATUS_SUCCESS(st) || r.xferred != 512) { ok = 0; break; }
        off += chunk; vbn++;
    }
    (void)vms_kif_acp_deaccess(chan);
    (void)vms_kif_dassgn(chan);
    return ok ? 0 : -1;
}

/*
 * stage_from_sysvol_over_acp (vms-104) - read `name` off SYSVOL_UNIT under the
 * directory tree `tree` (e.g. {SYS0,SYSCOMMON,SYSLIB}) THROUGH the executive
 * Files-11 ACP (IO$_ACCESS + IO$_READVBLK) and write it to STAGE_DIR/`name`
 * (mode 0755). Two producers the MMK-driven LINK / the produced image bind by
 * VMS spec resolve to these boot-staged copies:
 *   - SYS$SHARE:DECC$SHR.EXE  -> STAGE_DIR/DECC$SHR.EXE   (LINK's --use producer)
 *   - the produced image's PT_INTERP=/run/ovmx-boot/IMGACT.EXE -> STAGE_DIR/IMGACT.EXE
 * so their bytes come off the ODS-2 volume over the ACP, NEVER a /vms POSIX read
 * (Rule 9 / INV-6). This is the harness half of ovmx_init's boot bridge.
 * Returns 0 on success.
 */
static int stage_from_sysvol_over_acp(const char *const *tree, const char *name)
{
    uint32_t chan = 0;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_mount(SYSVOL_UNIT)))
        return -1;
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_assign(SYSVOL_UNIT, &chan)) || chan == 0)
        return -1;
    uint16_t dnum = 0, dseq = 0; uint8_t drvn = 0, dnmx = 0;
    uint32_t st = resolve_dir_fid(chan, tree, &dnum, &dseq, &drvn, &dnmx);
    if (!$VMS_STATUS_SUCCESS(st) || dnum == 0) { vms_kif_dassgn(chan); return -1; }

    struct vms_acp_access_args a;
    memset(&a, 0, sizeof(a));
    a.chan = chan;
    a.did_num = dnum; a.did_seq = dseq; a.did_rvn = drvn; a.did_nmx = dnmx;
    a.version = 0;   /* highest */
    strncpy(a.name, name, VMS_ACP_NAME_SIZE - 1);
    st = vms_kif_acp_access(&a);
    if (!$VMS_STATUS_SUCCESS(st)) { vms_kif_dassgn(chan); return -1; }

    uint64_t total = a.attr.efblk
        ? (uint64_t)(a.attr.efblk - 1u) * 512u + a.attr.ffbyte : 0;

    char dest[256];
    snprintf(dest, sizeof(dest), "%s/%s", STAGE_DIR, name);
    int fd = open(dest, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    if (fd < 0) { vms_kif_acp_deaccess(chan); vms_kif_dassgn(chan); return -1; }

    uint8_t blk[512];
    uint64_t written = 0; uint32_t vbn = 1; int ok = 1;
    while (written < total) {
        struct vms_acp_rw_args r;
        memset(&r, 0, sizeof(r));
        r.chan = chan; r.vbn = vbn; r.offset = 0; r.length = 512;
        r.buffer = (uint64_t)(uintptr_t)blk;
        if (!$VMS_STATUS_SUCCESS(vms_kif_acp_readvb(&r)) || r.xferred == 0) { ok = 0; break; }
        uint64_t chunk = total - written;
        if (chunk > r.xferred) chunk = r.xferred;
        if (chunk > sizeof(blk)) chunk = sizeof(blk);
        if (write(fd, blk, (size_t)chunk) != (ssize_t)chunk) { ok = 0; break; }
        written += chunk; vbn++;
    }
    (void)fchmod(fd, 0755);
    close(fd);
    vms_kif_acp_deaccess(chan);
    vms_kif_dassgn(chan);
    if (!ok) unlink(dest);
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
 * calibrate_tcc_ms (vms-9d4f) -- measure THIS run's actual in-guest TCC
 * compile speed by timing a direct (non-DCL) fork+exec of TCC.EXE compiling
 * the SAME reference TU (VMS_STRING.C) with the SAME flags drive_build's
 * descrip.mms uses for its dominant compile step. This is deliberately NOT
 * routed through MMK/DCL: it isolates the emulation-speed-dependent cost (a
 * real compile under TCG) from the drive's own fixed overhead, which is what
 * drive_budget_ms below turns into a scaled budget. Returns the measured
 * host-wall ms on a verified-successful compile (object produced, TCC exited
 * 0, within CAL_TIMEOUT_MS), or -1 if calibration itself could not produce a
 * trustworthy measurement -- callers must treat -1 as "no signal", not "the
 * guest is fast", and fall back to CAL_FALLBACK_MS.
 */
static long calibrate_tcc_ms(const char *tcc, const char *tccinc, const char *comp)
{
    char workdir[] = "/tmp/mmkcal_XXXXXX";
    if (!mkdtemp(workdir)) return -1;

    char src[512], path[512];
    int ok;
    snprintf(src, sizeof(src), "%s/VMS_STRING.C", comp);
    snprintf(path, sizeof(path), "%s/VMS_STRING.C", workdir);
    ok = (copy_file(src, path) == 0);
    static const char *hdrs[] = { "vms_string.h", "vms_types.h", NULL };
    for (int i = 0; ok && hdrs[i]; i++) {
        snprintf(src, sizeof(src), "%s/%s", comp, hdrs[i]);
        snprintf(path, sizeof(path), "%s/%s", workdir, hdrs[i]);
        ok = (copy_file(src, path) == 0);
    }
    if (!ok) { rmdir(workdir); return -1; }

    char objpath[512];
    snprintf(objpath, sizeof(objpath), "%s/VMS_STRING.OBJ", workdir);

    long long t0 = now_ms();

    pid_t pid = fork();
    if (pid < 0) { rmdir(workdir); return -1; }
    if (pid == 0) {
        if (chdir(workdir) != 0) _exit(120);
        int devnull = open("/dev/null", O_RDWR);
        if (devnull >= 0) { dup2(devnull, STDIN_FILENO); dup2(devnull, STDOUT_FILENO); dup2(devnull, STDERR_FILENO); }
        /* Same flags as the descrip.mms TCC line in drive_build (-I <tccinc>
         * -I . VMS_STRING.C -o VMS_STRING.OBJ), so the calibration compile is
         * the SAME work, just reached directly instead of via MMK/DCL. */
        execl(tcc, tcc, "-x", "c", "-c", "-ffreestanding", "-fno-builtin",
              "-I", tccinc, "-I", ".", "VMS_STRING.C", "-o", "VMS_STRING.OBJ",
              (char *)NULL);
        _exit(127);   /* exec failed */
    }

    int status = 0, waited = 0, reaped = 0;
    while (waited < CAL_TIMEOUT_MS) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { reaped = 1; break; }
        if (r < 0) break;
        struct timespec ts = { .tv_sec = 0, .tv_nsec = 50 * 1000 * 1000 };
        nanosleep(&ts, NULL);
        waited += 50;
    }
    if (!reaped) {
        kill(pid, SIGKILL);
        (void)waitpid(pid, &status, 0);
    }
    long long t1 = now_ms();

    struct stat objst;
    int obj_ok = (stat(objpath, &objst) == 0 && objst.st_size > 0);

    unlink(objpath);
    snprintf(path, sizeof(path), "%s/VMS_STRING.C", workdir); unlink(path);
    snprintf(path, sizeof(path), "%s/vms_string.h", workdir); unlink(path);
    snprintf(path, sizeof(path), "%s/vms_types.h", workdir); unlink(path);
    rmdir(workdir);

    if (!reaped || !WIFEXITED(status) || WEXITSTATUS(status) != 0 || !obj_ok)
        return -1;   /* no trustworthy measurement -- caller falls back */

    long ms = (long)(t1 - t0);
    return ms >= 0 ? ms : -1;
}

/*
 * drive_budget_ms (vms-9d4f) -- turn a calibration measurement into a
 * machine-relative host-wall budget for one drive. See the DRIVE BUDGET
 * comment above DRIVE_COMPILE_MARGIN for the model and rationale. `t_cal_ms`
 * is calibrate_tcc_ms's return value (may be -1: no measurement).
 */
static long drive_budget_ms(long t_cal_ms, int do_link)
{
    long base = (t_cal_ms > 0) ? t_cal_ms : CAL_FALLBACK_MS;
    long margin = do_link ? DRIVE_COMPILE_MARGIN_LINK : DRIVE_COMPILE_MARGIN;
    long budget = margin * base + DRIVE_FIXED_OVERHEAD_MS;
    /* Never budget a real drive below the historical floor -- calibration
     * refines the budget upward for a slow guest, it never shrinks the bound
     * a fast guest has always run under. */
    if (budget < CAL_FALLBACK_MS) budget = CAL_FALLBACK_MS;
    return budget;
}

/*
 * Drive ONE MMK build in a fresh work directory. Both modes compile the real
 * runtime TU VMS_STRING.C with TCC.EXE and archive it into
 * OVMXRT.OLB with LIBRARIAN.EXE. When do_link is set, the drive ALSO compiles the
 * driver OVMXRTRUN.C and LINKs OVMXRTRUN.OBJ + OVMXRT.OLB into the runnable image
 * OVMXRT.EXE (LINK.EXE --use DECC$SHR.EXE), and the harness ACTIVATES it before
 * cleanup. On success (reaped set): objbuf/objlen hold VMS_STRING.OBJ (compile
 * oracle), olbbuf/olblen hold OVMXRT.OLB (archive oracle); with do_link,
 * exebuf/exelen hold OVMXRT.EXE (link oracle) and *activation_rc is its exit
 * status; saw_marker says whether the drive echoed the completion marker. Caller
 * frees objbuf/olbbuf/exebuf.
 *
 * WHY do_link IS ON ONLY ONE DRIVE (main runs drive #1 do_link=0, drive #2
 * do_link=1). The two-drive structure exists for the compile+archive byte-
 * identity comparison (vms-6be), which needs two INDEPENDENT compile+archive
 * builds. The LINK+activate rung (vms-725) is a functional proof -- a runnable
 * image that activates to 216 -- and does not need to run TWICE in-guest: LINK
 * OUTPUT determinism is proven byte-identical on the host
 * (tests/toolchain/run_mmk_component_build.sh links the same component image
 * twice, cmp-clean). Running the (heavier) LINK+activate on BOTH drives pushed
 * the suite past run_tests.sh's 120s whole-VM budget under slow/contended TCG;
 * running it on ONE keeps the suite close to the vms-6be weight the CI barrier
 * already carries reliably.
 */
static void drive_build(const char *mmk, const char *comp, const char *tcc,
                        const char *tccinc, const char *libr,
                        const char *lnk, const char *deccshr, int do_link,
                        long budget_ms,
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

    /* Mount the writable ODS-2 fixture on VDA0: executive-global so MMK reaches
     * its description through the ACP (idempotent). */
    if (!$VMS_STATUS_SUCCESS(vms_kif_acp_mount(ODS2_UNIT)))
        return;

    char path[512], src[512];

    /* Stage the REAL component source (VMS-style upper-case .C) + its headers.
     * VMS_STRING.C is a real src/libvmssys runtime TU, fully tcc-compilable on
     * x86_64. Only ONE runtime TU is compiled per drive (not two): this keeps the
     * suite LIGHTER than the vms-6be compile+archive suite so the whole-VM 120s
     * QEMU budget is not exceeded on CI's slower runner (a two-runtime-TU + LINK
     * drive tipped it -- the qemu timeout then reddened the whole run). LIBRARIAN
     * archiving MULTIPLE members is proven on the host (run_mmk_component_build.sh
     * archives three members byte-identically); in-guest this suite proves the
     * compile->archive->LINK->activate chain end to end + its determinism. With
     * do_link OVMXRTRUN.C is staged too -- the freestanding driver whose main()
     * calls vms_strlen, so the LINK pulls VMS_STRING from the .OLB and the
     * activated image exits 216. */
    static const char *srcs[] = { "VMS_STRING.C", NULL };
    for (int i = 0; srcs[i]; i++) {
        snprintf(src,  sizeof(src),  "%s/%s", comp, srcs[i]);
        snprintf(path, sizeof(path), "%s/%s", workdir, srcs[i]);
        if (copy_file(src, path) != 0) return;
    }
    if (do_link) {
        snprintf(src,  sizeof(src),  "%s/OVMXRTRUN.C", comp);
        snprintf(path, sizeof(path), "%s/OVMXRTRUN.C", workdir);
        if (copy_file(src, path) != 0) return;
    }
    /* vms_string.h + vms_types.h are the TU's + driver's only component
     * includes; stage the whole staged header set. */
    static const char *hdrs[] = { "vms_string.h", "vms_types.h", NULL };
    for (int i = 0; hdrs[i]; i++) {
        snprintf(src,  sizeof(src),  "%s/%s", comp, hdrs[i]);
        snprintf(path, sizeof(path), "%s/%s", workdir, hdrs[i]);
        if (copy_file(src, path) != 0) return;
    }

    /* The descrip.mms: define the TCC + LIBRARIAN (+ LNK) foreign commands
     * (absolute image paths, so they resolve in the spawned DCL without depending
     * on SYS$SYSTEM), compile the real runtime TU (+ the driver), archive the
     * runtime object into OVMXRT.OLB, (LINK the driver + .OLB into the runnable
     * OVMXRT.EXE,) then echo the completion marker. Each TAB-indented line is one
     * command record MMK streams into the spawned DCL -- the same whole-line-raw
     * delivery spine #6 proved for the single compile, extended to the full
     * compile->archive->link chain.
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
    /* The MMS RULE line is DEPENDENCY-FREE with a BARE target (all MMK's
     * lib$tparse accepts). The TAB-indented ACTION lines are unchanged and keep
     * BARE toolchain names: TCC/LIBRARIAN/LINK are fork()+execve()'d natives that
     * resolve those names against the Linux work directory (their cwd) via POSIX,
     * exactly as before -- only MMK's own reach into its description moved onto
     * the ODS-2 volume. MMK builds the (never-present, MFD-stat-missing) bare
     * target unconditionally, running the action chain. */
    /* The toolchain + shareable are named by VMS logical spec (SYS$SYSTEM:/
     * SYS$SHARE:), resolved over the ACP -- NOT the retired /vms passthrough
     * (vms-104). tccinc stays a POSIX include dir: it is a native-TCC -I
     * argument, not a DCL-resolved image. The POSIX tool paths passed in are used
     * only by the harness's own execs (calibration / execl(MMK)); the drive
     * resolves the tools the VMS way. */
    (void)tcc; (void)libr; (void)lnk; (void)deccshr;
    char mms[2560];
    if (do_link)
        snprintf(mms, sizeof(mms),
            "OVMXRT.EXE :\n"
            "\tTCC :== \"$%s\"\n"
            "\tLIBRARIAN :== \"$%s\"\n"
            "\tLNK :== \"$%s\"\n"
            "\tTCC -x c -c -ffreestanding -fno-builtin -I %s -I . VMS_STRING.C -o VMS_STRING.OBJ\n"
            "\tTCC -x c -c -ffreestanding -fno-builtin -I %s -I . OVMXRTRUN.C -o OVMXRTRUN.OBJ\n"
            "\tLIBRARIAN /CREATE OVMXRT.OLB VMS_STRING.OBJ\n"
            "\tLNK --executable --use %s -o OVMXRT.EXE OVMXRTRUN.OBJ OVMXRT.OLB\n"
            "\tWRITE SYS$OUTPUT \"%s\"\n",
            TCC_VMSSPEC, LIBR_VMSSPEC, LNK_VMSSPEC, tccinc, tccinc,
            DECCSHR_VMSSPEC, EXPECT_MARKER);
    else
        snprintf(mms, sizeof(mms),
            "OVMXRT.OLB :\n"
            "\tTCC :== \"$%s\"\n"
            "\tLIBRARIAN :== \"$%s\"\n"
            "\tTCC -x c -c -ffreestanding -fno-builtin -I %s -I . VMS_STRING.C -o VMS_STRING.OBJ\n"
            "\tLIBRARIAN /CREATE OVMXRT.OLB VMS_STRING.OBJ\n"
            "\tWRITE SYS$OUTPUT \"%s\"\n",
            TCC_VMSSPEC, LIBR_VMSSPEC, tccinc, EXPECT_MARKER);
    /* Author the description + empty rules file on the ODS-2 volume (MMK opens
     * them through RMS); start clean in case a prior drive in this VM left them. */
    erase_ods2(ODS2_DIR "725.MMS");
    erase_ods2(ODS2_DIR "MMS$RULES");
    if (create_ods2_blob(ODS2_DIR "725.MMS", mms) != 0) return;
    if (create_ods2_blob(ODS2_DIR "MMS$RULES",
                         "! empty default rules (vms-725 build drive)\n") != 0) return;

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
        /* /DESCRIPTION + /RULES_FILE are qualifier VALUES (full ODS-2 specs, not
         * rule-parsed); the P1 target is BARE (like the rule). The child keeps
         * cwd = the Linux workdir so the forked toolchain writes its artifacts
         * there, where the asserts below read them. */
        const char *fcmd = do_link
                 ? "/DESCRIPTION=" ODS2_DIR "725.MMS /RULES_FILE=" ODS2_DIR "MMS$RULES OVMXRT.EXE"
                 : "/DESCRIPTION=" ODS2_DIR "725.MMS /RULES_FILE=" ODS2_DIR "MMS$RULES OVMXRT.OLB";
        /* Record the foreign command tail on THIS process's executive CLI
         * context (vms-f60d): execl keeps the same PID/PCB, so the shipped MMK's
         * LIB$GET_FOREIGN reads it back with vms_kif_getcli -- the authoritative
         * channel the real runtime uses, not the retired VMS_FOREIGN_CMD env
         * shim (lib$get_foreign now prefers the executive whenever /dev/vms
         * answers and consults the env var only as the no-executive fallback).
         * MMK.EXE is a bare static image, so it re-REGISTERs onto this same PCB
         * (EEXIST, context preserved). The env var is kept only for that
         * no-executive fallback, mirroring lib$get_foreign. */
        (void)vms_kif_setcli(1, fcmd);
        setenv("VMS_FOREIGN_CMD", fcmd, 1);
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

    /* The FINAL artifact this drive is meant to produce: OVMXRT.EXE (the LINK
     * output) with do_link, else OVMXRT.OLB (the archive output). */
    const char *finalpath = do_link ? exepath : olbpath;

    /* ONE bounded wait that stops as soon as THE PROOF IS CAPTURED -- the DCL has
     * echoed the completion marker AND the final artifact exists on disk --
     * rather than waiting on MMK's own process EXIT. This is deliberate and is
     * what makes the suite robust under slow/contended TCG (CI, or a busy dev
     * host): MMK's teardown-and-exit AFTER the build can take much longer than the
     * build itself under load, but the veracity the suite asserts (a real image,
     * driven over the mailbox to the marker) is already on disk by then. Keying on
     * the FINAL artifact (produced LAST, by the LINK or archive step) -- not an
     * intermediate object -- guarantees the drive is not killed as cleanup until
     * the last stage has actually completed. Once both are present, MMK is killed
     * as CLEANUP -- its self-exit timing is NOT a pass condition (that made the
     * gate flaky: an earlier run built the artifact and echoed the marker but had
     * not finished exiting within the bound). A genuine mid-drive $HIBER deadlock
     * still fails HARD: no marker is ever echoed, so *saw_marker stays 0 and the
     * bound elapses. */
    char acc[16384];
    size_t acclen = 0;
    int waited = 0;
    int wstatus = 0;
    struct stat finalst;
    acc[0] = '\0';
    while (waited < budget_ms) {
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
        /* Proof captured: marker echoed AND the final artifact is on disk. */
        if (*saw_marker && stat(finalpath, &finalst) == 0 && finalst.st_size > 0)
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
     * compile oracle), OVMXRT.OLB (the archive oracle), and with do_link
     * OVMXRT.EXE (the link oracle). */
    *objlen = read_file(objpath, objbuf);
    *olblen = read_file(olbpath, olbbuf);
    if (do_link) {
        *exelen = read_file(exepath, exebuf);

        /* ACTIVATE the produced image BEFORE cleanup (the activation oracle),
         * THROUGH THE ACP (vms-104). The MMK-driven LINK wrote OVMXRT.EXE into the
         * Linux work dir, but IMGACT reads every image off the DISCOVERED ODS-2
         * system volume over the Files-11 ACP with NO /vms POSIX read (Rule 9 /
         * INV-6). So the harness: (1) WRITES the produced bytes onto SYSVOL_UNIT at
         * [SYS0.SYSCOMMON.SYSEXE]OVMXRT.EXE over the ACP; (2) STAGES a POSIX copy
         * at STAGED_IMAGE_PATH (the Linux-exec handoff the kernel maps + whose
         * PT_INTERP=/vms/.../IMGACT.EXE it opens); (3) execs the staged copy, whose
         * path imgsrc_map_staged() rewrites back to the on-volume SYS$SYSTEM
         * location so IMGACT reads the GENUINE bytes over the ACP and binds
         * DECC$SHR (also on SYSVOL_UNIT) the same way. OVMX_SYSDEVICE (set in
         * main) points IMGACT at SYSVOL_UNIT. 216 iff the driven LINK produced a
         * real image that really activates + runs off the volume. */
        if (*exelen > 0) {
            int wrote = (write_produced_image((const uint8_t *)*exebuf, *exelen) == 0);
            (void)mkdir("/run", 0755);
            (void)mkdir(STAGE_DIR, 0755);
            int staged = (copy_file(exepath, STAGED_IMAGE_PATH) == 0);
            if (staged)
                (void)chmod(STAGED_IMAGE_PATH, 0755);
            if (wrote && staged)
                *activation_rc = activate_image(STAGED_IMAGE_PATH);
            else
                printf("  (produced-image activation prep failed: on-volume write %s, POSIX stage %s)\n",
                       wrote ? "OK" : "FAILED", staged ? "OK" : "FAILED");
            (void)unlink(STAGED_IMAGE_PATH);
        }
    }

    /* On a failed drive, surface the driven-DCL transcript so a regression is
     * attributable (a cwd / path / link error is named here rather than showing
     * only "artifact never appeared"). Bounded: acc is capped above. */
    if ((do_link && *exelen <= 0) || *olblen <= 0 || *objlen <= 0) {
        printf("  (drive did not produce its final artifact; driven-DCL transcript follows)\n");
        printf("----8<----\n%s\n---->8----\n", acc);
    }

    /* Cleanup: the toolchain's Linux work files, plus the description/rules
     * authored on the ODS-2 volume (VDA0: is left mounted for sibling suites). */
    const char *rm[] = { "VMS_STRING.C", "OVMXRTRUN.C",
                         "VMS_STRING.OBJ", "OVMXRTRUN.OBJ",
                         "OVMXRT.OLB", "OVMXRT.EXE", "vms_string.h",
                         "vms_types.h",
                         NULL };
    for (int i = 0; rm[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", workdir, rm[i]);
        unlink(path);
    }
    rmdir(workdir);
    erase_ods2(ODS2_DIR "725.MMS");
    erase_ods2(ODS2_DIR "MMS$RULES");
}

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    setvbuf(stdout, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    printf("=== test_syssvc_mmk_build (MMK.EXE drives TCC compile + LIBRARIAN archive [byte-identical twice] + LINK to a runnable image IMGACT-activated to 216, in QEMU, vms-725) ===\n");

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

    /* Point IMGACT at the generated ODS-2 system volume (vms-29ff): the produced
     * image + its DECC$SHR are read over the ACP off SYSVOL_UNIT, NOT the
     * clean-room VDA0: fixture. Every fork+exec below inherits this env; only the
     * IMGACT-activated OVMXRT.EXE reads it (the static toolchain images do not). */
    setenv("OVMX_SYSDEVICE", SYSVOL_UNIT, 1);

    /* Mount SYSVOL_UNIT and resolve its [SYS0.SYSCOMMON.SYSEXE] DID once, so the
     * do_link drive can write the produced image there over the ACP and IMGACT
     * can activate it off the mounted volume. A precondition failure is a hard
     * FAIL (the activation oracle below cannot run without it). */
    CHECK(sysvol_prepare() == 0,
          "$MOUNT " SYSVOL_UNIT " + resolve [SYS0.SYSCOMMON.SYSEXE] over the ACP "
          "(the system volume the produced image is activated off, vms-104)");

    /* Stage the two producers the drive binds by VMS spec off the ODS-2 volume
     * THROUGH the ACP into /run/ovmx-boot (vms-104): the C run-time shareable
     * (LINK's --use SYS$SHARE:DECC$SHR.EXE) and the OVMX image activator (the
     * produced OVMXRT.EXE's PT_INTERP=/run/ovmx-boot/IMGACT.EXE, baked by the
     * vmslink build). Both bytes come off the volume over the ACP, never a /vms
     * read. The DCL foreign-command resolver stages the TCC/LIBRARIAN/LINK tools
     * itself the same way (dcl_resolve_activatable_acp). */
    (void)mkdir("/run", 0755);
    (void)mkdir(STAGE_DIR, 0755);
    static const char *const syslib_tree[] = { "SYS0", "SYSCOMMON", "SYSLIB", NULL };
    static const char *const sysexe_tree[] = { "SYS0", "SYSCOMMON", "SYSEXE", NULL };
    CHECK(stage_from_sysvol_over_acp(syslib_tree, "DECC$SHR.EXE") == 0,
          "staged SYS$SHARE:DECC$SHR.EXE off " SYSVOL_UNIT " over the ACP into "
          STAGE_DIR " (LINK's --use producer, read over the ACP -- no /vms)");
    CHECK(stage_from_sysvol_over_acp(sysexe_tree, "IMGACT.EXE") == 0,
          "staged IMGACT.EXE off " SYSVOL_UNIT " over the ACP into " STAGE_DIR
          " (the produced image's PT_INTERP, read over the ACP -- no /vms)");

    /* CALIBRATE (vms-9d4f): measure this run's own in-guest TCC compile
     * speed once, before either drive, and derive both drives' host-wall
     * budgets from it -- see the DRIVE BUDGET comment above
     * DRIVE_COMPILE_MARGIN. Printed always: it is the load-bearing number
     * for diagnosing a future timing report on this suite. */
    long t_cal = calibrate_tcc_ms(tcc, tccinc, comp);
    long budget1 = drive_budget_ms(t_cal, /*do_link=*/0);
    long budget2 = drive_budget_ms(t_cal, /*do_link=*/1);
    if (t_cal > 0)
        printf("  (calibration: reference TCC compile took %ldms in-guest -- drive budgets %ldms / %ldms)\n",
               t_cal, budget1, budget2);
    else
        printf("  (calibration: no measurement -- falling back to the historical %dms floor for both drives)\n",
               CAL_FALLBACK_MS);

    /* Build #1: an independent MMK drive in its own work dir. */
    char *obj1 = NULL, *obj2 = NULL, *olb1 = NULL, *olb2 = NULL, *exe1 = NULL, *exe2 = NULL;
    long  objlen1 = -1, objlen2 = -1, olblen1 = -1, olblen2 = -1, exelen1 = -1, exelen2 = -1;
    int   act1 = -1, act2 = -1, mark1 = 0, mark2 = 0, reap1 = 0, reap2 = 0;

    /* Drive #1: compile + archive only (do_link=0) -- the lighter vms-6be drive,
     * for the compile+archive byte-identity comparison. */
    drive_build(mmk, comp, tcc, tccinc, libr, lnk, deccshr, /*do_link=*/0, budget1,
                &obj1, &objlen1, &olb1, &olblen1, &exe1, &exelen1,
                &act1, &mark1, &reap1);

    int obj1_valid = (objlen1 > 0 && obj1 != NULL && is_elf_reloc(obj1, objlen1));
    int olb1_valid = (olblen1 > 8 && olb1 != NULL && memcmp(olb1, "!<arch>\n", 8) == 0);

    /* Drive #2: the FULL compile -> archive -> LINK chain (do_link=1), whose image
     * the harness then activates. Runs only if drive #1 produced a valid archive.
     * Keying the short-circuit on drive #1's OLB -- not on MMK's exit -- is robust
     * under load (MMK's slow self-exit does not skip a real second build) AND keeps
     * a failed drive to ONE marker bound: mmk-build-image-not-activated (the driven
     * toolchain never runs -> no object -> no .OLB) fails FAST on drive #1 and
     * skips #2, and a genuine mid-drive wedge (no marker, no artifact) likewise
     * costs one bound. */
    if (obj1_valid && olb1_valid)
        drive_build(mmk, comp, tcc, tccinc, libr, lnk, deccshr, /*do_link=*/1, budget2,
                    &obj2, &objlen2, &olb2, &olblen2, &exe2, &exelen2,
                    &act2, &mark2, &reap2);

    /* A valid OVMX image is an ELF ET_DYN (executables are position-independent
     * ET_DYN carrying PT_INTERP=IMGACT.EXE) -- e_type==3 at offset 16. */
    int exe2_valid = (exelen2 > 20 && exe2 != NULL && exe2[0] == 0x7f &&
                      exe2[1] == 'E' && exe2[2] == 'L' && exe2[3] == 'F' &&
                      ((uint16_t)((unsigned char)exe2[16] | ((unsigned char)exe2[17] << 8))) == 3);
    (void)mark2; (void)reap1; (void)reap2; (void)act1;
    (void)exe1; (void)exelen1;   /* drive #1 does not link; exe1 stays unused */

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
    CHECK(olblen1 > 0 && olb1 != NULL && contains(olb1, olblen1, "VMS_STRING"),
          "OVMXRT.OLB carries the archived member VMS_STRING -- LIBRARIAN inserted the driven object");
    CHECK(olblen1 > 0 && olb1 != NULL && contains(olb1, olblen1, "vms_strlen"),
          "OVMXRT.OLB embeds the runtime symbol vms_strlen -- its member is a compile of the REAL src/libvmssys/vms_string.c");
    CHECK(olblen2 > 0 && olb2 != NULL,
          "the MMK-driven LIBRARIAN.EXE produced OVMXRT.OLB in the guest (build #2)");

    /* byte-identical across two independent in-guest MMK-driven builds -- the
     * `ar`-container mtime/uid/gid reproducibility trap LIBRARIAN zeroes. */
    CHECK(olblen1 > 0 && olblen1 == olblen2 && olb1 && olb2 && memcmp(olb1, olb2, (size_t)olblen1) == 0,
          "OVMXRT.OLB is BYTE-IDENTICAL across two independent MMK-driven in-guest builds (deterministic archive, zero bash)");

    /* --- LINK + ACTIVATE stage (spine #7 FINAL rung, vms-725): the runnable image
     * the DRIVEN LINK made on drive #2, and the exit status it yields when IMGACT
     * activates it. These redden under the SAME mmk-build-image-not-activated root
     * as the compile/archive assertions (no driven toolchain -> no objects -> no
     * .OLB -> drive #2 skipped -> no OVMXRT.EXE -> nothing to activate), so they
     * are declared in that control's knock_on_fail. The LINK OUTPUT's byte-identity
     * is proven on the host (tests/toolchain/run_mmk_component_build.sh links the
     * image twice, cmp-clean); in-guest this rung proves the driven LINK produces a
     * real image that ACTIVATES and RUNS, which is what could not be shown before. --- */
    CHECK(exelen2 > 0 && exe2 != NULL,
          "the MMK-driven LINK.EXE produced OVMXRT.EXE in the guest");
    CHECK(exe2_valid,
          "OVMXRT.EXE is a valid OVMX image (ELF ET_DYN) -- LINK really linked it in QEMU");
    /* The vmslink build bakes PT_INTERP=/run/ovmx-boot/IMGACT.EXE -- the OVMX
     * image activator's boot-staged (ACP-read) POSIX home, NOT the retired /vms
     * passthrough (vms-104; src/vmslink/CMakeLists.txt IMGACT_INTERP_PATH). The
     * kernel opens that interp at activation; the harness staged IMGACT.EXE there
     * off the volume over the ACP above. */
    CHECK(exelen2 > 0 && exe2 != NULL && contains(exe2, exelen2, "/run/ovmx-boot/IMGACT.EXE"),
          "OVMXRT.EXE carries PT_INTERP=IMGACT.EXE (/run/ovmx-boot, ACP-staged) -- it is an image the kernel activates through IMGACT, not a bare ELF");
    CHECK(act2 == EXPECT_EXIT,
          "IMGACT activated the MMK-driven OVMXRT.EXE and it RAN to exit 216 (vms_strlen(\"OVMXRT\")*36) -- the LINK pulled VMS_STRING from the .OLB and the image really runs");

    free(obj1); free(obj2); free(olb1); free(olb2); free(exe1); free(exe2);

    printf("=== test_syssvc_mmk_build: %d passed, %d failed ===\n", pass, fail);
    return fail > 0 ? 1 : 0;
}
