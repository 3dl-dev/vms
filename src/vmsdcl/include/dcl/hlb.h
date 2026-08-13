/*
 * dcl/hlb.h - OVMX text/help/object library container ("LBRO") on-disk format.
 *
 * This is the byte layout LIBRARY/CREATE writes for TEXT (.TLB) and HELP (.HLB)
 * libraries: a fixed header, a flat module-name index, then the concatenated
 * module data. HELP (.HLB) libraries store one module per level-1 HELP key --
 * the top-level topic name -- so LIBRARY compiles a numbered-level .HLP source
 * into a key-indexed binary library, and HELP resolves a topic by locating its
 * module in the index (see src/vmsdcl/dcl_library.c and src/vmsdcl/dcl_help.c).
 *
 * Clean-room provenance (project Rule 8): the *unpublished* VMS LBR (.HLB/.TLB)
 * byte layout is NOT reproduced. This "LBRO" container is an OVMX design choice,
 * shared by LIBRARY (dcl_library.c), LIBRARIAN.EXE, and the HELP reader
 * (dcl_help.c); it is never presented as VMS-authentic. The LIBRARY command
 * surface, the HELP library-compile semantics (each level-1 key becomes a
 * module), and the HLP$LIBRARY search list ARE documented behavior (VSI OpenVMS
 * DCL Dictionary: LIBRARY, HELP; VSI OpenVMS Command Definition, Librarian, and
 * Message Utilities Manual: Librarian). The OBJECT (.OLB) library is a separate
 * `ar`-container format (dcl/../ovmx_olb.h), not this one.
 */

#ifndef OVMX_DCL_HLB_H
#define OVMX_DCL_HLB_H

#include <stdint.h>

/* Container magic: the ASCII bytes "LBRO" (Library, OVMX) in little-endian. */
#define LBR_MAGIC       0x4C42524Fu  /* 'L','B','R','O' */

/* Library types (struct lbr_header.type). */
#define LBR_TYPE_TEXT   1
#define LBR_TYPE_HELP   2
#define LBR_TYPE_OBJECT 3   /* NB: OBJECT uses the ovmx_olb.h ar-container, not this */

/* Limits. */
#define LBR_MAX_MODULES 1024
#define LBR_NAME_LEN    32

/* On-disk header (16 bytes), at file offset 0. */
struct lbr_header {
    uint32_t magic;         /* LBR_MAGIC */
    uint32_t type;          /* LBR_TYPE_* */
    uint32_t module_count;  /* number of struct lbr_module entries following */
    uint32_t reserved;
};

/* On-disk module index entry (40 bytes). module_count of these follow the
 * header; `offset` is the byte offset (from file start) of this module's data. */
struct lbr_module {
    char     name[LBR_NAME_LEN]; /* uppercased key, NUL/space padded */
    uint32_t offset;
    uint32_t length;
};

#endif /* OVMX_DCL_HLB_H */
