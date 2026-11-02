/*
 * link_cmd.h - the link a command asks for (rd vms-3cb1). Included by link.c
 * and by its two front ends (link_cli.c, link_buildtool.c).
 */
#ifndef OVMX_LINK_CMD_H
#define OVMX_LINK_CMD_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef OVMX_LINK_C
/* Compiled on its own (the product-source symbol scan compiles every file):
 * the names link.c supplies. Included by link.c (OVMX_LINK_C), none of this. */
void die(const char *msg);
static int g_noinform;
#endif

/* --------------------------------------------------------------------------
 * The link a command asks for. Every front end fills this one
 * structure and link_run() acts on it; the linker proper never sees a command
 * line. LINK.EXE's front end is the VMS LINK command, read through the CLI$
 * routines against the LINK command table (link_cli.c, link_cld.h). The
 * build-machine front end of the host tool (link_buildtool.c) is compiled only
 * into the tool that builds OVMX on Linux, never into a shipped image.
 * -------------------------------------------------------------------------- */
struct link_include { const char *lib; const char *module; };

struct link_cmd {
    const char  *out;                 /* the image file */
    int          shareable, executable, allow_undef;
    const char **ins;   int nin,   cap_ins;     /* input files, link order */
    const char **libs;  int nlibs, cap_libs;    /* libraries searched ... */
    int         *lib_pos;                       /* ... after this many inputs */
    const char **uses;  int nuse,  cap_uses;    /* shareable images bound */
    struct link_include *incl; int nincl, cap_incl; /* /INCLUDE=(module) */
    char        *symvec;              /* SYMBOL_VECTOR entries, comma-joined */
    char        *gsmatch;             /* GSMATCH=kind,major,minor */
    const char  *transfer;            /* EVAX main transfer symbol */
    int          base_set;
    unsigned long long base;          /* EVAX executable P0 base */
    const char  *map;                 /* /MAP file, or NULL */
    int          map_brief;           /* /BRIEF */
};

__attribute__((unused)) static void *lc_grow(void *v, int *cap, int n, size_t esz)
{
    if (n + 1 < *cap) return v;
    *cap = *cap ? *cap * 2 : 16;
    v = realloc(v, (size_t)*cap * esz);
    if (!v) die("oom building the link");
    return v;
}
__attribute__((unused)) static void lc_input(struct link_cmd *c, const char *f)
{
    c->ins = lc_grow(c->ins, &c->cap_ins, c->nin, sizeof *c->ins);
    c->ins[c->nin++] = f;
}
__attribute__((unused)) static void lc_library(struct link_cmd *c, const char *f)
{
    if (c->nlibs + 1 >= c->cap_libs) {
        c->cap_libs = c->cap_libs ? c->cap_libs * 2 : 16;
        c->libs = realloc(c->libs, (size_t)c->cap_libs * sizeof *c->libs);
        c->lib_pos = realloc(c->lib_pos, (size_t)c->cap_libs * sizeof *c->lib_pos);
        if (!c->libs || !c->lib_pos) die("oom building the link");
    }
    c->lib_pos[c->nlibs] = c->nin;      /* plain inputs before it (link order) */
    c->libs[c->nlibs++] = f;
}
__attribute__((unused)) static void lc_use(struct link_cmd *c, const char *f)
{
    c->uses = lc_grow(c->uses, &c->cap_uses, c->nuse, sizeof *c->uses);
    c->uses[c->nuse++] = f;
}
__attribute__((unused)) static void lc_include(struct link_cmd *c, const char *lib, const char *module)
{
    c->incl = lc_grow(c->incl, &c->cap_incl, c->nincl, sizeof *c->incl);
    c->incl[c->nincl].lib = lib;
    c->incl[c->nincl++].module = module;
}
__attribute__((unused)) static void lc_symvec(struct link_cmd *c, const char *entries)
{
    size_t o = c->symvec ? strlen(c->symvec) : 0, n = strlen(entries);
    char *v = realloc(c->symvec, o + n + 2);
    if (!v) die("oom building the symbol vector");
    if (o) v[o++] = ',';
    memcpy(v + o, entries, n + 1);
    c->symvec = v;
}

#ifdef OVMX_LINK_C
static int link_run(struct link_cmd *c);
#else
int link_run(struct link_cmd *c);
#endif

#endif
