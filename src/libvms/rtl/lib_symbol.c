/*
 * lib_symbol.c - LIB$ CLI Symbol Table Routines
 *
 * LIB$SET_SYMBOL, LIB$DELETE_SYMBOL and LIB$GET_SYMBOL define, remove and
 * read DCL command-language symbols (LIB$K_CLI_LOCAL_SYM /
 * LIB$K_CLI_GLOBAL_SYM).
 *
 * UNDER DCL (rd vms-cded) they act on DCL's own symbol tables, as on VMS: the
 * call goes back to the CLI that activated the image (ovmx_cli.h) -- directly
 * when the image runs inside DCL's process, over the OVMX$CLI_FD channel when
 * DCL fork()+execve()d it. A symbol the image sets is in DCL's table when the
 * image exits, and the image reads DCL's symbols ($STATUS, P1, a global).
 *
 * WITH NO CLI (an image started some other way -- the corpus guest, a
 * detached process with no command interpreter): VMS answers LIB$_NOCLI. OVMX
 * keeps a process-local table there instead, so a program that sets and reads
 * its own symbols still works; it is not presented as DCL's table. (OVMX design
 * choice, recorded in the register below.)
 *
 * Symbol names are validated as DCL validates them (1..255 characters, a
 * letter, '$' or '_' first, then letters, digits, '$' and '_'):
 * LIB$_INVSYMNAM otherwise (observed on VAX V7.3 and Alpha V8.4,
 * docs/oracle/semantics/rtl/). Names are case-blind (upcased).
 *
 * Reference: OpenVMS RTL Library (LIB$) Manual -
 *            LIB$SET_SYMBOL, LIB$DELETE_SYMBOL, LIB$GET_SYMBOL
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include "ssdef.h"
#include "descrip.h"
#include "lib$routines.h"
#include "ovmx_cli.h"

#define SYM_MAX_NAME  256
#define SYM_MAX_VALUE OVMX_CLI_VALUE_MAX
#define SYM_MAX_COUNT 256

/* ---- the CLI ---------------------------------------------------------- */

static ovmx_cli_handler_t cli_handler;
static pthread_mutex_t cli_mutex = PTHREAD_MUTEX_INITIALIZER;

void lib$$set_cli_handler(ovmx_cli_handler_t handler)
{
    cli_handler = handler;
}

/* The fork()+execve() channel DCL handed this image, or -1. */
static int cli_fd(void)
{
    static int fd = -2;
    if (fd == -2) {
        const char *e = getenv(OVMX_CLI_FD_ENV);
        fd = -1;
        if (e && *e) {
            char *end;
            long v = strtol(e, &end, 10);
            if (*end == '\0' && v >= 0 && v < 65536 &&
                fcntl((int)v, F_GETFD) >= 0) {
                fd = (int)v;
                /* Ours alone: a process this image starts is not DCL's image. */
                (void)fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
        }
    }
    return fd;
}

/* Ask the CLI. Returns 1 with *rsp filled, 0 when there is no CLI. */
static int cli_call(const struct ovmx_cli_req *req, struct ovmx_cli_rsp *rsp)
{
    memset(rsp, 0, sizeof *rsp);
    if (cli_handler) {
        cli_handler(req, rsp);
        return 1;
    }
    int fd = cli_fd();
    if (fd < 0)
        return 0;
    pthread_mutex_lock(&cli_mutex);
    ssize_t n = write(fd, req, sizeof *req);
    if (n == (ssize_t)sizeof *req)
        n = read(fd, rsp, sizeof *rsp);
    pthread_mutex_unlock(&cli_mutex);
    if (n != (ssize_t)sizeof *rsp) {
        memset(rsp, 0, sizeof *rsp);
        rsp->status = LIB$_UNECLIERR;      /* the CLI went away mid-call */
    }
    return 1;
}

/* ---- the no-CLI table -------------------------------------------------- */

struct cli_symbol {
    char     name[SYM_MAX_NAME];
    char     value[SYM_MAX_VALUE];
    uint16_t value_len;
    uint32_t table_type;   /* LIB$K_CLI_LOCAL_SYM or LIB$K_CLI_GLOBAL_SYM */
    int      in_use;
};

static struct cli_symbol symbol_table[SYM_MAX_COUNT];
static pthread_mutex_t symbol_mutex = PTHREAD_MUTEX_INITIALIZER;
static int symbol_table_seeded = 0;

/* $STATUS, the one reserved symbol a command procedure always has (DCL
 * Concepts Manual, "Reserved Global Symbols"), seeded once to SS$_NORMAL. */
static void seed_reserved_symbols_locked(void) {
    if (symbol_table_seeded)
        return;
    symbol_table_seeded = 1;

    struct cli_symbol *entry = &symbol_table[0];
    strncpy(entry->name, "$STATUS", sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    static const char default_status[] = "%X00000001";
    memcpy(entry->value, default_status, sizeof(default_status) - 1);
    entry->value_len = sizeof(default_status) - 1;
    entry->table_type = LIB$K_CLI_LOCAL_SYM;
    entry->in_use = 1;
}

static struct cli_symbol *find_symbol_locked(const char *name, uint32_t table_type) {
    seed_reserved_symbols_locked();
    for (int i = 0; i < SYM_MAX_COUNT; i++) {
        if (symbol_table[i].in_use &&
            symbol_table[i].table_type == table_type &&
            strcasecmp(symbol_table[i].name, name) == 0) {
            return &symbol_table[i];
        }
    }
    return NULL;
}

static uint32_t local_set(const char *name, const char *val, uint16_t vlen,
                          uint32_t ttype)
{
    pthread_mutex_lock(&symbol_mutex);
    struct cli_symbol *entry = find_symbol_locked(name, ttype);
    if (!entry) {
        for (int i = 0; i < SYM_MAX_COUNT; i++) {
            if (!symbol_table[i].in_use) {
                entry = &symbol_table[i];
                break;
            }
        }
    }
    if (!entry) {
        pthread_mutex_unlock(&symbol_mutex);
        return LIB$_INSCLIMEM;
    }
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    if (vlen > 0)
        memcpy(entry->value, val, vlen);
    entry->value_len = vlen;
    entry->table_type = ttype;
    entry->in_use = 1;
    pthread_mutex_unlock(&symbol_mutex);
    return SS$_NORMAL;
}

static uint32_t local_delete(const char *name, uint32_t ttype)
{
    pthread_mutex_lock(&symbol_mutex);
    struct cli_symbol *entry = find_symbol_locked(name, ttype);
    if (entry)
        entry->in_use = 0;
    pthread_mutex_unlock(&symbol_mutex);
    return entry ? SS$_NORMAL : LIB$_NOSUCHSYM;
}

static uint32_t local_get(const char *name, struct ovmx_cli_rsp *rsp)
{
    pthread_mutex_lock(&symbol_mutex);
    uint32_t t = LIB$K_CLI_LOCAL_SYM;
    struct cli_symbol *entry = find_symbol_locked(name, t);
    if (!entry) {
        t = LIB$K_CLI_GLOBAL_SYM;
        entry = find_symbol_locked(name, t);
    }
    if (entry) {
        rsp->status = SS$_NORMAL;
        rsp->table = t;
        rsp->value_len = entry->value_len;
        memcpy(rsp->value, entry->value, entry->value_len);
    } else {
        rsp->status = LIB$_NOSUCHSYM;
    }
    pthread_mutex_unlock(&symbol_mutex);
    return rsp->status;
}

/* ---- argument checks ---------------------------------------------------- */

/* Copy and upcase a symbol name; LIB$_INVSYMNAM unless it is a DCL name. */
static uint32_t symbol_name(const struct dsc$descriptor_s *d, char *out)
{
    uint16_t n = d->dsc$w_length;
    if (n == 0 || n > OVMX_CLI_NAME_MAX)
        return LIB$_INVSYMNAM;
    for (uint16_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)d->dsc$a_pointer[i];
        int ok = isalpha(c) || c == '$' || c == '_' || (i > 0 && isdigit(c));
        if (!ok)
            return LIB$_INVSYMNAM;
        out[i] = (char)toupper(c);
    }
    out[n] = '\0';
    return SS$_NORMAL;
}

/*
 * lib$set_symbol - Define a CLI symbol.
 *
 * @param symbol      Descriptor of the symbol name (required).
 * @param value       Descriptor of the symbol's new value (required).
 * @param table_type  Optional pointer to LIB$K_CLI_LOCAL_SYM or
 *                    LIB$K_CLI_GLOBAL_SYM (default local).
 */
uint32_t lib$set_symbol(
    const struct dsc$descriptor_s *symbol,
    const struct dsc$descriptor_s *value,
    const uint32_t *table_type)
{
    if (!symbol || !symbol->dsc$a_pointer || !value)
        return SS$_BADPARAM;

    char name[SYM_MAX_NAME];
    uint32_t st = symbol_name(symbol, name);
    if (!(st & 1))
        return st;
    if (value->dsc$w_length >= SYM_MAX_VALUE)
        return LIB$_INSCLIMEM;

    uint32_t ttype = table_type ? *table_type : LIB$K_CLI_LOCAL_SYM;
    uint16_t vlen = value->dsc$a_pointer ? value->dsc$w_length : 0;

    struct ovmx_cli_req req;
    struct ovmx_cli_rsp rsp;
    memset(&req, 0, sizeof req);
    req.op = OVMX_CLI_OP_SET;
    req.table = ttype;
    req.name_len = (uint16_t)strlen(name);
    memcpy(req.name, name, req.name_len);
    req.value_len = vlen;
    if (vlen)
        memcpy(req.value, value->dsc$a_pointer, vlen);
    if (cli_call(&req, &rsp))
        return rsp.status;
    return local_set(name, value->dsc$a_pointer, vlen, ttype);
}

/*
 * lib$delete_symbol - Delete a CLI symbol (default: the local table).
 */
uint32_t lib$delete_symbol(
    const struct dsc$descriptor_s *symbol,
    const uint32_t *table_type)
{
    if (!symbol || !symbol->dsc$a_pointer)
        return SS$_BADPARAM;

    char name[SYM_MAX_NAME];
    uint32_t st = symbol_name(symbol, name);
    if (!(st & 1))
        return st;
    uint32_t ttype = table_type ? *table_type : LIB$K_CLI_LOCAL_SYM;

    struct ovmx_cli_req req;
    struct ovmx_cli_rsp rsp;
    memset(&req, 0, sizeof req);
    req.op = OVMX_CLI_OP_DELETE;
    req.table = ttype;
    req.name_len = (uint16_t)strlen(name);
    memcpy(req.name, name, req.name_len);
    if (cli_call(&req, &rsp))
        return rsp.status;
    return local_delete(name, ttype);
}

/*
 * lib$get_symbol - Read the value of a CLI symbol: the local table first,
 * then the global one, as DCL looks a symbol up.
 *
 * @param value       Receives the value (a CLASS_S descriptor is filled up to
 *                    its length and its length set; a CLASS_D one is sized).
 * @param value_len   Optional: the untruncated value length.
 * @param table_type  Optional OUTPUT: the table the symbol was found in
 *                    (LIB$K_CLI_LOCAL_SYM or LIB$K_CLI_GLOBAL_SYM).
 */
uint32_t lib$get_symbol(
    const struct dsc$descriptor_s *symbol,
    struct dsc$descriptor_s *value,
    uint16_t *value_len,
    uint32_t *table_type)
{
    if (!symbol || !symbol->dsc$a_pointer || !value)
        return SS$_BADPARAM;

    char name[SYM_MAX_NAME];
    uint32_t st = symbol_name(symbol, name);
    if (!(st & 1))
        return st;

    struct ovmx_cli_req req;
    struct ovmx_cli_rsp rsp;
    memset(&req, 0, sizeof req);
    req.op = OVMX_CLI_OP_GET;
    req.name_len = (uint16_t)strlen(name);
    memcpy(req.name, name, req.name_len);
    if (!cli_call(&req, &rsp))
        local_get(name, &rsp);
    if (!(rsp.status & 1))
        return rsp.status;

    uint16_t full_len = rsp.value_len;
    if (value->dsc$b_class == DSC$K_CLASS_D) {
        struct dsc$descriptor_d *dyn = (struct dsc$descriptor_d *)value;
        vms_desc_free(dyn);
        if (vms_desc_alloc(dyn, full_len) != 0)
            return SS$_INSFMEM;
        if (full_len > 0)
            memcpy(dyn->dsc$a_pointer, rsp.value, full_len);
    } else {
        uint16_t copy_len = full_len;
        if (copy_len > value->dsc$w_length)
            copy_len = value->dsc$w_length;
        if (copy_len > 0 && value->dsc$a_pointer)
            memcpy(value->dsc$a_pointer, rsp.value, copy_len);
        value->dsc$w_length = copy_len;
    }

    if (value_len)
        *value_len = full_len;
    if (table_type)
        *table_type = rsp.table;
    return SS$_NORMAL;
}
