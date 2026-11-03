/*
 * dcl/devmatch.h -- SHOW DEVICE's device-name prefix match (rd vms-582).
 *
 * `SHOW DEVICE D` lists every device whose NAME begins with "D". On VMS the
 * name a truncated argument is matched against is the device name proper --
 * the "DUA2" of "$2$DUA2:" or "VAX1$DUA2:" -- not the allocation-class or node
 * prefix in front of it: on a cluster `SHOW DEVICE D` lists the MSCP-served
 * $2$DUA2: and the node-prefixed VAX1$DKA0: alike (OpenVMS DCL Dictionary,
 * SHOW DEVICES: "If you truncate a device name ... the command displays
 * information about all devices whose names begin with the characters
 * entered"; lab VAX V7.3, tests/lab/captures/vms-5fa-shared-volume-20261102).
 * A prefix that itself carries a '$' names that prefix, so it is matched
 * against the whole name ("VAX1$D" lists VAX1's D devices only). A leading
 * '_' (the "this is a physical name" mark) is ignored on both sides.
 *
 * Header-only so the hermetic unit test (tests/dcl/test_show_device_prefix.c)
 * and every DCL build (host, VAX cross, Alpha) share the one definition
 * without a new translation unit in each build list.
 */
#ifndef DCL_DEVMATCH_H
#define DCL_DEVMATCH_H

#include <stddef.h>
#include <string.h>

static inline int dcl_devnam_prefix_match(const char *devnam,
                                          const char *prefix, size_t plen)
{
    if (!devnam || !prefix)
        return 0;
    while (*devnam == '_')
        devnam++;
    while (plen > 0 && *prefix == '_') {
        prefix++;
        plen--;
    }
    if (plen == 0)
        return 1;
    if (memchr(prefix, '$', plen) == NULL) {
        const char *last = strrchr(devnam, '$');
        if (last)
            devnam = last + 1;
    }
    return strncmp(devnam, prefix, plen) == 0;
}

#endif /* DCL_DEVMATCH_H */
