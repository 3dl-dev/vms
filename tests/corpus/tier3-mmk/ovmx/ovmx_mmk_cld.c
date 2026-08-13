/*
 * ovmx_mmk_cld.c - OVMX (vms-ec70) compiled-CLD provider for MMK.
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) ***
 *
 * On OpenVMS, MMK's command table (MMK_CLD) is produced by SET COMMAND compiling
 * mmk_cld.cld into an object linked into the image; mmk.c references it as an
 * external symbol `mmk_cld`.  OVMX instead compiles the SAME mmk_cld.cld SOURCE
 * at run time with cli$compile_cld (the OVMX CLI$ callable interface, bead
 * vms-8c1), and hands the resulting table to cli$dcl_parse.  mmk.c's OVMX seam
 * routes MMK_CLD -> ovmx_mmk_get_cld().  The CLD grammar itself is unchanged
 * vendored MadGoat source.
 *
 * The CLD source text is embedded via mmk_cld_src.h, generated at build time
 * from tests/corpus/tier3-mmk/mmk_cld.cld by mk_mmk.sh (see that recipe).
 */
#include <stdint.h>
#include <stddef.h>
#include <descrip.h>
#include <ssdef.h>
#include <clitable.h>

#include "mmk_cld_src.h"   /* static const char mmk_cld_source[]; */

/* Returns the compiled MMK command table, compiling it once on first use.
 * On failure returns NULL — cli$dcl_parse then returns SS$_BADPARAM, which
 * mmk.c reports honestly (no silent success). */
struct cli_command_table *ovmx_mmk_get_cld(void)
{
    static struct cli_command_table *cached = NULL;
    if (cached) return cached;

    struct dsc$descriptor_s src;
    src.dsc$w_length  = (uint16_t)(sizeof(mmk_cld_source) - 1);
    src.dsc$b_dtype   = DSC$K_DTYPE_T;
    src.dsc$b_class   = DSC$K_CLASS_S;
    src.dsc$a_pointer = (char *)mmk_cld_source;

    struct cli_command_table *tbl = NULL;
    uint32_t st = cli$compile_cld(&src, &tbl);
    if ((st & 1) == 0) return NULL;
    cached = tbl;
    return cached;
}
