/*
 * vms/descrip.h - VMS argument descriptors in the VMS layout (vms-022).
 *
 * A 32-bit descriptor is 8 bytes: length word, data-type byte, class byte, a
 * 32-bit address -- whatever the client's pointer size (DSCDEF). Values from
 * docs/oracle/alpha84-starlet-defs/DSCDEF.txt.
 */
#ifndef __VMS_DESCRIP_H
#define __VMS_DESCRIP_H
#include "vms_abi.h"

#define DSC$K_DTYPE_Z   0
#define DSC$K_DTYPE_BU  2
#define DSC$K_DTYPE_WU  3
#define DSC$K_DTYPE_LU  4
#define DSC$K_DTYPE_T   14
#define DSC$K_DTYPE_VT  37
#define DSC$K_CLASS_Z   0
#define DSC$K_CLASS_S   1
#define DSC$K_CLASS_D   2
#define DSC$K_CLASS_A   4
#define DSC$K_CLASS_VS  11
#define DSC$K_Z_BLN     8
#define DSC$K_S_BLN     8

#pragma __required_pointer_size __save
#pragma __required_pointer_size __short

struct dsc$descriptor {
    unsigned short dsc$w_length;
    unsigned char  dsc$b_dtype;
    unsigned char  dsc$b_class;
    char          *dsc$a_pointer;
};
struct dsc$descriptor_s {
    unsigned short dsc$w_length;
    unsigned char  dsc$b_dtype;
    unsigned char  dsc$b_class;
    char          *dsc$a_pointer;
};
struct dsc$descriptor_d {
    unsigned short dsc$w_length;
    unsigned char  dsc$b_dtype;
    unsigned char  dsc$b_class;
    char          *dsc$a_pointer;
};
struct dsc$descriptor_vs {
    unsigned short dsc$w_maxstrlen;
    unsigned char  dsc$b_dtype;
    unsigned char  dsc$b_class;
    char          *dsc$a_pointer;
};

#pragma __required_pointer_size __restore

__VMS_ABI_SIZE(struct dsc$descriptor_s, 8);
__VMS_ABI_OFFSET(struct dsc$descriptor_s, dsc$b_dtype, 2);
__VMS_ABI_OFFSET(struct dsc$descriptor_s, dsc$b_class, 3);
__VMS_ABI_OFFSET(struct dsc$descriptor_s, dsc$a_pointer, 4);
__VMS_ABI_SIZE(struct dsc$descriptor_vs, 8);

#define $DESCRIPTOR(name, string) \
    struct dsc$descriptor_s name = { sizeof(string) - 1, DSC$K_DTYPE_T, DSC$K_CLASS_S, (char *)(string) }

#endif /* __VMS_DESCRIP_H */
