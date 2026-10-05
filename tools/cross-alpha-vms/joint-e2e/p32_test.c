/*
 * p32_test.c - vms-5bc9: a DEC C default (32-bit pointer) program on OVMX/Alpha.
 *
 * Built WITHOUT -mpointer-size=64, so every pointer is a longword and the
 * port's crtlmap binds the 32-bit DECC$SHR entry points (decc$strcpy,
 * decc$strtol, decc$malloc ...). Exercises the pointer-transparent string /
 * memory / allocator surface, and the pointer-to-pointer functions whose 32-bit
 * entry points must store a LONGWORD through the out-pointer (a 64-bit store
 * would overwrite the guard word placed right after it). Sentinel 7 = all held.
 */
typedef unsigned int size_t;
extern int printf(const char *, ...);
extern void *malloc(size_t);
extern void *calloc(size_t, size_t);
extern void *realloc(void *, size_t);
extern void free(void *);
extern char *strdup(const char *);
extern char *strcpy(char *, const char *);
extern char *strcat(char *, const char *);
extern size_t strlen(const char *);
extern int strcmp(const char *, const char *);
extern char *strchr(const char *, int);
extern char *strrchr(const char *, int);
extern char *strstr(const char *, const char *);
extern void *memset(void *, int, size_t);
extern void *memcpy(void *, const void *, size_t);
extern void *memmove(void *, const void *, size_t);
extern long strtol(const char *, char **, int);
extern unsigned long strtoul(const char *, char **, int);
extern double strtod(const char *, char **);
extern char *strsep(char **, const char *);
extern char *strtok_r(char *, const char *, char **);
extern void qsort(void *, size_t, size_t, int (*)(const void *, const void *));

static int cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }

struct outp { char *p; unsigned int guard; };

int main(int argc, char **argv)
{
    (void)argv;
    int ok_str = 0, ok_mem = 0, ok_alloc = 0, ok_strto = 0, ok_tok = 0, ok_sort = 0;

    /* sizeof(char *) is the proof this TU really is 32-bit. */
    int p32 = sizeof(char *) == 4;

    char *s = malloc(64);
    if (s) {
        strcpy(s, "OVMX ");
        strcat(s, "32-bit pointers");
        char *d = strdup(s);
        ok_str = strlen(s) == 20 && strchr(s, '3') == s + 5 && strrchr(s, 'p') == s + 12 &&
                 strstr(s, "point") == s + 12 && d && strcmp(d, s) == 0;
        free(d);
    }

    char buf[32];
    memset(buf, 'x', sizeof buf);
    memcpy(buf, "abcdef", 7);
    memmove(buf + 1, buf, 6);
    ok_mem = buf[0] == 'a' && buf[1] == 'a' && buf[6] == 'f' && buf[31] == 'x';

    char *big = realloc(s, 1u << 20);              /* grows through mremap/copy */
    int *z = calloc(1000, sizeof(int));
    if (big && z) {
        big[(1u << 20) - 1] = 'q';
        ok_alloc = big[0] == 'O' && z[999] == 0 && big[(1u << 20) - 1] == 'q';
    }

    static const char num[] = "  -123xyz 0x1fq 2.5rest";
    struct outp o1 = { 0, 0x5a5a5a5au }, o2 = { 0, 0x5a5a5a5au }, o3 = { 0, 0x5a5a5a5au };
    long a = strtol(num, &o1.p, 10);
    unsigned long b = strtoul(o1.p + 3, &o2.p, 16);
    double c = strtod(o2.p + 2, &o3.p);
    ok_strto = a == -123 && o1.p == num + 6 && b == 0x1f && o2.p == num + 14 &&
               c == 2.5 && o3.p == num + 19 &&
               o1.guard == 0x5a5a5a5au && o2.guard == 0x5a5a5a5au && o3.guard == 0x5a5a5a5au;

    char line[] = "alpha,vax,,axp";
    struct outp sp = { line, 0x5a5a5a5au };
    char *t1 = strsep(&sp.p, ",");
    char *t2 = strsep(&sp.p, ",");
    char *t3 = strsep(&sp.p, ",");
    char line2[] = "one two  three";
    struct outp sv = { 0, 0x5a5a5a5au };
    char *u1 = strtok_r(line2, " ", &sv.p);
    char *u2 = strtok_r(0, " ", &sv.p);
    char *u3 = strtok_r(0, " ", &sv.p);
    ok_tok = t1 && strcmp(t1, "alpha") == 0 && t2 && strcmp(t2, "vax") == 0 && t3 && *t3 == 0 &&
             sp.p == line + 11 && sp.guard == 0x5a5a5a5au &&
             u1 && strcmp(u1, "one") == 0 && u2 && strcmp(u2, "two") == 0 &&
             u3 && strcmp(u3, "three") == 0 && sv.guard == 0x5a5a5a5au;

    int v[6] = { 5, 3, 9, 1, 7, 2 };
    qsort(v, 6, sizeof v[0], cmp);
    ok_sort = v[0] == 1 && v[1] == 2 && v[5] == 9;

    free(big);
    free(z);
    int ok = p32 && ok_str && ok_mem && ok_alloc && ok_strto && ok_tok && ok_sort;
    printf("OVMX p32 test: ptr32=%d str=%d mem=%d alloc=%d strto=%d tok=%d sort=%d argc=%d\n",
           p32, ok_str, ok_mem, ok_alloc, ok_strto, ok_tok, ok_sort, argc);
    return ok ? 7 : 3;
}
