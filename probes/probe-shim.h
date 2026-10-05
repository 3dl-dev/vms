/* PROBE-ONLY shim (vms-3e4 inventory run) -- NOT a deliverable. */
#ifndef PROBE_SHIM_H
#define PROBE_SHIM_H
#ifdef __cplusplus
extern "C" {
#endif
int *get_vms_errno_addr(void);
#define vaxc$errno (*get_vms_errno_addr())
#define __PROBE_GETCWD2(b,s,...) getcwd(b,s)
#define getcwd(...) __PROBE_GETCWD2(__VA_ARGS__, 0)
#pragma __pointer_size __save
#pragma __pointer_size 32
typedef char *__char_ptr32;
typedef const char *__const_char_ptr32;
typedef __char_ptr32 *__char_ptr_char_ptr32;
#pragma __pointer_size __restore
#ifdef __cplusplus
}
#endif
#endif
