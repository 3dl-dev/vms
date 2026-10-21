/*
 * TBKDEF.H - VMS Traceback (TBK$) API parameter block.
 *
 * Layout and constants are the Alpha V8.4 $TBKDEF (docs/oracle/
 * alpha84-starlet-defs/TBKDEF.txt, LIBRARY/EXTRACT of the node's own
 * STARLET): a 152-byte block, TBK$K_LENGTH 152, TBK$K_VERSION 1, every
 * PQ_ field a quadword pointer at the offset noted below. (The former header
 * guessed a 32-byte block with five fields; the oracle has twenty.)
 * OVMX carries the block; it does not implement the TBK$ symbolize routines.
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

#define TBK$K_LENGTH     152  /* $TBKDEF tbk$k_length / tbk$c_length */
#define TBK$C_LENGTH     152
#define TBK$K_VERSION      1

#define TBK$M_EXCEPTION_IS_FAULT 0x1ULL
#define TBK$M_ENTRY_TYPE         0x100000000ULL

/* TBK_API_PARAM -- offsets from $TBKDEF in the comments. Pointer fields are
 * "void *" so the header does not depend on descrip.h; on LP64 each is the
 * quadword the PQ_ prefix names. */
typedef struct _tbk_api_param {
    uint16_t  tbk$w_length;               /*   0 */
    uint8_t   tbk$b_type;                 /*   2 */
    uint8_t   tbk$b_version;              /*   3 */
    uint32_t  tbk$l_reserveda;            /*   4 */
    uint64_t  tbk$q_faulting_pc;          /*   8 */
    uint64_t  tbk$q_faulting_fp;          /*  16 */
    void     *tbk$pq_filename_desc;       /*  24 */
    void     *tbk$pq_library_module_desc; /*  32 */
    void     *tbk$pq_record_number;       /*  40 */
    void     *tbk$pq_image_desc;          /*  48 */
    void     *tbk$pq_module_desc;         /*  56 */
    void     *tbk$pq_routine_desc;        /*  64 */
    void     *tbk$pq_listing_lineno;      /*  72 */
    void     *tbk$pq_rel_pc;              /*  80 */
    void     *tbk$pq_image_base_addr;     /*  88 */
    void     *tbk$pq_module_base_addr;    /*  96 */
    void     *tbk$pq_malloc_rtn;          /* 104 */
    void     *tbk$pq_free_rtn;            /* 112 */
    void     *tbk$pq_symbolize_flags;     /* 120 */
    uint64_t  tbk$q_reserved0;            /* 128 */
    uint64_t  tbk$q_reserved1;            /* 136 */
    uint64_t  tbk$q_reserved2;            /* 144 */
} TBK_API_PARAM;

#if defined(__LP64__) || defined(_LP64)
_Static_assert(sizeof(TBK_API_PARAM) == TBK$K_LENGTH, "TBK_API_PARAM is not $TBKDEF's 152 bytes");
#endif

/* Forward declaration so "(struct _descriptor *)" casts used by some
 * corpus programs remain valid even though this header does not
 * define the full struct (see comment above). */
struct _descriptor;

#ifdef __cplusplus
}
#endif

#endif /* __TBKDEF_H */
