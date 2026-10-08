/*
 * vms/vms_abi.h - common scaffolding for the VMS-layout STARLET headers in
 * this directory (vms-022).
 *
 * The headers under vms/ present OpenVMS data structures in their VMS byte
 * layout: every "l_" address field is a 32-bit (P0/P1) address whatever the
 * client's default pointer size, as in the DEC C STARLET headers. Every field
 * offset and structure size is asserted against the observed OpenVMS Alpha
 * V8.4 definitions (docs/oracle/alpha84-starlet-defs/*DEF.txt).
 *
 * The services they declare are the VMS-ABI entry points by their upper-case
 * names (SYS$PARSE, SYS$ASSIGN, ...), which accept these layouts. OVMX's own
 * code uses the lower-case sys$ API with its native structures
 * (src/vmsrms/include/rms, src/libvms/include) and is unaffected.
 */
#ifndef __VMS_ABI_H
#define __VMS_ABI_H

#ifdef __cplusplus
#define __VMS_ABI_ASSERT(c, m) static_assert(c, m)
#define __VMS_ABI_EXTERN_C_BEGIN extern "C" {
#define __VMS_ABI_EXTERN_C_END }
#else
#define __VMS_ABI_ASSERT(c, m) _Static_assert(c, m)
#define __VMS_ABI_EXTERN_C_BEGIN
#define __VMS_ABI_EXTERN_C_END
#endif

#define __VMS_ABI_OFFSET(type, field, off) \
    __VMS_ABI_ASSERT(__builtin_offsetof(type, field) == (off), #type "." #field " offset (VMS layout)")
#define __VMS_ABI_SIZE(type, size) \
    __VMS_ABI_ASSERT(sizeof(type) == (size), #type " size (VMS layout)")

#endif /* __VMS_ABI_H */
