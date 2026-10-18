/*
 * hostabi/fopen-same.h - the FOPEN_* mode strings a DEC C program built FOR the
 * OVMX/Alpha host gets from binutils' <fopen-same.h> (vms-5303e).
 *
 * binutils' bfd/bfdio.c, under `#ifdef VMS` (DEC C defines VMS), splits the
 * mode it is given at its commas into mode + RMS file attributes
 * ("w+b,rfm=udf,rat=none" -> fopen (name, "w+b", "rfm=udf", "rat=none")), and a
 * mode WITHOUT commas falls off the end of _bfd_real_fopen: the VMS branch has
 * no return of its own. The modes with the attributes are binutils' own
 * include/fopen-vms.h -- written for exactly this host and never selected by
 * bfd/sysdep.h, which includes fopen-same.h or fopen-bin.h. A VMS host takes
 * fopen-vms.h; this stub, found first by `-iquote` (a quoted include is searched
 * there before the -I directories), is how the build says so, unchanged source.
 * Its attributes are applied by the C RTL's fopen at $CREATE (vms-45f).
 */
#ifndef __HOSTABI_FOPEN_SAME_H
#define __HOSTABI_FOPEN_SAME_H
#include "fopen-vms.h"
#endif
