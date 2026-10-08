/*
 * test_dnet_fal_server.c - the OVMX FAL SERVER (dnet_fal_server_run) driven by
 * the EXACT DAP segments a real OpenVMS VAX V7.3 COPY sent it on the lab
 * (rd vms-d85 inbound, tests/lab/captures/decnet-fal-inbound-20261004/). A
 * scripted transport replays the VMS client's segments and records the
 * server's replies; file I/O is captured in memory (the shipped I/O is RMS over
 * the ACP, proven on the booted executive -- this pins the PROTOCOL a real VMS
 * client needs, so a regression in it reds on a host).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "dnet_fal.h"
#include "ssdef.h"

static int g_pass, g_fail;
#define CHECK(c, m) do { if (c) g_pass++; else { g_fail++; printf("  FAIL: %s\n", m); } } while (0)

/* ---- in-memory file system standing in for RMS ---- */
static char g_put_spec[256], g_put_data[4096]; static size_t g_put_len;
static int g_put_closed;
static int g_wopen_no_rsa;   /* 1 = RMS $CREATE left the NAM resultant empty */
static const char *g_src_lines[] = { "Served by the OVMX FAL server code", "second record" };

static const char *g_found;
int dnet_fal_search_begin(const char *spec, void **ctx)
{ static int c;
  if (strstr(spec, "GREET.TXT")) g_found = "DKA0:[SRV]GREET.TXT;1";
  else if (strstr(spec, "PUTNAME.TXT") && g_put_spec[0]) g_found = "DKA0:[SRV]PUTNAME.TXT;1";
  else return -1;
  c = 0; *ctx = &c; return 0; }
int dnet_fal_search_next(void *ctx, char *rsa, size_t cap)
{ int *c = ctx; if ((*c)++) return -1; snprintf(rsa, cap, "%s", g_found); return 0; }
void dnet_fal_search_end(void *ctx) { (void)ctx; }
int dnet_fal_wopen(const char *spec, uint8_t rfm, uint8_t rat, void **h, char *rsa, size_t cap)
{ (void)rfm; (void)rat; snprintf(g_put_spec, sizeof g_put_spec, "%s", spec); g_put_len = 0; g_put_closed = 0;
  if (rsa && cap) { if (g_wopen_no_rsa) rsa[0] = '\0'; else snprintf(rsa, cap, "DKA0:[SRV]%s1", spec); }
  *h = g_put_spec; return 0; }
int dnet_fal_wput(void *h, const uint8_t *rec, size_t len)
{ (void)h; if (g_put_len + len + 1 > sizeof g_put_data) return -1;
  memcpy(g_put_data + g_put_len, rec, len); g_put_len += len; g_put_data[g_put_len++] = '\n'; return 0; }
int dnet_fal_wclose(void *h) { (void)h; g_put_closed = 1; return 0; }
static int g_rpos, g_ropen_ok;
int dnet_fal_ropen(const char *spec, void **h, uint8_t *rfm, uint8_t *rat)
{ if (!strstr(spec, "GREET.TXT")) return -1; g_rpos = 0; g_ropen_ok = 1;
  if (rfm) *rfm = 2;
  if (rat) *rat = 2;
  *h = &g_rpos; return 0; }
/* The second record carries an embedded NUL: records are length-delimited,
 * never C strings (a VAR file read back must be record-for-record). */
int dnet_fal_rget(void *h, uint8_t *rec, size_t cap, size_t *len)
{ int *p = h; if (*p >= 2) return 0;
  const char *l = g_src_lines[*p]; size_t n = strlen(l);
  if (*p == 1) { if (n + 2 > cap) return -1; memcpy(rec, l, n); rec[n] = 0; rec[n + 1] = 'Z'; *len = n + 2; }
  else { if (n > cap) return -1; memcpy(rec, l, n); *len = n; }
  (*p)++; return 1; }
int dnet_fal_rclose(void *h) { (void)h; return 0; }

/* ---- scripted transport ---- */
struct script { const char **in; int nin, pos; char out[64][1600]; int nout; };
static size_t unhex(const char *h, uint8_t *o, size_t cap)
{ size_t n = 0; while (h[0] && h[1] && n < cap) { unsigned v; sscanf(h, "%2x", &v); o[n++] = (uint8_t)v; h += 2; } return n; }
static int s_send(void *c, const uint8_t *seg, size_t len)
{ struct script *s = c; if (s->nout >= 64) return -1; char *o = s->out[s->nout++];
  for (size_t i = 0; i < len && 2 * i + 2 < sizeof s->out[0]; i++) sprintf(o + 2 * i, "%02x", seg[i]);
  return 0; }
static int s_recv(void *c, uint8_t *buf, size_t cap, size_t *len)
{ struct script *s = c; if (s->pos >= s->nin) return -1; *len = unhex(s->in[s->pos++], buf, cap); return 0; }

static int saw(struct script *s, const char *prefix)
{ for (int i = 0; i < s->nout; i++) if (!strncmp(s->out[i], prefix, strlen(prefix))) return i; return -1; }

int main(void)
{
    static struct dnet_dap_transport t;
    t.send = s_send; t.recv = s_recv;

    /* 1. VMS COPY local -> OVMX (PUT), with DISPLAY main+NAME. */
    static const char *put_in[] = {
        "01003c1007030702000500f7fbd9ffaeac8694e77f",
        "020210efa00401000202000001018080100e00030002010c5055544e414d452e5458543b53408102",
        "0400020840",
        "0402040409034008060f000048656c6c6f206c696e65206f6e6508060900006c696e652074776f070004",
        "07000100",
    };
    static struct script ps; memset(&ps, 0, sizeof ps); ps.in = put_in; ps.nin = 5;
    t.ctx = &ps; t.rxlen = t.rxoff = 0;
    uint32_t st = dnet_fal_server_run(&t);
    int i_att = saw(&ps, "0200"), i_name = saw(&ps, "0f0001"), i_ack = saw(&ps, "0600");
    CHECK(st == 1, "VMS PUT session completes (SS$_NORMAL)");
    CHECK(i_att >= 0 && i_name > i_att && i_ack > i_name,
          "CREATE reply = ATTRIBUTES, NAME(resultant), ACK -- the order a VMS COPY requires");
    CHECK(strcmp(g_put_spec, "PUTNAME.TXT;") == 0, "the file created is the spec VMS named");
    CHECK(g_put_closed && g_put_len == 24 && !memcmp(g_put_data, "Hello line one\nline two\n", 24),
          "both blocked DATA records stored verbatim and the file closed");
    CHECK(saw(&ps, "070002") >= 0, "END-OF-STREAM and CLOSE answered ACCESS COMPLETE(RESPONSE)");

    /* 1b. If $CREATE ever returned no resultant (rd vms-98e), the server
     * must REFUSE the access honestly -- an ACK without the NAME the VMS COPY
     * asked for is a DAP sync error at the peer (RMS-F-BUG_DAP 0001A006). */
    g_wopen_no_rsa = 1; g_put_spec[0] = '\0';
    static struct script ps2; memset(&ps2, 0, sizeof ps2); ps2.in = put_in; ps2.nin = 5;
    t.ctx = &ps2; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    CHECK(saw(&ps2, "0600") < 0 && saw(&ps2, "0f0001") < 0 && saw(&ps2, "0900") >= 0,
          "no $CREATE resultant: STATUS refusal, never ACK without the NAME asked for");
    g_wopen_no_rsa = 0;

    /* 2. VMS COPY OVMX -> local: link 1 = DIRECTORY LIST, link 2 = OPEN + GET. */
    static const char *dir_in[] = {
        "0100240407030702000500f7fbd9ffaeac8694e77f",
        "030006010a47524545542e5458543b",
    };
    static struct script ds; memset(&ds, 0, sizeof ds); ds.in = dir_in; ds.nin = 2;
    t.ctx = &ds; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    CHECK(st == 1 && ds.nout == 4 &&
          !strcmp(ds.out[1], "0f00040a444b41303a5b5352565d") &&
          !strcmp(ds.out[2], "0f00020b47524545542e5458543b31") && !strcmp(ds.out[3], "070002"),
          "DIRECTORY LIST = NAME(directory), NAME(file), ACCESS COMPLETE(RESPONSE)");

    static const char *get_in[] = {
        "01003c1007030702000500f7fbd9ffaeac8694e77f",
        "02020baf2001000202000080801003000101194c4142244449534b3a5b5352565d47524545542e5458543b3142028102",
        "0400020800",
        "040001090300",
        "070004",
        "07000100",
    };
    static struct script gs; memset(&gs, 0, sizeof gs); gs.in = get_in; gs.nin = 6;
    t.ctx = &gs; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    int d1 = saw(&gs, "080000536572766564"), eof = saw(&gs, "09002750");
    CHECK(st == 1, "VMS GET session completes");
    CHECK(saw(&gs, "0f0001") >= 0, "OPEN with DISPLAY NAME returns the resultant NAME before ACK");
    CHECK(d1 >= 0 && eof > d1, "RAC=file transfer GET streams the records then STATUS EOF (MAC 5 / MIC 047)");
    CHECK(saw(&gs, "0800007365636f6e64207265636f7264005a") >= 0,
          "a record with an embedded NUL is sent whole (length-delimited, not truncated at the NUL)");

    /* 3. Honest misses: an unknown file in a DIRECTORY LIST and an OPEN. */
    static const char *miss_in[] = {
        "0100240407030702000500f7fbd9ffaeac8694e77f",
        "030006010a4e4f5045472e5458543b",
    };
    static struct script ms; memset(&ms, 0, sizeof ms); ms.in = miss_in; ms.nin = 2;
    t.ctx = &ms; t.rxlen = t.rxoff = 0;
    st = dnet_fal_server_run(&t);
    CHECK(st == SS$_NOSUCHFILE && saw(&ms, "09003240") >= 0,
          "a DIRECTORY LIST of a missing file is an honest STATUS FNF (MAC 4 / MIC 062)");

    printf("test_dnet_fal_server: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
