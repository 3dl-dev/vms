/* mf_util.c (vms-bdd) — the HELPER translation unit of the multi-.o joint-e2e
 * proof. Compiled by the REAL alpha-dec-vms cross cc1 into its OWN object
 * (mf_util.obj), SEPARATE from mf_main.obj, so LINK.EXE must resolve BOTH
 * halves of a real program: (a) the intra-image cross-.o references (mf_main
 * -> mf_dup/mf_len/mf_eq/mf_free, defined HERE) at link time, and (b) the
 * genuine libc surface THIS TU pulls in (malloc/free/memcpy/strlen/strcmp) as
 * decc$ imports bound to the genuine alpha DECC$SHR.
 *
 * This TU is the exact reason the FIRST vms-bdd attempt (2026-08-27, BEFORE
 * PR #795 + #958) hit `%LINK-F-UNDEF, EVAX: undefined symbol 'decc$free'
 * referenced by <util>.obj`: musl-alpha defines `free = weak_alias(__libc_free,
 * free)`, the cross cc1 emitted the alias as an equate nm did not report, so
 * mk_decc_shr.sh's ALPHA branch never exported decc$free, and a SEPARATE object
 * referencing free() deferred it under a STRICT (no --allow-undefined) link.
 * PR #795 (mk_decc_shr.sh: export the whole weak-alias-equate class) closes
 * that; this program re-runs the STRICT multi-.o link + activation to prove it.
 *
 * alpha-dec-vms is LP64 (-mpointer-size=64); no libc headers are set up in the
 * cross image, so declare the referenced names by hand (matching joint_main.c /
 * crtl_rms_test.c style) — the NAMES are what the link binds, and the cross cc1
 * decorates them to the decc$ surface at codegen. */
typedef unsigned long ovmx_size_t;

extern void       *malloc(ovmx_size_t);
extern void        free(void *);
extern void       *memcpy(void *, const void *, ovmx_size_t);
extern ovmx_size_t strlen(const char *);
extern int         strcmp(const char *, const char *);

/* Duplicate n bytes onto the heap: malloc + memcpy (both from DECC$SHR). */
void *mf_dup(const void *src, ovmx_size_t n)
{
    void *p = malloc(n);
    if (p)
        memcpy(p, src, n);
    return p;
}

/* strlen across the .o boundary. */
ovmx_size_t mf_len(const char *s)
{
    return strlen(s);
}

/* strcmp identity test across the .o boundary. */
int mf_eq(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

/* free across the .o boundary — the symbol whose decc$free was UNDEF pre-#795. */
void mf_free(void *p)
{
    free(p);
}
