/*
 * test_lib_rtl_batch3.c - Unit tests for the LIB$ RTL routines added under
 * vms-801 R2.2 batch 3:
 *
 *   lib$init_timer lib$stat_timer lib$show_timer lib$free_timer  (timing)
 *   lib$sys_asctim (delta-time formatting)
 *   lib$show_vm lib$stat_vm lib$find_vm_zone lib$show_vm_zone
 *   lib$verify_vm_zone                                            (VM zones)
 *   lib$convert_date_string lib$get_maximum_date_length
 *   lib$format_date_time lib$get_date_format                      (date/time)
 *   lib$get_users_language lib$sys_getmsg                         (locale/msg)
 *   lib$lookup_key                                                (keyword)
 *   lib$get_command lib$get_foreign                               (input)
 *
 * Assertions are grounded in the documented behaviour (OpenVMS RTL LIB$
 * Manual).
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include "lib$routines.h"
#include "libdef.h"
#include "libdtdef.h"
#include "ssdef.h"
#include "stsdef.h"
#include "rmsdef.h"
#include "descrip.h"
#include "gen64def.h"

/* --- Prototypes for routines not (yet) in the public headers --------- */
extern uint32_t lib$init_timer(uint32_t *handle);
extern uint32_t lib$stat_timer(const int32_t *code, void *value,
                               uint32_t *handle);
extern uint32_t lib$show_timer(uint32_t *handle, ...);
extern uint32_t lib$free_timer(uint32_t *handle);
extern uint32_t lib$sys_asctim(uint16_t *timlen, struct dsc$descriptor_s *tb,
                               const struct _generic_64 *timadr,
                               uint32_t cvtflg);
extern uint32_t lib$show_vm(void);
extern uint32_t lib$stat_vm(const int32_t *code, uint32_t *value);
extern uint32_t lib$find_vm_zone(uint32_t *context, uint32_t *zone_id);
extern uint32_t lib$show_vm_zone(const uint32_t *zone_id, const int32_t *detail,
                                 uint32_t (*action)(struct dsc$descriptor_s *,
                                                    void *),
                                 void *user_arg);
extern uint32_t lib$verify_vm_zone(const uint32_t *zone_id);
extern uint32_t lib$create_vm_zone(uint32_t *zone_id, ...);
extern uint32_t lib$get_vm(const uint32_t *num_bytes, void **base_adr, ...);
extern uint32_t lib$convert_date_string(const struct dsc$descriptor_s *input,
                                        uint64_t *out_time, ...);
extern uint32_t lib$get_maximum_date_length(int32_t *length, void *context,
                                            const uint32_t *flags);
extern uint32_t lib$format_date_time(struct dsc$descriptor_s *out,
                                     const void *in_time, void *context,
                                     uint16_t *out_len, const uint32_t *flags);
extern uint32_t lib$get_date_format(struct dsc$descriptor_s *out, void *ctx);
extern uint32_t lib$get_users_language(struct dsc$descriptor_s *language);
extern uint32_t lib$sys_getmsg(const uint32_t *msgid, uint16_t *msglen,
                               struct dsc$descriptor_s *bufadr,
                               const uint32_t *flags);
extern uint32_t lib$lookup_key(const struct dsc$descriptor_s *input,
                               const uint32_t *table, uint32_t *key_value,
                               struct dsc$descriptor_s *keyword,
                               uint16_t *keyword_len);
extern uint32_t lib$get_command(struct dsc$descriptor_s *result,
                                const struct dsc$descriptor_s *prompt,
                                uint16_t *result_len);
extern uint32_t lib$get_foreign(struct dsc$descriptor_s *result,
                                const struct dsc$descriptor_s *prompt,
                                uint16_t *result_len, ...);

static int failures = 0;

static void check(int cond, const char *name)
{
    if (cond) {
        printf("  OK: %s\n", name);
    } else {
        printf("  FAIL: %s\n", name);
        failures++;
    }
}

/* ------------------------------------------------------------------ */
static void test_timer(void)
{
    printf("Testing lib$init_timer / stat_timer / show_timer / free_timer...\n");

    uint32_t h = 0;
    check(lib$init_timer(&h) == SS$_NORMAL, "lib$init_timer returns SS$_NORMAL");
    check(h != 0, "lib$init_timer allocates a handle (non-zero)");

    /* Burn a little time so the elapsed delta is measurable. */
    struct timespec req = { 0, 20 * 1000 * 1000 };  /* 20 ms */
    nanosleep(&req, NULL);

    GENERIC_64 elapsed;
    int32_t code = 1;
    check(lib$stat_timer(&code, &elapsed.gen64$l_longword[0], &h) == SS$_NORMAL,
          "lib$stat_timer code 1 returns SS$_NORMAL");
    check((int64_t)elapsed.gen64$q_quadword < 0,
          "elapsed real time is a negative (delta) quadword");

    uint32_t cpu = 0xffffffffu;
    code = 2;
    check(lib$stat_timer(&code, &cpu, &h) == SS$_NORMAL,
          "lib$stat_timer code 2 (CPU) returns SS$_NORMAL");
    check(cpu != 0xffffffffu, "CPU statistic was written");

    uint32_t flt = 0xffffffffu;
    code = 5;
    check(lib$stat_timer(&code, &flt, &h) == SS$_NORMAL,
          "lib$stat_timer code 5 (faults) returns SS$_NORMAL");

    code = 99;
    check(lib$stat_timer(&code, &cpu, &h) == LIB$_INVARG,
          "lib$stat_timer rejects an unknown code");

    check(lib$show_timer(&h) == SS$_NORMAL, "lib$show_timer returns SS$_NORMAL");
    check(lib$free_timer(&h) == SS$_NORMAL, "lib$free_timer returns SS$_NORMAL");
    check(h == 0, "lib$free_timer clears the handle");
}

/* ------------------------------------------------------------------ */
static void test_asctim_delta(void)
{
    printf("Testing lib$sys_asctim delta formatting...\n");

    /* 1 day, 2 hours, 3 minutes, 4.05 seconds -> negative delta quadword. */
    uint64_t ticks = ((uint64_t)1 * 86400 + 2 * 3600 + 3 * 60 + 4) * 10000000ULL
                   + 5 * 100000ULL;
    GENERIC_64 delta;
    delta.gen64$q_quadword = (uint64_t)(-(int64_t)ticks);

    char buf[32];
    struct dsc$descriptor_s d = { sizeof(buf) - 1, DSC$K_DTYPE_T,
                                  DSC$K_CLASS_S, buf };
    uint16_t len = 0;
    uint32_t st = lib$sys_asctim(&len, &d, &delta, 0);
    buf[len] = '\0';
    check(st == SS$_NORMAL, "lib$sys_asctim(delta) returns SS$_NORMAL");
    check(strcmp(buf, "1 02:03:04.05") == 0,
          "delta formats as \"1 02:03:04.05\"");
}

/* ------------------------------------------------------------------ */
static char captured_zone[128];

static uint32_t capture_line(struct dsc$descriptor_s *s, void *arg)
{
    (void)arg;
    uint16_t n = s->dsc$w_length;
    if (n > sizeof(captured_zone) - 1) n = sizeof(captured_zone) - 1;
    memcpy(captured_zone, s->dsc$a_pointer, n);
    captured_zone[n] = '\0';
    return SS$_NORMAL;
}

static void test_vm_zone(void)
{
    printf("Testing lib$create/find/show/verify/stat_vm zone routines...\n");

    check(lib$show_vm() == SS$_NORMAL, "lib$show_vm returns SS$_NORMAL");

    uint32_t zid = 0;
    int algo = 1, arg = 32;
    unsigned flags = 0;
    struct dsc$descriptor_s name_d = { 11, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                       (char *)"Danger Zone" };
    uint32_t st = lib$create_vm_zone(&zid, &algo, &arg, &flags,
                                     0, 0, 0, 0, 0, 0, &name_d, 0, 0);
    check(st == SS$_NORMAL && zid != 0, "lib$create_vm_zone with a name");

    void *p = NULL;
    uint32_t sz = 64;
    check(lib$get_vm(&sz, &p, &zid) == SS$_NORMAL && p != NULL,
          "lib$get_vm from the named zone");

    /* find_vm_zone must eventually return our zone. */
    uint32_t ctx = 0, found = 0, this_zone = 0;
    while (lib$find_vm_zone(&ctx, &this_zone) == SS$_NORMAL) {
        if (this_zone == zid) { found = 1; break; }
    }
    check(found, "lib$find_vm_zone enumerates the created zone");

    int detail = 1;
    captured_zone[0] = '\0';
    check(lib$show_vm_zone(&zid, &detail, capture_line, NULL) == SS$_NORMAL,
          "lib$show_vm_zone invokes the action routine");
    check(strstr(captured_zone, "Zone Name = \"Danger Zone\"") != NULL,
          "show_vm_zone line carries the zone name");

    check(lib$verify_vm_zone(&zid) == SS$_NORMAL,
          "lib$verify_vm_zone returns SS$_NORMAL");

    uint32_t bad = 999;
    check(lib$verify_vm_zone(&bad) == LIB$_BADZONE,
          "lib$verify_vm_zone rejects a bad zone id");

    /* stat_vm reports the DEFAULT zone (zone 0); the call must succeed. */
    int32_t code = 1;
    uint32_t calls = 0;
    check(lib$stat_vm(&code, &calls) == SS$_NORMAL,
          "lib$stat_vm code 1 returns SS$_NORMAL");
}

/* ------------------------------------------------------------------ */
static void test_date_routines(void)
{
    printf("Testing lib$convert_date_string / format_date_time / ...\n");

    uint64_t now_t = (uint64_t)time(NULL) * 10000000ULL + 0x007C95674BEB4000ULL;

    struct dsc$descriptor_s tomorrow = { 8, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                         (char *)"TOMORROW" };
    uint64_t tv = 0;
    check(lib$convert_date_string(&tomorrow, &tv, 0, 0, 0, 0) == SS$_NORMAL,
          "lib$convert_date_string(\"TOMORROW\") returns SS$_NORMAL");
    check(tv > now_t, "TOMORROW is later than now");

    struct dsc$descriptor_s abs_d = { 20, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                      (char *)"17-NOV-2003 06:00:00" };
    uint64_t av = 0;
    check(lib$convert_date_string(&abs_d, &av, 0, 0, 0, 0) == SS$_NORMAL,
          "lib$convert_date_string parses an absolute date");
    check(av > 0x007C95674BEB4000ULL, "absolute time is after the VMS epoch");

    int32_t maxlen = 0;
    uint32_t f = LIB$M_DATE_FIELDS | LIB$M_TIME_FIELDS;
    check(lib$get_maximum_date_length(&maxlen, 0, &f) == SS$_NORMAL,
          "lib$get_maximum_date_length returns SS$_NORMAL");
    check(maxlen >= 23, "maximum date length bounds the standard format");

    char fbuf[64];
    struct dsc$descriptor_s fd = { (uint16_t)maxlen, DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, fbuf };
    uint16_t flen = 0;
    check(lib$format_date_time(&fd, 0, 0, &flen, &f) == SS$_NORMAL,
          "lib$format_date_time returns SS$_NORMAL");
    fbuf[flen] = '\0';
    check(strchr(fbuf, '-') && strchr(fbuf, ':'),
          "formatted date+time has date and time separators");

    struct dsc$descriptor_s df = { 0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL };
    uint32_t gst = lib$get_date_format(&df, 0);
    check(gst == LIB$_DEFFORUSE || gst == SS$_NORMAL,
          "lib$get_date_format returns DEFFORUSE (no logical) or SS$_NORMAL");
}

/* ------------------------------------------------------------------ */
static void test_locale_and_msg(void)
{
    printf("Testing lib$get_users_language / lib$sys_getmsg...\n");

    char lbuf[64];
    struct dsc$descriptor_s lang = { sizeof(lbuf) - 1, DSC$K_DTYPE_T,
                                     DSC$K_CLASS_S, lbuf };
    uint32_t st = lib$get_users_language(&lang);
    check(st == LIB$_ENGLUSED || st == SS$_NORMAL,
          "lib$get_users_language returns ENGLUSED (no logical) or SS$_NORMAL");

    char mbuf[256];
    struct dsc$descriptor_s msg = { sizeof(mbuf) - 1, DSC$K_DTYPE_T,
                                    DSC$K_CLASS_S, mbuf };
    uint16_t mlen = 0;
    uint32_t code = SS$_NORMAL;
    uint32_t flags = 0x0f;
    check(lib$sys_getmsg(&code, &mlen, &msg, &flags) == SS$_NORMAL,
          "lib$sys_getmsg returns SS$_NORMAL");
    check(mlen > 0, "lib$sys_getmsg produced text");
}

/* ------------------------------------------------------------------ */
/* Counted-string (ASCIC) helper, matching the corpus lib_lookup_key.c. */
#define ASCIC(nm, str) \
    static struct { unsigned char count; char s[sizeof(str)]; } \
    nm = { sizeof(str) - 1, str }

static void test_lookup_key(void)
{
    printf("Testing lib$lookup_key...\n");

    ASCIC(create, "CREATE");
    ASCIC(delete, "DELETE");
    ASCIC(show,   "SHOW");
    ASCIC(set,    "SET");
    ASCIC(quit,   "EXIT");

    uint32_t table[] = { 10,
                         (uint32_t)(uintptr_t)&create, 1,
                         (uint32_t)(uintptr_t)&delete, 2,
                         (uint32_t)(uintptr_t)&show,   3,
                         (uint32_t)(uintptr_t)&set,    4,
                         (uint32_t)(uintptr_t)&quit,   5 };
    /* On LP64 the ASCIC addresses may not fit in a uint32_t; skip the
     * pointer-dependent assertions if truncation would occur. */
    int addressable =
        ((uintptr_t)&create >> 32) == 0 && ((uintptr_t)&quit >> 32) == 0;

    if (!addressable) {
        printf("  SKIP: ASCIC addresses exceed 32 bits on this host\n");
        return;
    }

    char kbuf[64];
    struct dsc$descriptor_s kw = { sizeof(kbuf) - 1, DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, kbuf };
    uint16_t klen = 0;
    uint32_t val = 0;

    struct dsc$descriptor_s in_cr = { 2, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                      (char *)"CR" };
    kw.dsc$w_length = sizeof(kbuf) - 1;
    check(lib$lookup_key(&in_cr, table, &val, &kw, &klen) == SS$_NORMAL &&
          val == 1, "abbreviation \"CR\" resolves to CREATE (value 1)");

    struct dsc$descriptor_s in_exit = { 4, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                        (char *)"EXIT" };
    kw.dsc$w_length = sizeof(kbuf) - 1;
    check(lib$lookup_key(&in_exit, table, &val, &kw, &klen) == SS$_NORMAL &&
          val == 5, "exact \"EXIT\" resolves to value 5");

    struct dsc$descriptor_s in_s = { 1, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                     (char *)"S" };
    kw.dsc$w_length = sizeof(kbuf) - 1;
    check(lib$lookup_key(&in_s, table, &val, &kw, &klen) == LIB$_AMBKEY,
          "ambiguous \"S\" (SHOW/SET) returns LIB$_AMBKEY");

    struct dsc$descriptor_s in_x = { 3, DSC$K_DTYPE_T, DSC$K_CLASS_S,
                                     (char *)"XYZ" };
    kw.dsc$w_length = sizeof(kbuf) - 1;
    check(lib$lookup_key(&in_x, table, &val, &kw, &klen) == LIB$_UNRKEY,
          "unknown \"XYZ\" returns LIB$_UNRKEY");
}

/* ------------------------------------------------------------------ */
static void test_input_eof(void)
{
    printf("Testing lib$get_command / lib$get_foreign EOF behavior...\n");

    /* Redirect stdin to /dev/null so the read hits end of file. */
    FILE *saved = stdin;
    (void)saved;
    if (!freopen("/dev/null", "r", stdin)) {
        printf("  SKIP: could not reopen stdin\n");
        return;
    }

    char ibuf[64];
    struct dsc$descriptor_s in = { sizeof(ibuf) - 1, DSC$K_DTYPE_T,
                                   DSC$K_CLASS_S, ibuf };
    uint16_t ilen = 0;
    check(lib$get_command(&in, NULL, &ilen) == RMS$_EOF,
          "lib$get_command returns RMS$_EOF at end of file");
    check(lib$get_foreign(&in, NULL, &ilen) == RMS$_EOF,
          "lib$get_foreign returns RMS$_EOF at end of file");
}

/* ------------------------------------------------------------------ */
int main(void)
{
    printf("=== LIB$ RTL batch 3 unit tests (vms-801 R2.2) ===\n\n");

    test_timer();
    test_asctim_delta();
    test_vm_zone();
    test_date_routines();
    test_locale_and_msg();
    test_lookup_key();
    test_input_eof();

    printf("\n=== %s (%d failure%s) ===\n",
           failures == 0 ? "PASS" : "FAIL",
           failures, failures == 1 ? "" : "s");
    return failures == 0 ? 0 : 1;
}
