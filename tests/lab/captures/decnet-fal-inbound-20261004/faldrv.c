/* faldrv - LAB HARNESS ONLY (never shipped): runs the REAL dnet_fal_client_get/
 * put (OVMX's compiled FAL client + DAP codec) with its NSP segments piped to
 * dapprobe.py, which carries them over a real NSP link to a real VMS FAL. The
 * local-file side (rms_textfile_*) is stubbed onto POSIX stdio because a bare
 * pod has no executive; the DAP/FAL protocol logic under test is unmodified. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdint.h>
#include "dnet_fal.h"
#include "rms_textfile.h"
#include "sysuaf.h"
#include "dnet_cterm.h"

struct rms_textfile { FILE *f; };
rms_textfile_t *rms_textfile_open(const char *s0){ const char *rb=strrchr(s0,']'); char s[300]; snprintf(s,sizeof s,"%s",rb?rb+1:s0); char *c=strrchr(s,';'); if(c) *c=0; FILE*f=fopen(s,"r"); if(!f) return NULL; rms_textfile_t*t=malloc(sizeof *t); t->f=f; return t; }
int rms_textfile_getline(rms_textfile_t *t, char *b, size_t n, int *tl){ *tl=0; if(!fgets(b,(int)n,t->f)) return 0; b[strcspn(b,"\n")]=0; return 1; }
void rms_textfile_close(rms_textfile_t *t){ fclose(t->f); free(t); }
int rms_textfile_write_line(const char *s, const char *l){ FILE*f=fopen(s,"w"); if(!f) return -1; fprintf(f,"%s\n",l); fclose(f); return 0; }
int rms_textfile_append_line(const char *s, const char *l){ FILE*f=fopen(s,"a"); if(!f) return -1; fprintf(f,"%s\n",l); fclose(f); return 0; }
/* LAB STUB ONLY: the shipped search is RMS $PARSE/$SEARCH; here a POSIX glob in
 * the serve directory, names rendered as a fixed lab directory. */
#include <glob.h>
struct lsrch { glob_t g; size_t i; };
int dnet_fal_search_begin(const char *spec, void **ctx){ char pat[300]; const char *rb=strrchr(spec,']'); snprintf(pat,sizeof pat,"%s",rb?rb+1:spec); char *c=strrchr(pat,';'); if(c) *c=0;
  struct lsrch *s=calloc(1,sizeof *s); if(glob(pat,0,NULL,&s->g)!=0){free(s);return -1;} *ctx=s; return 0; }
int dnet_fal_search_next(void *ctx, char *rsa, size_t cap){ struct lsrch *s=ctx; if(s->i>=s->g.gl_pathc) return -1; snprintf(rsa,cap,"LAB$DISK:[SRV]%s;1",s->g.gl_pathv[s->i++]); return 0; }
void dnet_fal_search_end(void *ctx){ struct lsrch *s=ctx; globfree(&s->g); free(s); }
int dnet_fal_wopen(const char *spec, uint8_t rfm, uint8_t rat, void **h, char *rsa, size_t cap){ (void)rfm;(void)rat; const char *rb=strrchr(spec,']'); char s[300]; snprintf(s,sizeof s,"%s",rb?rb+1:spec); char *c=strrchr(s,';'); if(c) *c=0;
  FILE *f=fopen(s,"w"); if(!f) return -1; *h=f; if(rsa&&cap) snprintf(rsa,cap,"LAB$DISK:[SRV]%s;1",s); return 0; }
int dnet_fal_wput(void *h, const uint8_t *rec, size_t len){ fwrite(rec,1,len,(FILE*)h); fputc('\n',(FILE*)h); return 0; }
int dnet_fal_wclose(void *h){ return fclose((FILE*)h)==0?0:-1; }
int sysuaf_lookup(const char *u, sysuaf_record_t *r){ (void)u; (void)r; return -1; }
int sysuaf_authenticate(const sysuaf_record_t *r, const char *p){ (void)r; (void)p; return 0; }
int sysuaf_interactive_login_permitted(const sysuaf_record_t *r){ (void)r; return 0; }

static int tsend(void *c, const uint8_t *seg0, size_t n){ (void)c;
  uint8_t segbuf[4096]; const uint8_t *seg = seg0; const char *cap = getenv("CFG_SYSCAP");
  if (cap && *cap && n >= 11 && seg0[0] == 1 && seg0[1] == 0) { /* LAB: override CONFIG SYSCAP */
    memcpy(segbuf, seg0, 11); size_t k = 11; for (const char *q = cap; q[0] && q[1]; q += 2) { unsigned v; sscanf(q, "%2x", &v); segbuf[k++] = (uint8_t)v; }
    seg = segbuf; n = k; }
  printf("S "); for(size_t i=0;i<n;i++) printf("%02x",seg[i]); printf("\n"); fflush(stdout); return 0; }
static int trecv(void *c, uint8_t *b, size_t cap, size_t *n){ (void)c; printf("R\n"); fflush(stdout); char line[8192]; if(!fgets(line,sizeof line,stdin)) return -1; if(line[0]=='-') return -1; size_t k=0; for(char*p=line; p[0]&&p[1]&&p[0]!='\n'&&k<cap; p+=2){ unsigned v; sscanf(p,"%2x",&v); b[k++]=(uint8_t)v; } *n=k; return 0; }

int main(int argc, char **argv){
  if (strcmp(argv[1],"ci")==0) { uint8_t b[256]; size_t n=0;
    if (dnet_cterm_sc_connect_build(DNET_OBJ_FAL,"OVMX",0x0001,0x0004,argv[2],argv[3],"",b,sizeof b,&n)!=0) return 2;
    for(size_t i=0;i<n;i++) printf("%02x",b[i]); printf("\n"); return 0; }
  static struct dnet_dap_transport t; t.send=tsend; t.recv=trecv;
  if (strcmp(argv[1],"serve")==0) {
    if (argc > 2 && chdir(argv[2]) != 0) return 3;
    uint32_t st = dnet_fal_server_run(&t);
    printf("X %08X\n", st); fflush(stdout); return st == 1 ? 0 : 1;
  }
  uint32_t st = (strcmp(argv[1],"get")==0) ? dnet_fal_client_get(argv[2], argv[3], &t)
                                           : dnet_fal_client_put(argv[3], argv[2], &t);
  printf("X %08X\n", st); fflush(stdout); return st == 1 ? 0 : 1;
}
