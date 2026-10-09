/*
 * trmdef.h - $TRMDEF: item codes and modifiers of the terminal driver's
 * extended read ($QIO IO$_READVBLK!IO$M_EXTEND), rd vms-eb3d.
 *
 * Values are the VAX V7.3 node's own (docs/oracle/vax73-starlet-defs/
 * TRMDEF.txt, read through LIBRARY/MACRO/EXTRACT); clean-room, Rule 8.
 */
#ifndef TRMDEF_H
#define TRMDEF_H

#define TRM$_MODIFIERS   0
#define TRM$_EDITMODE    1
#define TRM$_TIMEOUT     2
#define TRM$_TERM        3
#define TRM$_PROMPT      4
#define TRM$_INISTRNG    5
#define TRM$_PICSTRNG    6
#define TRM$_FILLCHR     7
#define TRM$_INIOFFSET   8
#define TRM$_ALTECHSTR   9
#define TRM$_ESCTRMOVR   10
#define TRM$_LASTITM     11
#define TRM$_RECLINE     31

#define TRM$M_TM_NOECHO      64
#define TRM$M_TM_TIMED       128
#define TRM$M_TM_CVTLOW      256
#define TRM$M_TM_NOFILTR     512
#define TRM$M_TM_DSABLMBX    1024
#define TRM$M_TM_PURGE       2048
#define TRM$M_TM_TRMNOECHO   4096
#define TRM$M_TM_REFRESH     8192
#define TRM$M_TM_ESCAPE      16384
#define TRM$M_TM_NOEDIT      32768
#define TRM$M_TM_NORECALL    65536

/*
 * OVMX, labelled (Rule 8): the item list of an extended read. VMS's entry is
 * {word length, word code, longword address-or-value}; OVMX's $QIO runs in
 * 64-bit images whose addresses do not fit a longword, so an entry here is
 * {word length, word code, longword 0, quadword address-or-value}.
 */
struct ovmx_trm_item {
    unsigned short     len;
    unsigned short     code;
    unsigned int       mbz;
    unsigned long long val;
};

#endif /* TRMDEF_H */
