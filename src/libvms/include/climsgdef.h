/*
 * climsgdef.h - CLI$_ condition values (the $CLIMSGDEF names) OVMX returns.
 *
 * Values as OpenVMS Alpha V8.4 $GETMSG reports them for facility CLI (3)
 * (docs/oracle/messages/alpha84-msgcat2.txt, observed on the node): e.g.
 * "MC 00038240 %CLI-W-IVQUAL, unrecognized qualifier - check validity,
 * spelling, and placement". DCL displays these with its own prefix
 * (%DCL-W-IVQUAL, docs/oracle/keystroke/PG.SHOW/vax73.ks.txt); the condition
 * value -- what $STATUS holds -- is the CLI one below.
 */
#ifndef __CLIMSGDEF_H
#define __CLIMSGDEF_H

#define CLI$_IVKEYW   0x00038060   /* %CLI-W-IVKEYW, unrecognized keyword */
#define CLI$_IVVERB   0x00038090   /* %CLI-W-IVVERB, unrecognized command verb */
#define CLI$_IVQUAL   0x00038240   /* %CLI-W-IVQUAL, unrecognized qualifier */

#endif /* __CLIMSGDEF_H */
