#ifndef OVMX_RMS_OPTARGS_H
#define OVMX_RMS_OPTARGS_H
/*
 * ovmx_rms_optargs.h - omitted err/suc arguments of the RMS services.
 *
 * A caller may write sys$open(&fab): on VMS the callee sees an argument count
 * of 1 and takes err/suc as absent.  The C ABI has no argument count, so pad
 * the omitted arguments with 0 (see ovmx_optargs.h).  Include AFTER the RMS
 * prototypes.  Off inside the OVMX build itself (OVMX_NO_PAD_MACROS) so the
 * definitions and internal callers see the plain prototypes.
 */
#ifndef OVMX_NO_PAD_MACROS
#include "ovmx_optargs.h"
#define sys$open(...) OVMX_PAD_3(sys$open, __VA_ARGS__)
#define sys$close(...) OVMX_PAD_3(sys$close, __VA_ARGS__)
#define sys$create(...) OVMX_PAD_3(sys$create, __VA_ARGS__)
#define sys$erase(...) OVMX_PAD_3(sys$erase, __VA_ARGS__)
#define sys$extend(...) OVMX_PAD_3(sys$extend, __VA_ARGS__)
#define sys$display(...) OVMX_PAD_3(sys$display, __VA_ARGS__)
#define sys$connect(...) OVMX_PAD_3(sys$connect, __VA_ARGS__)
#define sys$disconnect(...) OVMX_PAD_3(sys$disconnect, __VA_ARGS__)
#define sys$get(...) OVMX_PAD_3(sys$get, __VA_ARGS__)
#define sys$put(...) OVMX_PAD_3(sys$put, __VA_ARGS__)
#define sys$update(...) OVMX_PAD_3(sys$update, __VA_ARGS__)
#define sys$delete(...) OVMX_PAD_3(sys$delete, __VA_ARGS__)
#define sys$find(...) OVMX_PAD_3(sys$find, __VA_ARGS__)
#define sys$read(...) OVMX_PAD_3(sys$read, __VA_ARGS__)
#define sys$write(...) OVMX_PAD_3(sys$write, __VA_ARGS__)
#define sys$rewind(...) OVMX_PAD_3(sys$rewind, __VA_ARGS__)
#define sys$flush(...) OVMX_PAD_3(sys$flush, __VA_ARGS__)
#define sys$parse(...) OVMX_PAD_3(sys$parse, __VA_ARGS__)
#define sys$search(...) OVMX_PAD_3(sys$search, __VA_ARGS__)
#endif
#endif /* OVMX_RMS_OPTARGS_H */
