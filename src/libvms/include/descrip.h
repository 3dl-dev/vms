/*
 * DESCRIP.H - VMS String Descriptor Definitions
 *
 * OpenVMX compatibility layer - VMS descriptor structures and macros.
 * Implements the VMS descriptor mechanism used for passing strings
 * and arrays between routines in the VMS calling standard.
 *
 * Reference: OpenVMS Programming Concepts Manual, Chapter 24
 *            OpenVMS Calling Standard, Chapter 7
 */

#ifndef __DESCRIP_H
#define __DESCRIP_H

#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Descriptor class constants
 *
 * DSC$K_CLASS_Z  - Unspecified/invalid class
 * DSC$K_CLASS_S  - Fixed-length (static) descriptor
 * DSC$K_CLASS_D  - Dynamic string descriptor
 * DSC$K_CLASS_A  - Array descriptor
 * DSC$K_CLASS_P  - Procedure descriptor
 * DSC$K_CLASS_PI - Procedure incarnation descriptor
 * DSC$K_CLASS_J  - Label descriptor
 * DSC$K_CLASS_JI - Label incarnation descriptor
 * DSC$K_CLASS_SD - Decimal string descriptor
 * DSC$K_CLASS_NCA - Non-contiguous array descriptor
 * DSC$K_CLASS_VS - Varying string descriptor
 * DSC$K_CLASS_VSA - Varying string array descriptor
 * DSC$K_CLASS_UBS - Unaligned bit string descriptor
 * DSC$K_CLASS_UBA - Unaligned bit array descriptor
 * DSC$K_CLASS_SB - String with bounds descriptor
 * DSC$K_CLASS_UBSB - Unaligned bit string with bounds descriptor
 */
#define DSC$K_CLASS_Z    0
#define DSC$K_CLASS_S    1
#define DSC$K_CLASS_D    2
#define DSC$K_CLASS_A    4
#define DSC$K_CLASS_P    5
#define DSC$K_CLASS_PI   6
#define DSC$K_CLASS_J    7
#define DSC$K_CLASS_JI   8
#define DSC$K_CLASS_SD   9
#define DSC$K_CLASS_NCA  10
#define DSC$K_CLASS_VS   11
#define DSC$K_CLASS_VSA  12
#define DSC$K_CLASS_UBS  13
#define DSC$K_CLASS_UBA  14
#define DSC$K_CLASS_SB   15
#define DSC$K_CLASS_UBSB 16

/*
 * Descriptor data type constants
 *
 * Atomic data types as defined by the VMS calling standard.
 */
#define DSC$K_DTYPE_Z    0   /* Unspecified */
#define DSC$K_DTYPE_BU   2   /* Byte unsigned */
#define DSC$K_DTYPE_WU   3   /* Word unsigned */
#define DSC$K_DTYPE_LU   4   /* Longword unsigned */
#define DSC$K_DTYPE_QU   5   /* Quadword unsigned */
#define DSC$K_DTYPE_B    6   /* Byte integer (signed, 8-bit) */
#define DSC$K_DTYPE_W    7   /* Word integer (signed, 16-bit) */
#define DSC$K_DTYPE_L    8   /* Longword integer (signed, 32-bit) */
#define DSC$K_DTYPE_Q    9   /* Quadword integer (signed, 64-bit) */
#define DSC$K_DTYPE_F    10  /* F-floating (single precision, VAX) */
#define DSC$K_DTYPE_D    11  /* D-floating (double precision, VAX) */
#define DSC$K_DTYPE_FC   12  /* F-floating complex */
#define DSC$K_DTYPE_DC   13  /* D-floating complex */
#define DSC$K_DTYPE_T    14  /* ASCII text string */
#define DSC$K_DTYPE_NU   15  /* Numeric string, unsigned */
#define DSC$K_DTYPE_NL   16  /* Numeric string, left separate sign */
#define DSC$K_DTYPE_NLO  17  /* Numeric string, left overpunched sign */
#define DSC$K_DTYPE_NR   18  /* Numeric string, right separate sign */
#define DSC$K_DTYPE_NRO  19  /* Numeric string, right overpunched sign */
#define DSC$K_DTYPE_NZ   20  /* Numeric string, zoned sign */
#define DSC$K_DTYPE_P    21  /* Packed decimal string */
#define DSC$K_DTYPE_V    22  /* Aligned bit string */
#define DSC$K_DTYPE_VU   23  /* Unaligned bit string */
#define DSC$K_DTYPE_OU   25  /* Octaword unsigned */
#define DSC$K_DTYPE_O    26  /* Octaword signed */
#define DSC$K_DTYPE_G    27  /* G-floating (double precision, VAX) */
#define DSC$K_DTYPE_H    28  /* H-floating (quad precision, VAX) */
#define DSC$K_DTYPE_GC   29  /* G-floating complex */
#define DSC$K_DTYPE_HC   30  /* H-floating complex */
#define DSC$K_DTYPE_ADT  35  /* Absolute date-time */
#define DSC$K_DTYPE_VT   37  /* Varying text */
#define DSC$K_DTYPE_FS   52  /* IEEE S-floating (single precision) */
#define DSC$K_DTYPE_FT   53  /* IEEE T-floating (double precision) */
#define DSC$K_DTYPE_FSC  54  /* IEEE S-floating complex */
#define DSC$K_DTYPE_FTC  55  /* IEEE T-floating complex */

/*
 * Fixed-length (static) string descriptor - CLASS_S
 *
 * This is the most commonly used descriptor type in VMS.
 * It describes a fixed-length string or scalar data item.
 * The descriptor itself does not own the storage; it merely
 * points to a buffer allocated elsewhere.
 */
struct dsc$descriptor_s {
    uint16_t  dsc$w_length;    /* Length of data in bytes */
    uint8_t   dsc$b_dtype;     /* Data type code */
    uint8_t   dsc$b_class;     /* Descriptor class code */
    char     *dsc$a_pointer;   /* Address of first byte of data */
};

/*
 * Dynamic string descriptor - CLASS_D
 *
 * Used for strings whose length can change at runtime.
 * The STR$ routines manage the allocated storage.  The fields
 * are identical to CLASS_S but the class code indicates that
 * the system may reallocate the buffer as needed.
 */
struct dsc$descriptor_d {
    uint16_t  dsc$w_length;    /* Current length of data in bytes */
    uint8_t   dsc$b_dtype;     /* Data type code */
    uint8_t   dsc$b_class;     /* Descriptor class code (DSC$K_CLASS_D) */
    char     *dsc$a_pointer;   /* Address of first byte of data */
};

/*
 * Array descriptor - CLASS_A
 *
 * Describes a contiguous array of elements. Extends the basic
 * descriptor with dimensionality and bounds information.
 */
struct dsc$descriptor_a {
    uint16_t  dsc$w_length;    /* Length of an array element in bytes */
    uint8_t   dsc$b_dtype;     /* Data type code of each element */
    uint8_t   dsc$b_class;     /* Descriptor class code (DSC$K_CLASS_A) */
    char     *dsc$a_pointer;   /* Address of first element */
    uint8_t   dsc$b_scale;     /* Signed power-of-two scale multiplier */
    uint8_t   dsc$b_digits;    /* Number of decimal digits in numeric string */
    uint8_t   dsc$b_aflags;    /* Array flags */
    uint8_t   dsc$b_dimct;     /* Number of dimensions */
    uint32_t  dsc$l_arsize;    /* Total size of array in bytes */
    /*
     * The following fields are present for each dimension.
     * For a 1-dimensional array, there is one set of (m, l, u).
     * For a 2-dimensional array, there are two sets, etc.
     * We provide space for up to 2 dimensions inline.
     */
    int32_t   dsc$l_m1;        /* Addressing coefficient for dim 1 (multiplier) */
    int32_t   dsc$l_m2;        /* Addressing coefficient for dim 2 (multiplier) */
    int32_t   dsc$l_l1;        /* Lower bound for dimension 1 */
    int32_t   dsc$l_u1;        /* Upper bound for dimension 1 */
    int32_t   dsc$l_l2;        /* Lower bound for dimension 2 */
    int32_t   dsc$l_u2;        /* Upper bound for dimension 2 */
};

/* Array flag bits for dsc$b_aflags */
#define DSC$M_FL_REDIM   0x01  /* Array can be redimensioned */
#define DSC$M_FL_COLUMN  0x02  /* Column-major order (Fortran) */
#define DSC$M_FL_COEFF   0x04  /* Multipliers are present */
#define DSC$M_FL_BOUNDS  0x08  /* Bounds are present */

/*
 * Generic descriptor - for function parameters where the
 * class is not known at compile time.
 */
struct dsc$descriptor {
    uint16_t  dsc$w_length;
    uint8_t   dsc$b_dtype;
    uint8_t   dsc$b_class;
    char     *dsc$a_pointer;
};

/*
 * Convenience typedefs
 */
typedef struct dsc$descriptor_s  dsc_descriptor_t;
typedef struct dsc$descriptor_d  dsc_dynamic_t;
typedef struct dsc$descriptor_a  dsc_array_t;

/*
 * $DESCRIPTOR macro
 *
 * Creates a static (CLASS_S) descriptor initialized to point to
 * a C string literal.  The length is computed at compile time
 * and does NOT include the null terminator (VMS convention).
 *
 * Usage:
 *     $DESCRIPTOR(my_name, "HELLO WORLD");
 *     lib$put_output(&my_name);
 */
#define $DESCRIPTOR(name, string) \
    struct dsc$descriptor_s name = { \
        (uint16_t)(sizeof(string) - 1), \
        DSC$K_DTYPE_T, \
        DSC$K_CLASS_S, \
        (char *)(string) \
    }

/*
 * $DESCRIPTOR64 - 64-bit aware descriptor macro (same as $DESCRIPTOR
 * on this platform since pointers are already native width)
 */
#define $DESCRIPTOR64(name, string) $DESCRIPTOR(name, string)

/*
 * $DESCRIPTOR_D - Initialize an empty dynamic descriptor
 */
#define $DESCRIPTOR_D(name) \
    struct dsc$descriptor_d name = { \
        0, DSC$K_DTYPE_T, DSC$K_CLASS_D, NULL \
    }

/*
 * Convenience macros to initialize descriptors at runtime
 */
#define DSC$INIT_S(desc, len, addr) \
    do { \
        (desc)->dsc$w_length  = (uint16_t)(len); \
        (desc)->dsc$b_dtype   = DSC$K_DTYPE_T; \
        (desc)->dsc$b_class   = DSC$K_CLASS_S; \
        (desc)->dsc$a_pointer = (char *)(addr); \
    } while (0)

#define DSC$INIT_D(desc) \
    do { \
        (desc)->dsc$w_length  = 0; \
        (desc)->dsc$b_dtype   = DSC$K_DTYPE_T; \
        (desc)->dsc$b_class   = DSC$K_CLASS_D; \
        (desc)->dsc$a_pointer = NULL; \
    } while (0)

/*
 * Helper functions for descriptor manipulation
 */

/**
 * vms_init_descriptor - Initialize a static string descriptor
 *
 * @param desc    Pointer to descriptor to initialize
 * @param string  Pointer to character data
 * @param length  Length of string data in bytes
 *
 * Initializes desc as a CLASS_S, DTYPE_T descriptor pointing
 * to the given string of the specified length.
 */
static inline void vms_init_descriptor(
    struct dsc$descriptor_s *desc,
    const char *string,
    uint16_t length)
{
    desc->dsc$w_length  = length;
    desc->dsc$b_dtype   = DSC$K_DTYPE_T;
    desc->dsc$b_class   = DSC$K_CLASS_S;
    desc->dsc$a_pointer = (char *)string;
}

/**
 * vms_desc_to_cstr - Convert a descriptor to a null-terminated C string
 *
 * @param desc    Pointer to source descriptor
 * @param buf     Destination buffer (must be at least desc->dsc$w_length + 1 bytes)
 * @param bufsiz  Size of destination buffer
 *
 * @return  Pointer to buf on success, NULL if buf is too small.
 *
 * Copies the descriptor data into buf and appends a null terminator.
 * VMS strings are not null-terminated, so this conversion is needed
 * when passing VMS strings to C library functions.
 */
static inline char *vms_desc_to_cstr(
    const struct dsc$descriptor_s *desc,
    char *buf,
    size_t bufsiz)
{
    uint16_t len;

    if (desc == NULL || buf == NULL)
        return NULL;

    len = desc->dsc$w_length;
    if ((size_t)(len + 1) > bufsiz)
        return NULL;

    if (desc->dsc$a_pointer != NULL && len > 0)
        memcpy(buf, desc->dsc$a_pointer, len);

    buf[len] = '\0';
    return buf;
}

/**
 * vms_cstr_to_desc - Initialize a descriptor from a C string
 *
 * @param desc    Pointer to descriptor to initialize
 * @param cstr    Null-terminated C string
 *
 * Sets up desc as a CLASS_S, DTYPE_T descriptor whose length is
 * strlen(cstr) and whose pointer points directly to cstr.
 * The caller must ensure cstr remains valid for the lifetime
 * of the descriptor.
 */
static inline void vms_cstr_to_desc(
    struct dsc$descriptor_s *desc,
    const char *cstr)
{
    if (desc == NULL)
        return;

    if (cstr != NULL) {
        desc->dsc$w_length  = (uint16_t)strlen(cstr);
        desc->dsc$a_pointer = (char *)cstr;
    } else {
        desc->dsc$w_length  = 0;
        desc->dsc$a_pointer = NULL;
    }
    desc->dsc$b_dtype = DSC$K_DTYPE_T;
    desc->dsc$b_class = DSC$K_CLASS_S;
}

/**
 * vms_desc_alloc - Allocate a dynamic descriptor with given initial size
 *
 * @param desc    Pointer to dynamic descriptor to initialize
 * @param length  Initial buffer size in bytes
 *
 * @return  0 on success, -1 on allocation failure
 */
static inline int vms_desc_alloc(
    struct dsc$descriptor_d *desc,
    uint16_t length)
{
    if (desc == NULL)
        return -1;

    desc->dsc$b_dtype = DSC$K_DTYPE_T;
    desc->dsc$b_class = DSC$K_CLASS_D;

    if (length > 0) {
        desc->dsc$a_pointer = (char *)malloc(length);
        if (desc->dsc$a_pointer == NULL) {
            desc->dsc$w_length = 0;
            return -1;
        }
        desc->dsc$w_length = length;
    } else {
        desc->dsc$a_pointer = NULL;
        desc->dsc$w_length  = 0;
    }
    return 0;
}

/**
 * vms_desc_free - Free storage held by a dynamic descriptor
 *
 * @param desc  Pointer to dynamic descriptor
 *
 * Releases the buffer and resets the descriptor to empty.
 * Safe to call on an already-free descriptor.
 */
static inline void vms_desc_free(struct dsc$descriptor_d *desc)
{
    if (desc == NULL)
        return;

    if (desc->dsc$a_pointer != NULL && desc->dsc$b_class == DSC$K_CLASS_D) {
        free(desc->dsc$a_pointer);
    }
    desc->dsc$a_pointer = NULL;
    desc->dsc$w_length  = 0;
}

/**
 * dsc$init - Create a descriptor from a C string (returns by value)
 *
 * @param str  Null-terminated C string
 * @return     Initialized static descriptor
 *
 * Convenience function for quick descriptor creation.
 * The caller must ensure str remains valid.
 */
static inline struct dsc$descriptor_s dsc$init(const char *str)
{
    struct dsc$descriptor_s d;
    d.dsc$w_length  = (uint16_t)strlen(str);
    d.dsc$b_dtype   = DSC$K_DTYPE_T;
    d.dsc$b_class   = DSC$K_CLASS_S;
    d.dsc$a_pointer = (char *)str;
    return d;
}

/**
 * dsc$strncpy - Extract C string from descriptor (legacy helper)
 *
 * @param dest    Destination buffer
 * @param src     Source descriptor
 * @param maxlen  Size of destination buffer
 */
static inline void dsc$strncpy(
    char *dest,
    const struct dsc$descriptor_s *src,
    size_t maxlen)
{
    size_t len = src->dsc$w_length;
    if (len >= maxlen)
        len = maxlen - 1;
    if (src->dsc$a_pointer != NULL && len > 0)
        memcpy(dest, src->dsc$a_pointer, len);
    dest[len] = '\0';
}

#ifdef __cplusplus
}
#endif

#endif /* __DESCRIP_H */
