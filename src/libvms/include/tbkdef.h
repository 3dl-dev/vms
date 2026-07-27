/*
 * TBKDEF.H - VMS Traceback (TBK$) API Parameter Block
 *
 * OpenVMX compatibility layer - Defines the TBK_API_PARAM structure
 * and TBK$K_ constants used with tbk$alpha_symbolize/tbk$i64_symbolize
 * (and the x86_64 equivalent used by this port) to symbolize call-frame
 * program counters into image/module/routine names for stack tracebacks.
 *
 * PROVENANCE: the TBK$ symbolize API and its general parameter-block
 * shape are documented in the public OpenVMS Programming Concepts
 * Manual / RTL Traceback chapter. Exact field offsets and the
 * TBK$K_LENGTH/TBK$K_VERSION values could NOT be confirmed against a
 * fetchable public source this session — this struct defines exactly
 * the fields the current corpus program's x86_64 code path touches
 * (tbk$w_length, tbk$b_version, the three descriptor pointers, the
 * line-number and flags out-pointers, and tbk$q_faulting_pc), as an
 * OVMX clean-room representation (CLAUDE.md rule 8). Flagged in
 * vms-531 findings for operator sign-off.
 *
 * Reference: OpenVMS Programming Concepts Manual, Condition Handling
 *            and Traceback
 */

#ifndef __TBKDEF_H
#define __TBKDEF_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================
 * TBK$K_ — TBK_API_PARAM version/length constants
 * ================================================================ */

#define TBK$K_LENGTH     32   /* sizeof(TBK_API_PARAM) as expected by tbk$*_symbolize */
#define TBK$K_VERSION     1   /* Current TBK_API_PARAM version */

/* ================================================================
 * TBK_API_PARAM — traceback symbolize parameter block
 *
 * The descriptor-pointer fields are declared as "void *" rather than
 * "struct dsc$descriptor_s *" so this header has no dependency on
 * descrip.h include order; callers (per the corpus) explicitly cast
 * their "struct dsc$descriptor_s *" (aka "struct _descriptor *" in
 * older VMS sources) to match, which is a standard implicit pointer
 * conversion in C.
 * ================================================================ */

typedef struct _tbk_api_param {
    unsigned short int  tbk$w_length;              /* Block length — TBK$K_LENGTH */
    unsigned char        tbk$b_version;             /* Block version — TBK$K_VERSION */
    unsigned char        tbk$b_unused;               /* Reserved/alignment */
    void                *tbk$pq_image_desc;         /* Out: image name descriptor */
    void                *tbk$pq_module_desc;        /* Out: module name descriptor */
    void                *tbk$pq_routine_desc;       /* Out: routine name descriptor */
    unsigned int        *tbk$pq_listing_lineno;     /* Out: source line number */
    unsigned __int64     *tbk$pq_symbolize_flags;    /* In/out: symbolize control flags */
    unsigned __int64      tbk$q_faulting_pc;         /* In: PC/IP to symbolize */
    unsigned __int64      tbk$q_faulting_fp;         /* In: frame pointer (Alpha only) */
} TBK_API_PARAM;

/* Forward declaration so "(struct _descriptor *)" casts used by some
 * corpus programs remain valid even though this header does not
 * define the full struct (see comment above). */
struct _descriptor;

#ifdef __cplusplus
}
#endif

#endif /* __TBKDEF_H */
