/*
 * ovmx_decc_p32.c - DEC C RTL 32-bit-pointer entry points that cannot simply be
 * the 64-bit implementation (vms-5bc9). Alpha only.
 *
 * Compiled by the alpha-dec-vms cc1 with the DEC C DEFAULT pointer size
 * (32-bit), so each definition below is decorated to the 32-bit entry name
 * (strtol -> decc$strtol) and each call to _<name>64 binds the 64-bit
 * implementation (decc$_<name>64) DECC$SHR already defines. These functions
 * hand a pointer back THROUGH a pointer argument (char **endptr, ...): the
 * 64-bit implementation would store 8 bytes into the 32-bit caller's 4-byte
 * slot, so each wrapper receives the result in a 64-bit local and stores it as
 * a 32-bit pointer. Every such pointer points into the caller's own (32-bit
 * addressable) data, so the narrowing is exact. Pointer-transparent functions
 * need no code -- see decc_p32_alias.txt.
 *
 * No C RTL headers: this TU sees no musl declarations (built 64-bit); the few
 * prototypes it needs are declared here, the 64-bit ones under
 * #pragma __pointer_size 64.
 */
/* Alpha VMS only (an empty translation unit elsewhere, so host-side source
 * scans that compile every product file still can). */
#if defined(__alpha) && defined(__VMS)

typedef unsigned int size_t;
typedef int wchar_t;
struct dirent;
typedef struct __mbstate { unsigned int __opaque1, __opaque2; } mbstate_t;

#pragma __pointer_size __save
#pragma __pointer_size 64
typedef char *c64;
typedef const char *cc64;
typedef wchar_t *w64;
typedef const wchar_t *cw64;
typedef struct dirent *d64;
extern double             _strtod64  (cc64, c64 *);
extern long               _strtol64  (cc64, c64 *, int);
extern long long          _strtoll64 (cc64, c64 *, int);
extern unsigned long      _strtoul64 (cc64, c64 *, int);
extern unsigned long long _strtoull64(cc64, c64 *, int);
extern double             _wcstod64  (cw64, w64 *);
extern long               _wcstol64  (cw64, w64 *, int);
extern unsigned long      _wcstoul64 (cw64, w64 *, int);
extern c64                _strsep64  (c64 *, cc64);
extern c64                _strtok_r64(c64, cc64, c64 *);
extern w64                _wcstok64  (w64, cw64, w64 *);
extern size_t             _mbsrtowcs64(w64, cc64 *, size_t, mbstate_t *);
extern size_t             _wcsrtombs64(c64, cw64 *, size_t, mbstate_t *);
extern int                _readdir_r64(void *, d64, d64 *);
#pragma __pointer_size __restore

/* A pointer the 64-bit implementation produced into the caller's 32-bit data. */
#define P32(T, p) ((T)(unsigned int)(unsigned long long)(p))

double strtod(const char *s, char **e)
{ c64 e64; double r = _strtod64(s, &e64); if (e) *e = P32(char *, e64); return r; }

long strtol(const char *s, char **e, int b)
{ c64 e64; long r = _strtol64(s, &e64, b); if (e) *e = P32(char *, e64); return r; }

long long strtoll(const char *s, char **e, int b)
{ c64 e64; long long r = _strtoll64(s, &e64, b); if (e) *e = P32(char *, e64); return r; }

unsigned long strtoul(const char *s, char **e, int b)
{ c64 e64; unsigned long r = _strtoul64(s, &e64, b); if (e) *e = P32(char *, e64); return r; }

unsigned long long strtoull(const char *s, char **e, int b)
{ c64 e64; unsigned long long r = _strtoull64(s, &e64, b); if (e) *e = P32(char *, e64); return r; }

double wcstod(const wchar_t *s, wchar_t **e)
{ w64 e64; double r = _wcstod64(s, &e64); if (e) *e = P32(wchar_t *, e64); return r; }

long wcstol(const wchar_t *s, wchar_t **e, int b)
{ w64 e64; long r = _wcstol64(s, &e64, b); if (e) *e = P32(wchar_t *, e64); return r; }

unsigned long wcstoul(const wchar_t *s, wchar_t **e, int b)
{ w64 e64; unsigned long r = _wcstoul64(s, &e64, b); if (e) *e = P32(wchar_t *, e64); return r; }

char *strsep(char **sp, const char *delim)
{
    c64 s64 = *sp;
    c64 tok = _strsep64(&s64, delim);
    *sp = P32(char *, s64);
    return P32(char *, tok);
}

char *strtok_r(char *s, const char *delim, char **save)
{
    c64 sv64 = *save;
    c64 tok = _strtok_r64(s, delim, &sv64);
    *save = P32(char *, sv64);
    return P32(char *, tok);
}

wchar_t *wcstok(wchar_t *s, const wchar_t *delim, wchar_t **save)
{
    w64 sv64 = *save;
    w64 tok = _wcstok64(s, delim, &sv64);
    *save = P32(wchar_t *, sv64);
    return P32(wchar_t *, tok);
}

size_t mbsrtowcs(wchar_t *d, const char **src, size_t n, mbstate_t *st)
{
    cc64 s64 = *src;
    size_t r = _mbsrtowcs64(d, &s64, n, st);
    *src = P32(const char *, s64);
    return r;
}

size_t wcsrtombs(char *d, const wchar_t **src, size_t n, mbstate_t *st)
{
    cw64 s64 = *src;
    size_t r = _wcsrtombs64(d, &s64, n, st);
    *src = P32(const wchar_t *, s64);
    return r;
}

int readdir_r(void *dir, struct dirent *ent, struct dirent **res)
{
    d64 r64;
    int rc = _readdir_r64(dir, ent, &r64);
    *res = P32(struct dirent *, r64);
    return rc;
}

#endif /* __alpha && __VMS */
