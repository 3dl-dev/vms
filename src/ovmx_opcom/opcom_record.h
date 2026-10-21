/*
 * opcom_record.h - the text of OPCOM's records, oracle-exact (rd vms-798).
 *
 * Pure (snprintf only): the same functions build what OPCOM files in
 * SYS$MANAGER:OPERATOR.LOG and what it broadcasts to the enabled operator
 * terminals, and tests/ovmx_init/test_opcom_record.c pins them against the real
 * OpenVMS VAX V7.3 console in the demo
 * (tests/lab/captures/vms-c56-console-chatter-20261021/C.console.txt):
 *
 *   %%%%%%%%%%%  OPCOM  21-OCT-2026 15:07:10.69  %%%%%%%%%%%
 *   Operator _VAXC$OPA0: has been enabled, username SYSTEM
 *
 *   %%%%%%%%%%%  OPCOM  21-OCT-2026 15:07:10.71  %%%%%%%%%%%
 *   Operator status for operator _VAXC$OPA0:
 *   CENTRAL, PRINTER, TAPES, DISKS, DEVICES, CARDS, NETWORK, CLUSTER, SECURITY,
 *   LICENSE, OPER1, OPER2, OPER3, OPER4, OPER5, OPER6, OPER7, OPER8, OPER9, OPER10,
 *   OPER11, OPER12
 *
 *   %%%%%%%%%%%  OPCOM  21-OCT-2026 15:07:14.11  %%%%%%%%%%%
 *   Message from user JOB_CONTROL on VAXC
 *   %JBC-E-OPENERR, error opening SYS$COMMON:[SYSEXE]QMAN$MASTER.DAT;
 *
 * and, for a request that wants a reply (REQUEST/REPLY), "Request N, from user
 * U on NODE" (docs/design-opcom-executive-logging.md sec2/sec6).
 */
#ifndef OVMX_OPCOM_RECORD_H
#define OVMX_OPCOM_RECORD_H

#include <stdio.h>
#include <string.h>
#include <stddef.h>

#define OPCOM_HASHES "%%%%%%%%%%%"

static inline void opcom_banner(char *buf, size_t sz, const char *timestamp)
{
    snprintf(buf, sz, "%s  OPCOM  %s  %s", OPCOM_HASHES, timestamp, OPCOM_HASHES);
}

/* "Message from user U on NODE" or, for a numbered request,
 * "Request N, from user U on NODE". */
static inline void opcom_from_line(char *buf, size_t sz, int request,
                                   unsigned reqno, const char *user,
                                   const char *node)
{
    if (request)
        snprintf(buf, sz, "Request %u, from user %s on %s", reqno, user, node);
    else
        snprintf(buf, sz, "Message from user %s on %s", user, node);
}

static inline void opcom_enabled_line(char *buf, size_t sz, const char *node,
                                      const char *term, const char *user)
{
    snprintf(buf, sz, "Operator _%s$%s: has been enabled, username %s",
             node, term, user);
}

static inline void opcom_status_line(char *buf, size_t sz, const char *node,
                                     const char *term)
{
    snprintf(buf, sz, "Operator status for operator _%s$%s:", node, term);
}

/* Every class, as VMS lists an operator enabled for all of them. */
static const char *const opcom_all_classes[] = {
    "CENTRAL, PRINTER, TAPES, DISKS, DEVICES, CARDS, NETWORK, CLUSTER, SECURITY,",
    "LICENSE, OPER1, OPER2, OPER3, OPER4, OPER5, OPER6, OPER7, OPER8, OPER9, OPER10,",
    "OPER11, OPER12",
};
#define OPCOM_N_CLASS_LINES (sizeof(opcom_all_classes) / sizeof(opcom_all_classes[0]))

/* The message text of an OPC block, by length (never to a NUL), trailing
 * CR/LF/blanks trimmed. Returns the length copied into out. */
static inline size_t opcom_block_text(const char *blk, size_t blklen,
                                      size_t hdrlen, char *out, size_t outsz)
{
    size_t n;

    if (!out || outsz == 0)
        return 0;
    out[0] = '\0';
    if (!blk || blklen <= hdrlen)
        return 0;
    n = blklen - hdrlen;
    if (n > outsz - 1)
        n = outsz - 1;
    memcpy(out, blk + hdrlen, n);
    out[n] = '\0';
    while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r' ||
                     out[n - 1] == ' ' || out[n - 1] == '\0'))
        out[--n] = '\0';
    return n;
}

#endif /* OVMX_OPCOM_RECORD_H */
