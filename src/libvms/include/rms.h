/*
 * RMS.H - VMS Record Management Services Master Include
 *
 * OpenVMX compatibility layer - Traditional one-stop include for all
 * RMS programming on VMS.  On real VMS, programs include <rms.h> to
 * get the complete RMS API.
 *
 * The canonical declarations live in the vmsrms module
 * (src/vmsrms/include/rms/rms.h).  This header is a thin wrapper so
 * that code using the legacy #include <rms.h> path continues to work.
 */

#ifndef __RMS_H
#define __RMS_H
/* RMS declarations live in the vmsrms module */
#include <rms/rms.h>
#endif /* __RMS_H */
