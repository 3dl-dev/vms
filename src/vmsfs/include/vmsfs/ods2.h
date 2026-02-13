#ifndef __VMSFS_ODS2_H
#define __VMSFS_ODS2_H

#include <stdint.h>

/* ODS-2 on-disk structures - for future FUSE driver */
/* These match the actual ODS-2 format from VMS */

#pragma pack(push, 1)

typedef struct ods2_home_block {
    uint32_t hm2$l_homelbn;
    uint32_t hm2$l_alhomelbn;
    uint32_t hm2$l_altidxlbn;
    uint16_t hm2$w_struclev;    /* Structure level (2) */
    uint16_t hm2$w_cluster;
    uint16_t hm2$w_homevbn;
    uint16_t hm2$w_alhomevbn;
    uint16_t hm2$w_altidxvbn;
    uint16_t hm2$w_ibmapvbn;
    uint32_t hm2$l_ibmaplbn;
    uint32_t hm2$l_maxfiles;
    uint16_t hm2$w_ibmapsize;
    uint16_t hm2$w_resfiles;
    uint16_t hm2$w_devtype;
    uint16_t hm2$w_rvn;
    uint16_t hm2$w_setcount;
    uint16_t hm2$w_volchar;
    uint32_t hm2$l_volowner;
    uint32_t hm2$l_reserved1;
    uint16_t hm2$w_protect;
    uint16_t hm2$w_fileprot;
    uint16_t hm2$w_reserved2;
    uint16_t hm2$w_checksum1;
    /* ... more fields */
    char     hm2$t_volname[12];
    char     hm2$t_format[12];
} ods2_home_block_t;

#pragma pack(pop)

#endif /* __VMSFS_ODS2_H */
