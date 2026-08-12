/*
 * ovmx_secparam.h - OVMX security-relevant parameters that are SYSGEN
 * parameters on real OpenVMS but have no parameter store on OVMX yet.
 *
 * This is NOT a VMS definitions header (unlike ssdef.h, prvdef.h, etc. in
 * this same directory) -- it is an OVMX stand-in, and its
 * contents are OVMX compile-time constants, not VMS structure/bit-mask
 * definitions transcribed from a manual. It exists so that a value pinned
 * to the oracle has exactly ONE place to be hand-maintained instead of
 * two: every consumer (src/libvms/syssvc/sys_security.c and its test,
 * tests/libvms/test_protection.c, as of vms-2b8 round 5) includes this
 * file rather than each carrying its own #define, so the two cannot drift
 * out of agreement the way they did in round 4.
 */

#ifndef __OVMX_SECPARAM_H
#define __OVMX_SECPARAM_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MAXSYSGROUP -- the SYSGEN parameter that decides which UIC groups get
 * the SYSTEM protection category.
 *
 * PINNED TO TWO INDEPENDENT SOURCES (vms-2b8 round 4; CLAUDE.md Rule 10 --
 * "pin it to the oracle or public documentation, or do not rely on the
 * value"). Round 3 had only one: a lab transcript introduced by the same
 * branch that depends on it, which is self-certification, not a pin.
 * Round 4 corroborated the existing lab transcript against PUBLIC OpenVMS
 * documentation, independent of both this branch and the lab:
 *
 *   1. Lab transcript, VAX2, OpenVMS VAX V7.3, 30-JUL-2026 (docs/oracle/
 *      vax73-privileges.md S7):
 *        $ MCR SYSGEN SHOW MAXSYSGROUP
 *        Parameter Name  Current  Default   Min.    Max.     Unit    Dynamic
 *        MAXSYSGROUP           8        8      1   32768  UIC Group    D
 *
 *   2. VSI OpenVMS Wiki, "UIC Protection" (https://wiki.vmssoftware.com/
 *      UIC_Protection), fetched 31-JUL-2026: "System refers to users with
 *      the UIC group of 0 through the value of MAXSYSGROUP (10 by default;
 *      bear in mind that numbers in a UIC are octal)" -- octal 10 is
 *      decimal 8, the same value the lab measured, from a source that
 *      cannot be circular with either the lab capture or this tree.
 *
 * Two sources, neither derived from the other, agreeing on the same value:
 * that is a pin, not a disclosure. It is a settable SYSGEN parameter on
 * VMS and a compile-time constant here because OVMX has no SYSGEN
 * parameter store for it yet; when it gets one this becomes a read of it,
 * and this header goes away.
 */
#define OVMX_MAXSYSGROUP 8

#ifdef __cplusplus
}
#endif

#endif /* __OVMX_SECPARAM_H */
