/*
 * crtl_features.h -- vms-db7: the DEC C RTL feature-switch API and reentrancy
 * level (src/vmsrms/crtl_features.c). The alpha DECC$SHR aliases
 * decc$feature_get_index, decc$feature_get_name, decc$feature_get_value,
 * decc$feature_set_value, decc$set_reentrancy and decc$get_reentrancy onto
 * these (mk_decc_shr.sh). Only features OVMX honours are in the table; any
 * other name is -1/EINVAL. The C RTL file layer (crtl_rms_fd.c) reads
 * DECC$FILE_SHARING when it opens a file.
 */
#ifndef __RMS_CRTL_FEATURES_H
#define __RMS_CRTL_FEATURES_H

#define OVMX_CRTL_FEAT_FILE_SHARING  1     /* DECC$FILE_SHARING -- see crtl_features.c */
#define OVMX_CRTL_FEAT_COUNT         1
#define OVMX_C_SINGLE_THREAD         0     /* reentrancy.h C$C_SINGLE_THREAD */
#define OVMX_C_MULTITHREAD           1     /* reentrancy.h C$C_MULTITHREAD   */

int ovmx_crtl_feature_get_index(const char *name);
const char *ovmx_crtl_feature_get_name(int index);
int ovmx_crtl_feature_get_value(int index, int mode);
int ovmx_crtl_feature_set_value(int index, int mode, int value);
int ovmx_crtl_feature_current(int index);
int ovmx_crtl_set_reentrancy(int level);
int ovmx_crtl_get_reentrancy(void);

#endif /* __RMS_CRTL_FEATURES_H */
