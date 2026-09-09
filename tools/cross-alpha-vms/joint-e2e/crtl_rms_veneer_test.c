/* crtl_rms_veneer_test.c (vms-f49, rung 4) — the CRTL/RMS port test with a
 * FULLY-QUALIFIED ODS-2 filespec, for the CRTL->RMS veneer gate.
 *
 * Identical to crtl_rms_test.c (same heap+RMS+stdio round-trip, same sentinel
 * ladder) except PT_NAME is a qualified spec: under the veneer, fopen routes to
 * sys$create/RMS, which resolves a device+directory. A bare name (crtl_rms_test.c
 * default) has no default device/dir in the RUN-context process, so RMS/Files-11
 * cannot resolve it (the resolution path returns no device -> a downstream NULL).
 * The host-arch proof (tests/qemu/test_syssvc_crtl_rms_veneer.c) likewise writes
 * a qualified VDA0:[OVMXDIR]VENEER.DAT; this is the faithful shape (the proof
 * asserts the veneer WRITE lands on the real ODS-2 volume, not default-directory
 * resolution). VDA0:[SYSTMP] is the volume's scratch directory (FID (64,1,0),
 * mastered by build-alpha-bootimage.sh). The veneer-proof SYSTARTUP's independent
 * DIRECTORY/FULL reader queries the same spec.
 */
#define PT_NAME "VDA0:[SYSTMP]PORTTEST.DAT"
#include "crtl_rms_test.c"
