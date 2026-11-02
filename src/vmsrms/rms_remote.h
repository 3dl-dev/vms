/*
 * rms_remote.h - RMS remote file access over DECnet (rd vms-0a7 slice S1).
 *
 * On OpenVMS a file specification that names a node -- node"user pw"::file --
 * is opened by RMS itself: $PARSE, $SEARCH, $OPEN, $CREATE, $GET, $PUT and
 * $CLOSE assign the _NET: device, connect to the remote FAL (object 17) with
 * the access control in the NCB, and speak DAP over $QIO. No utility carries
 * its own DECnet code: DIRECTORY, TYPE and COPY of a remote file are their
 * ordinary RMS paths. This module is that half of RMS. The RMS entry points
 * hand it a control block when the block names a node (or already holds a
 * remote access); everything else is RMS's local path, untouched.
 *
 * The password stays in this process: it travels from the filespec into the
 * NCB access-control string of IO$_ACCESS and is wiped after. Expanded and
 * resultant strings show it as the word "password", as VMS does when
 * NAM$M_PWD is clear.
 *
 * PEER INPUT: every DAP message comes from the remote FAL. The receive path
 * decodes with the bounded DAP codec (src/vmsdecnet/dap), copies with
 * explicit bounds, and caps a listing at RMS_REMOTE_MAXFILES names.
 * tests/vmsrms/test_rms_remote.c replays real VAX FAL captures through it and
 * fuzzes it under ASan/UBSan.
 */
#ifndef RMS_REMOTE_H
#define RMS_REMOTE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct rms_fileattr;

/* 1 when `s` (n bytes) names a node: a "::" outside a quoted string. */
int rms_remote_is_spec(const char *s, size_t n);

/* Does this control block belong to the remote path? A FAB whose file name
 * names a node, or one with a remote access in progress; a RAB whose FAB is
 * remote. */
int rms_remote_owns_fab(const void *fab);
int rms_remote_owns_rab(const void *rab);

uint32_t rms_remote_parse(void *fab);
uint32_t rms_remote_search(void *fab);
uint32_t rms_remote_open(void *fab);
uint32_t rms_remote_create(void *fab);
uint32_t rms_remote_close(void *fab);
uint32_t rms_remote_connect(void *rab);
uint32_t rms_remote_disconnect(void *rab);
uint32_t rms_remote_get(void *rab);
uint32_t rms_remote_put(void *rab);
void     rms_remote_search_end(void *nam);

/* rms_file_attr of a node:: spec: the remote file's ATTRIBUTES (record
 * format, attributes, size limit, organization) from a DAP OPEN. */
uint32_t rms_remote_file_attr(const char *spec, struct rms_fileattr *out);

/* RMS$_FND "ACP file or directory lookup failed" (catalog 0x0001C02A; not in
 * rmsdef.h): a remote access whose logical link did not open. The connect's
 * own failure is the STV. */
#define RMS_REMOTE_FND 0x0001C02Au

/* Most file names one remote $SEARCH keeps (a bound on what a remote can
 * make this process hold). */
#define RMS_REMOTE_MAXFILES 65536

/*
 * The logical link. On the runtime it is the _NET: device: $ASSIGN, $QIOW
 * IO$_ACCESS with the NCB, IO$_WRITEVBLK / IO$_READVBLK, IO$_DEACCESS,
 * $DASSGN. Host tests replace it with a replay of captured FAL segments
 * (test_rms_remote.c); NULL restores the _NET: device.
 */
struct rms_remote_link_ops {
    /* Connect: *st gets how the connect ended; returns the link or NULL. */
    void *(*open)(void *ctx, const char *ncb, size_t ncblen, uint32_t *st);
    int   (*send)(void *link, const uint8_t *seg, size_t len);       /* 0 / -1 */
    int   (*recv)(void *link, uint8_t *buf, size_t cap, size_t *len); /* 0 / -1 */
    void  (*close)(void *link);
    void  *ctx;
};
void rms_remote_set_link_ops(const struct rms_remote_link_ops *ops);

#ifdef __cplusplus
}
#endif

#endif /* RMS_REMOTE_H */
