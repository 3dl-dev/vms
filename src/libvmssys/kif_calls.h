/*
 * kif_calls.h - executive SERVICES named by service, not by transport encoding
 * (rd vms-bbde, docs/design-executive-process-lifecycle.md section 5).
 *
 * Every userspace path to the executive goes through the transport seam
 * (kif_transport.h: kif_xport_dev_open / kif_xport_ioctl / kif_xport_dev_close).
 * vms_kif.c is the full policy layer; this header is the small, freestanding,
 * explicit-handle subset that the components which cannot use vms_kif.c's
 * per-thread binding (IMGACT.EXE, PID 1's boot bridge, the datalink library,
 * the BSD-sockets veneer) call instead of issuing ioctl(2) on /dev/vms
 * themselves. A caller names a SERVICE (KIF_SVC_*); only this header, inside
 * libvmssys, knows that today's transport encodes it as a VMS_IOCTL_* request.
 * When the transport becomes a system call, the mapping below and the
 * transport file change -- no caller does.
 *
 * tools/ci/check_transport_seam.py fails the build on a VMS_IOCTL_* token or a
 * "/dev/vms" literal anywhere in src/ outside src/libvmssys and the executive.
 */
#ifndef _KIF_CALLS_H
#define _KIF_CALLS_H

#include "kif_transport.h"
#include "../kernel/vms_ioctl.h"

enum kif_svc {
    KIF_SVC_REGISTER,
    KIF_SVC_ASSIGN,
    KIF_SVC_DASSGN,
    KIF_SVC_GETCLI,
    KIF_SVC_SETEXIT,
    KIF_SVC_GETEXIT,
    KIF_SVC_LNM_GETSCOPE,
    KIF_SVC_ACP_ASSIGN,
    KIF_SVC_ACP_ACCESS,
    KIF_SVC_ACP_DEACCESS,
    KIF_SVC_ACP_FILEOP,
    KIF_SVC_ACP_READVBLK,
    KIF_SVC_ACP_WRITEVBLK,
    KIF_SVC_L2_OPEN,
    KIF_SVC_L2_CLOSE,
    KIF_SVC_L2_SEND,
    KIF_SVC_L2_RECV,
    KIF_SVC_BGCONN_GETNAME,
    KIF_SVC_BGCONN_SOCKOPT
};

/* The transport's encoding of a service (today: the /dev/vms request word). */
static inline unsigned long kif_svc_request(enum kif_svc s)
{
    switch (s) {
    case KIF_SVC_REGISTER:       return VMS_IOCTL_REGISTER;
    case KIF_SVC_ASSIGN:         return VMS_IOCTL_ASSIGN;
    case KIF_SVC_DASSGN:         return VMS_IOCTL_DASSGN;
    case KIF_SVC_GETCLI:         return VMS_IOCTL_GETCLI;
    case KIF_SVC_SETEXIT:        return VMS_IOCTL_SETEXIT;
    case KIF_SVC_GETEXIT:        return VMS_IOCTL_GETEXIT;
    case KIF_SVC_LNM_GETSCOPE:   return VMS_IOCTL_LNM_GETSCOPE;
    case KIF_SVC_ACP_ASSIGN:     return VMS_IOCTL_ACP_ASSIGN;
    case KIF_SVC_ACP_ACCESS:     return VMS_IOCTL_ACP_ACCESS;
    case KIF_SVC_ACP_DEACCESS:   return VMS_IOCTL_ACP_DEACCESS;
    case KIF_SVC_ACP_FILEOP:     return VMS_IOCTL_ACP_FILEOP;
    case KIF_SVC_ACP_READVBLK:   return VMS_IOCTL_ACP_READVBLK;
    case KIF_SVC_ACP_WRITEVBLK:  return VMS_IOCTL_ACP_WRITEVBLK;
    case KIF_SVC_L2_OPEN:        return VMS_IOCTL_L2_OPEN;
    case KIF_SVC_L2_CLOSE:       return VMS_IOCTL_L2_CLOSE;
    case KIF_SVC_L2_SEND:        return VMS_IOCTL_L2_SEND;
    case KIF_SVC_L2_RECV:        return VMS_IOCTL_L2_RECV;
    case KIF_SVC_BGCONN_GETNAME: return VMS_IOCTL_BGCONN_GETNAME;
    case KIF_SVC_BGCONN_SOCKOPT: return VMS_IOCTL_BGCONN_SOCKOPT;
    }
    return 0;
}

/*
 * kif_call - issue one service on `h` (a handle from kif_xport_dev_open(), or an
 * executive object handle such as a BG connection). 0 on delivery, a negative
 * value on a transport failure; the service's own VMS status is in the
 * argument block, as for every executive service.
 */
static inline long kif_call(int h, enum kif_svc s, void *arg)
{
    unsigned long req = kif_svc_request(s);
    if (req == 0)
        return -1;
    return kif_xport_ioctl(h, req, arg);
}

#endif /* _KIF_CALLS_H */
