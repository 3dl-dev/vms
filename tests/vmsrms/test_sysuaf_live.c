/*
 * test_sysuaf_live.c - vms-d92 (epic vms-d0c): the SYSUAF ATOMIC FLIP proof.
 *
 * Authors a genuine binary $UAFDEF SYSUAF (RMS Prolog-3 indexed file, primary
 * USERNAME key + secondary UIC key) with the SAME code path the seed and the
 * runtime use -- sysuaf_view_to_raw + sysuaf_set_password (the real Purdy
 * UAI$C_PURDY_S hash) + sysuaf_put_record -- then reads it BACK through the
 * indexed engine and proves LOGIN authenticity:
 *
 *   1. SYSTEM authenticates with MANAGER   (reading the real binary record +
 *      Purdy-verifying -- NO ASCII, NO SHA-256),
 *   2. SYSTEM is REFUSED with a wrong password,
 *   3. GUEST authenticates with GUEST,
 *   4. a passwordless account (OPERATOR: no PURDY_S credential) refuses EVERY
 *      password (the binary equivalent of the retired empty-hash rule, vms-08f),
 *   5. the record read back carries the real $UAFDEF fields (UIC via the
 *      secondary key, default directory, privileges),
 *   6. get-by-UIC (secondary key -> SIDR -> primary) resolves SYSTEM from [1,4].
 *
 * Host-only: the engine reads/writes blocks via rms_io_* wrapping a plain fd
 * (host ctest has no /dev/vms); the /dev/vms ACP end-to-end (booting to
 * Username: and authenticating SYSTEM/MANAGER over the real ACP) is the qemu
 * e2e reap gate (vms-3d6). No ASCII, no SHA-256, no /vms passthrough, no stub.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <fcntl.h>

#include "sysuaf.h"        /* view API: set_password / authenticate / mapping  */
#include "sysuaf_rms.h"    /* the binary $UAFDEF indexed engine                 */
#include "rms_io.h"        /* rms_io_posix_wrap / _unwrap                       */
#include "rmsdef.h"
#include "ssdef.h"

static int g_fail = 0;
static void check(int cond, const char *msg)
{
    printf("%s: %s\n", cond ? "PASS" : "FAIL", msg);
    if (!cond)
        g_fail = 1;
}

/* Build one $UAFDEF record from view fields (+ optional password). */
static void build(sysuaf_rms_record_t *out, const char *user,
                  uint16_t grp, uint16_t mem, const char *dir,
                  const char *priv, const char *pw)
{
    sysuaf_record_t v;
    memset(&v, 0, sizeof(v));
    strncpy(v.username, user, sizeof(v.username) - 1);
    v.uic_group  = grp;
    v.uic_member = mem;
    strncpy(v.default_dir, dir, sizeof(v.default_dir) - 1);
    strncpy(v.privileges, priv, sizeof(v.privileges) - 1);
    sysuaf_view_to_raw(&v);
    if (pw)
        sysuaf_set_password(&v, pw);   /* real Purdy hash into v.raw            */
    *out = v.raw;
}

int main(void)
{
    char path[] = "/tmp/ovmx_sysuaf_live_XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0) { printf("mkstemp failed\n"); return 1; }

    rms_file_t *f = rms_io_posix_wrap(fd);
    if (!f) { printf("posix_wrap failed\n"); unlink(path); return 1; }

    /* ---- author the binary SYSUAF ---- */
    sysuaf_rms_file_t sf;
    uint32_t st = sysuaf_rms_create(f, &sf);
    check(st == RMS$_CREATED && sf.ctx, "sysuaf_rms_create (USERNAME + UIC keys)");
    if (!sf.ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }

    sysuaf_rms_record_t rec;
    build(&rec, "SYSTEM",   1,   4,   "SYS$SYSDEVICE:[SYSMGR]",        "TMPMBX,NETMBX", "MANAGER");
    check(sysuaf_put_record(&sf, &rec) == RMS$_NORMAL, "put SYSTEM (Purdy MANAGER)");
    build(&rec, "OPERATOR", 1,   6,   "SYS$SYSDEVICE:[SYSMGR]",        "OPER,TMPMBX",   NULL);
    check(sysuaf_put_record(&sf, &rec) == RMS$_NORMAL, "put OPERATOR (no password)");
    build(&rec, "GUEST",    128, 129, "SYS$SYSDEVICE:[USERS.GUEST]",   "TMPMBX",        "GUEST");
    check(sysuaf_put_record(&sf, &rec) == RMS$_NORMAL, "put GUEST (Purdy GUEST)");
    sysuaf_rms_close(&sf);

    /* ---- reopen the on-disk prologue and authenticate ---- */
    st = sysuaf_rms_open(f, &sf);
    check($VMS_STATUS_SUCCESS(st) && sf.ctx, "sysuaf_rms_open re-binds the prologue");
    if (!sf.ctx) { rms_io_posix_unwrap(f); unlink(path); return 1; }

    sysuaf_rms_record_t raw;
    sysuaf_record_t view;

    /* SYSTEM by USERNAME primary key -> Purdy verify. */
    check(sysuaf_get_by_username(&sf, "SYSTEM", &raw) == RMS$_NORMAL,
          "get_by_username SYSTEM (binary primary key)");
    sysuaf_raw_to_view(&raw, &view);
    check(sysuaf_authenticate(&view, "MANAGER") == 1,
          "SYSTEM authenticates with MANAGER (binary + Purdy, no SHA-256)");
    check(sysuaf_authenticate(&view, "WRONGPW") == 0,
          "SYSTEM refused with a wrong password");
    check(sysuaf_authenticate(&view, "") == 0,
          "SYSTEM refused with an empty password");
    check(view.uic_group == 1 && view.uic_member == 4,
          "SYSTEM record carries UIC [1,4] from the binary field");
    check(strcmp(view.default_dir, "SYS$SYSDEVICE:[SYSMGR]") == 0,
          "SYSTEM default directory round-trips from the binary record");
    check(view.privileges[0] != '\0',
          "SYSTEM privileges render from the binary uaf$q_priv mask");

    /* GUEST. */
    check(sysuaf_get_by_username(&sf, "GUEST", &raw) == RMS$_NORMAL,
          "get_by_username GUEST");
    sysuaf_raw_to_view(&raw, &view);
    check(sysuaf_authenticate(&view, "GUEST") == 1,
          "GUEST authenticates with GUEST");
    check(sysuaf_authenticate(&view, "system") == 0,
          "GUEST refused with the wrong password");

    /* OPERATOR: no PURDY_S credential -> refuses every password. */
    check(sysuaf_get_by_username(&sf, "OPERATOR", &raw) == RMS$_NORMAL,
          "get_by_username OPERATOR");
    sysuaf_raw_to_view(&raw, &view);
    check(sysuaf_authenticate(&view, "OPERATOR") == 0,
          "passwordless OPERATOR refuses a guessed password");
    check(sysuaf_authenticate(&view, "") == 0,
          "passwordless OPERATOR refuses the empty password");

    /* case-insensitive username lookup. */
    check(sysuaf_get_by_username(&sf, "system", &raw) == RMS$_NORMAL,
          "username lookup folds case (system -> SYSTEM)");

    /* secondary UIC key: [1,4] -> SYSTEM. */
    check(sysuaf_get_by_uic(&sf, ((uint32_t)1 << 16) | 4u, &raw) == RMS$_NORMAL,
          "get_by_uic [1,4] resolves via the secondary key/SIDR");
    sysuaf_raw_to_view(&raw, &view);
    check(strcmp(view.username, "SYSTEM") == 0,
          "get_by_uic [1,4] returns SYSTEM");

    /* a username with no account. */
    check(sysuaf_get_by_username(&sf, "NOBODY", &raw) == RMS$_RNF,
          "get_by_username on an absent account returns RMS$_RNF");

    sysuaf_rms_close(&sf);
    rms_io_posix_unwrap(f);
    unlink(path);

    printf("%s\n", g_fail ? "SYSUAF LIVE FLIP: FAIL" : "SYSUAF LIVE FLIP: ALL PASS");
    return g_fail ? 1 : 0;
}
