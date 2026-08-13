/*
 * ovmx_mmk_cms.c - OVMX (vms-ec70) honest CMS stubs for MMK.
 *
 * *** OVMX DESIGN CHOICE (clean-room, Rule 8) + DEFERRED-GAP FLAG ***
 *
 * MMK optionally integrates with DEC/VMS CMS (Code Management System) — the
 * vendored cms_interface.c uses the CMS$ callable library, which OVMX does not
 * provide.  CMS is off by default (the /CMS qualifier and the use_cms global
 * gate every call), and it is not part of the self-host spine.  These stubs
 * replace cms_interface.c and return an honest failure (CMS not available) so
 * MMK never silently pretends a CMS operation succeeded; if a description file
 * ever asks for CMS, MMK reports it cannot do it rather than faking it.
 */
#include <stdint.h>
#include <ssdef.h>

#ifndef SS$_UNSUPPORTED
#define SS$_UNSUPPORTED 0x00000924   /* even (failure) */
#endif

unsigned int cms_get_rdt(char *fspec, char *generation, void *rdt)
{
    (void)fspec; (void)generation; (void)rdt;
    return SS$_UNSUPPORTED;
}

unsigned int cms_fetch_file(char *fspec, char *outspec)
{
    (void)fspec; (void)outspec;
    return SS$_UNSUPPORTED;
}

unsigned int cms_parse_name(char *fspec, char *libname, int libsize, int *llen,
                            char *elename, int elesize, int *elen, int flags)
{
    (void)fspec; (void)libname; (void)libsize; (void)llen;
    (void)elename; (void)elesize; (void)elen; (void)flags;
    return SS$_UNSUPPORTED;
}
