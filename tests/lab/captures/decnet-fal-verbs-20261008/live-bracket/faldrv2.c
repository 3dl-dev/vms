/* faldrv2 - LAB HARNESS ONLY (never shipped; rd vms-b2f): runs OVMX's compiled
 * dnet_fal_server_run with its NSP segments piped to dapprobe_skipci.py. The
 * RMS hooks are POSIX stand-ins in the serve directory, naming files the way a
 * VMS FAL names SYS$LOGIN (SYS$SYSDEVICE:[SYSMGR]) so the run isolates the DAP
 * protocol. CFG_VER (hex vernum..usrsoft) / CFG_SYSCAP override the CONFIG. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include <glob.h>
#include <sys/stat.h>
#include "dnet_fal.h"
#include "sysuaf.h"
#include "rmsdef.h"
#include "ssdef.h"

#define DIRP "SYS$SYSDEVICE:[SYSMGR]"
static void base(const char *spec, char *o, size_t n)
{ const char *rb = strrchr(spec, ']'); if (!rb) rb = strrchr(spec, ':');
  snprintf(o, n, "%s", rb ? rb + 1 : spec); char *c = strrchr(o, ';'); if (c) *c = 0; }
struct lsrch { glob_t g; size_t i; int ok; char esa[300]; };
int dnet_fal_search_begin(const char *spec, void **ctx)
{ struct lsrch *s = calloc(1, sizeof *s); char pat[300]; base(spec, pat, sizeof pat);
  s->ok = (glob(pat, 0, NULL, &s->g) == 0);
  const char *rb = strrchr(spec, ']'); if (!rb) rb = strrchr(spec, ':');
  snprintf(s->esa, sizeof s->esa, DIRP "%s", rb ? rb + 1 : spec); *ctx = s; return 0; }
int dnet_fal_search_next(void *ctx, char *rsa, size_t cap)
{ struct lsrch *s = ctx; if (!s->ok || s->i >= s->g.gl_pathc) return -1;
  snprintf(rsa, cap, DIRP "%s;1", s->g.gl_pathv[s->i++]); return 0; }
void dnet_fal_search_end(void *ctx) { struct lsrch *s = ctx; if (s->ok) globfree(&s->g); free(s); }
uint32_t dnet_fal_search_status(void *ctx, uint32_t *stv, char *esa, size_t cap)
{ struct lsrch *s = ctx; if (stv) *stv = 0; if (esa && cap) snprintf(esa, cap, "%s", s ? s->esa : "");
  return (s && s->ok && s->i > 0) ? RMS$_NMF : RMS$_FNF; }
int dnet_fal_fileattr(const char *spec, struct dnet_fal_fattr *o, uint32_t *sts)
{ char n[300]; base(spec, n, sizeof n); struct stat st; memset(o, 0, sizeof *o);
  if (stat(n, &st) != 0) { if (sts) *sts = RMS$_FNF; return -1; }
  if (!strncmp(n, "PRIV", 4)) { if (sts) *sts = RMS$_PRV; return -1; }   /* lab: refused */
  o->rfm = 2; o->rat = 2; o->lrl = 80; o->alq = 1 + (uint32_t)st.st_size / 512; o->ebk = o->alq;
  o->ffb = (uint16_t)(st.st_size % 512); o->fileprot = 0xfa00; o->uic_group = 1; o->uic_member = 4;
  o->revision = 1; if (sts) *sts = RMS$_NORMAL; return 0; }
int dnet_fal_erase(const char *spec, uint32_t *sts, uint32_t *stv)
{ char n[300]; base(spec, n, sizeof n); *stv = 0;
  if (!strncmp(n, "PRIV", 4)) { *sts = RMS$_PRV; *stv = SS$_NOPRIV; return -1; }
  if (unlink(n) != 0) { *sts = RMS$_FNF; return -1; } *sts = RMS$_NORMAL; return 0; }
int dnet_fal_rename(const char *o, const char *nw, uint32_t *sts, uint32_t *stv)
{ char a[300], b[300]; base(o, a, sizeof a); base(nw, b, sizeof b); *stv = 0;
  if (!strncmp(a, "PRIV", 4)) { *sts = RMS$_PRV; *stv = SS$_NOPRIV; return -1; }
  if (rename(a, b) != 0) { *sts = RMS$_FNF; return -1; } *sts = RMS$_NORMAL; return 0; }
int dnet_fal_ropen_st(const char *spec, void **h, uint8_t *rfm, uint8_t *rat, uint32_t *sts)
{ char n[300]; base(spec, n, sizeof n);
  if (!strncmp(n, "PRIV", 4)) { if (sts) *sts = RMS$_PRV; return -1; }
  FILE *f = fopen(n, "r"); if (!f) { if (sts) *sts = RMS$_FNF; return -1; }
  if (rfm) *rfm = 2; if (rat) *rat = 2; if (sts) *sts = RMS$_NORMAL; *h = f; return 0; }
int dnet_fal_ropen(const char *s, void **h, uint8_t *rfm, uint8_t *rat) { return dnet_fal_ropen_st(s, h, rfm, rat, NULL); }
int dnet_fal_rget(void *h, uint8_t *rec, size_t cap, size_t *len)
{ char b[2048]; if (!fgets(b, sizeof b, (FILE *)h)) return 0; size_t n = strcspn(b, "\n");
  if (n > cap) return -1; memcpy(rec, b, n); *len = n; return 1; }
int dnet_fal_rclose(void *h) { return fclose((FILE *)h) == 0 ? 0 : -1; }
int dnet_fal_wopen(const char *spec, uint8_t rfm, uint8_t rat, void **h, char *rsa, size_t cap)
{ (void)rfm; (void)rat; char n[300]; base(spec, n, sizeof n); FILE *f = fopen(n, "w"); if (!f) return -1;
  *h = f; if (rsa && cap) snprintf(rsa, cap, DIRP "%s;1", n); return 0; }
int dnet_fal_wput(void *h, const uint8_t *rec, size_t len) { fwrite(rec, 1, len, (FILE *)h); fputc('\n', (FILE *)h); return 0; }
int dnet_fal_wclose(void *h) { return fclose((FILE *)h) == 0 ? 0 : -1; }
int sysuaf_lookup(const char *u, sysuaf_record_t *r) { (void)u; (void)r; return -1; }
int sysuaf_authenticate(const sysuaf_record_t *r, const char *p) { (void)r; (void)p; return 0; }
int sysuaf_interactive_login_permitted(const sysuaf_record_t *r) { (void)r; return 0; }

static void hexin(const char *q, uint8_t *o, size_t *k) { for (; q[0] && q[1]; q += 2) { unsigned v; sscanf(q, "%2x", &v); o[(*k)++] = (uint8_t)v; } }
static int tsend(void *c, const uint8_t *seg0, size_t n)
{ (void)c; uint8_t sb[4096]; const uint8_t *seg = seg0;
  const char *ver = getenv("CFG_VER"), *cap = getenv("CFG_SYSCAP");
  if (n >= 11 && seg0[0] == 1 && seg0[1] == 0 && ((ver && *ver) || (cap && *cap))) {
    size_t k = 6; memcpy(sb, seg0, 6);
    if (ver && *ver) hexin(ver, sb, &k); else { memcpy(sb + 6, seg0 + 6, 5); k = 11; }
    if (cap && *cap) hexin(cap, sb, &k); else { memcpy(sb + k, seg0 + 11, n - 11); k += n - 11; }
    seg = sb; n = k; }
  printf("S "); for (size_t i = 0; i < n; i++) printf("%02x", seg[i]); printf("\n"); fflush(stdout); return 0; }
static int trecv(void *c, uint8_t *b, size_t cap, size_t *n)
{ (void)c; printf("R\n"); fflush(stdout); char line[8192]; if (!fgets(line, sizeof line, stdin)) return -1;
  if (line[0] == '-') return -1; size_t k = 0;
  for (char *p = line; p[0] && p[1] && p[0] != '\n' && k < cap; p += 2) { unsigned v; sscanf(p, "%2x", &v); b[k++] = (uint8_t)v; }
  *n = k; return 0; }
int main(int argc, char **argv)
{ static struct dnet_dap_transport t; t.send = tsend; t.recv = trecv;
  if (argc > 2 && chdir(argv[2]) != 0) return 3;
  uint32_t st = dnet_fal_server_run(&t); printf("X %08X\n", st); fflush(stdout); return st == 1 ? 0 : 1; }
