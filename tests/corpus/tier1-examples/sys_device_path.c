
/* Copyright 2003-2023 James F. Duff */
/* License and disclaimer: http://www.eight-cubed.com/disclaimer.html */

#define __NEW_STARLET 1
#include <stdio.h>
#include <stdlib.h>
#include <ssdef.h>
#include <descrip.h>
#include <dpsdef.h>
#include <dvidef.h>
#include <dcdef.h>
#include <dvsdef.h>
#include <iledef.h>
#include <gen64def.h>
#include <starlet.h>

#include "errchk.h"

#ifdef __VAX
#  error "Alpha or IA64 code only"
#endif /* __VAX */


/******************************************************************************/
int main (void) {

static GENERIC_64 ds_context = { 0 };

static unsigned int dps_context;

static int r0_status;
static int path_count;
static int dviitm = DVI$_AVAILABLE_PATH_COUNT;
static int disk_class = DC$_DISK;
static int mpdev_count = 0;

static char path[256];
static char device[64];

static struct dsc$descriptor_s path_d = { 0,
                                          DSC$K_DTYPE_T,
                                          DSC$K_CLASS_S,
                                          path };
static struct dsc$descriptor_s device_d = { 0,
                                            DSC$K_DTYPE_T,
                                            DSC$K_CLASS_S,
                                            device };
static ILE3 dsitms[] = { 4, DVS$_DEVCLASS, &disk_class, NULL,
                         0, 0, NULL, NULL };
static ILE3 dpsitms[] = { 255, DPS$_MP_PATHNAME, path, &path_d.dsc$w_length,
                          0, 0, NULL, NULL };

static $DESCRIPTOR (wild_d, "*");

static char finished = FALSE;
static char paths_found;

    while (!finished) {
        device_d.dsc$w_length = sizeof (device);
        r0_status = sys$device_scan (&device_d,
                                     &device_d.dsc$w_length,
                                     &wild_d,
                                     dsitms,
                                     &ds_context);
        if (r0_status == SS$_NOMOREDEV) {
            finished = TRUE;
            continue;
        } else {
            errchk_sig (r0_status);
        }

        r0_status = lib$getdvi (&dviitm,
                                0,
                                &device_d,
                                &path_count,
                                0,
                                0);
        errchk_sig (r0_status);

        if (path_count > 1) {
            mpdev_count++;
            (void)printf ("Paths for device %-.*s\n",
                          device_d.dsc$w_length,
                          device_d.dsc$a_pointer);
            paths_found = FALSE;
            dps_context = 0;
            while (!paths_found) {
                r0_status = sys$device_path_scan (0,
                                                  &device_d,
                                                  dpsitms,
                                                  &dps_context,
                                                  0);
                if (r0_status == SS$_NOMOREPATHS) {
                    paths_found = TRUE;
                    continue;
                } else {
                    errchk_sig (r0_status);
                }
                (void)printf ("    %-.*s\n",
                              path_d.dsc$w_length,
                              path_d.dsc$a_pointer);
            }
        }
    }
    (void)printf ("Total of %d multipath disks found\n", mpdev_count);
}
