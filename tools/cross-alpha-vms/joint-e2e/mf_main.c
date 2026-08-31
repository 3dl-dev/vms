/* mf_main.c (vms-bdd) — the MAIN translation unit of the multi-.o joint-e2e
 * proof. The rung above crtl_rms_test.c (which is single-object): a REAL
 * multi-file C program whose main() calls ACROSS a genuine .o boundary into
 * mf_util.obj (mf_dup/mf_len/mf_eq/mf_free), which in turn pulls the libc
 * surface (malloc/free/memcpy/strlen/strcmp) from the genuine alpha DECC$SHR.
 *
 * Built by the REAL alpha-dec-vms cross cc1 (-mpointer-size=64) as a SEPARATE
 * object from mf_util.c, linked crt0.obj + mf_main.obj + mf_util.obj STRICT
 * (no --allow-undefined) against the genuine DECC$SHR + LIBOTS$SHR, then
 * activated on qemu-system-alpha + the real /dev/vms executive over the ODS-2
 * ACP. Proves LINK.EXE scales to N objects with a real import set AND that the
 * decc$free-class weak-alias export (PR #795) + the N=7 thunk-descriptor fix
 * (PR #958) hold end-to-end for a genuine multi-file program.
 *
 * SENTINEL-RETURN CONVENTION (deterministic; main returns a constant on the
 * success path). crt0 maps the return N through C$_EXIT1, so BOOT-A decodes
 * $STATUS as C$_EXIT1 + (N-1)*8:
 *
 *   5 = FULL SUCCESS  (cross-.o malloc/free/strlen/strcmp/memcpy round-trip)
 *   1 = mf_len (strlen) across the boundary returned the wrong length
 *   2 = mf_dup (malloc+memcpy) across the boundary returned NULL
 *   3 = mf_eq (strcmp) identity mismatch on the heap copy
 *   4 = mf_eq (strcmp) negative test wrongly matched
 */
typedef unsigned long ovmx_size_t;

/* Cross-.o entry points — DEFINED in mf_util.obj, resolved by LINK.EXE at link
 * time (intra-image, not a DECC$SHR import). */
extern void       *mf_dup(const void *, ovmx_size_t);
extern ovmx_size_t mf_len(const char *);
extern int         mf_eq(const char *, const char *);
extern void        mf_free(void *);

extern int printf(const char *, ...);

int main(int argc, char **argv, char **envp)
{
    (void)argv; (void)envp;

    const char *msg = "OVMX multi-file port test";   /* 25 chars */

    /* 1. strlen across the boundary. */
    ovmx_size_t n = mf_len(msg);
    if (n != 25)
        return 1;

    /* 2. heap dup (malloc + memcpy) across the boundary. */
    char *copy = (char *)mf_dup(msg, n + 1);
    if (!copy)
        return 2;

    /* 3. strcmp identity across the boundary. */
    if (!mf_eq(copy, msg)) {
        mf_free(copy);
        return 3;
    }

    /* 4. strcmp negative test across the boundary (must NOT match). */
    if (mf_eq(copy, "a different string")) {
        mf_free(copy);
        return 4;
    }

    /* 5. free across the boundary + success sentinel. */
    mf_free(copy);
    printf("OVMX multi-file port test: OK (cross-.o malloc/free/strlen/strcmp/memcpy) argc=%d\n", argc);
    return 5;   /* success -> $STATUS = C$_EXIT1 + (5-1)*8 = 0x0035A029 */
}
