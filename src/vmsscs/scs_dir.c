/*
 * scs_dir.c - SCS$DIRECTORY connect + SCS$DIR_LOOKUP responder (vms-246).
 * See scs_dir.h for the full clean-room provenance and the
 * GROUNDED-vs-REPLAYED field breakdown.
 */
#include "scs_dir.h"

#include <string.h>

#include "scs_env.h" /* vms-ec7: THE shared SCS message envelope */

/* --- byte-exact SCA-content templates (payload byte 0 = abs frame 14) --- */

/*
 * [48:50] IN EVERY TEMPLATE BELOW IS THE SCA CREDIT FIELD, NOT A FLAG.
 *
 * CORRECTED (vms-66f, adversary-caught). Earlier revisions of these templates
 * commented [48:50] as "flag", and the lookup REQUEST template went further --
 * REFUTED-QUOTE-BEGIN
 *   "flag = 0 (a request; the response has 1)"
 * REFUTED-QUOTE-END
 * -- i.e. a request/response discriminator.
 * THE CAPTURE THOSE TEMPLATES ARE CUT FROM REFUTES THAT.
 * Census over all 12 lookup messages in formation-ci1-joinwindow.pcap, selected
 * by CONNECTION IDENTITY (the Con.ID pair of each observed SCS$DIRECTORY
 * connection) rather than by SCA length -- vms-c11 showed length-keyed censuses
 * in this epic silently dropped whole classes -- and split by the GROUNDED
 * [58:62] marker:
 *
 *     REQUEST  ([58:62]=0): [48:50] histogram {0: 2, 1: 4}   6 frames
 *     RESPONSE ([58:62]=1): [48:50] histogram {1: 6}         6 frames
 *
 * So 1 appears on four REQUESTs and on all six RESPONSEs: [48:50] neither
 * separates the two nor is constant within requests. What it is, is the credit
 * field docs/cluster-protocol-spec.md sec 4(d) pins at SCA [48:50] for the
 * 58/62/66/86/94/110/190 message classes -- the same grounding that reads
 * SCS$DIRECTORY = 3 and SCS$DIR_LOOKUP = 1 as extended Send Credits in the
 * 110-byte CONNECT_REQ class, which is exactly dir_connreq_tmpl's 3 below.
 *
 * WHY OVMX STILL SHIPS 0 IN dir_lookupreq_tmpl: that template is a byte-exact
 * replay of SCA 29 (raw pcap 37), the FIRST lookup on its connection, whose
 * credit is 0 on the wire. OVMX stamps no live credit on any frame -- the
 * standing reachability gap in scs_credit.h -- so every inquiry it sends
 * carries 0 whether it is the first on the connection or the fourth. That is a
 * KNOWN DEVIATION, recorded as spec sec 4h gap (f), and one of the unseparated
 * candidates for the unanswered-inquiry gap (sec 4h gap (e)).
 *
 * WHAT DRIVES 0 vs 1 IS NOT DECODED. The two zeros are exactly the first lookup
 * on each of the two directory connections (raw 37 and 1244), but that is a
 * correlation over n=2, not a rule. Recorded as spec sec 4h gap (f).
 *
 * Re-derive every figure above: tools/cluster/scs_dir_role_measure.py
 * (14 checks, 0 failures, 2026-08-05). The checked-in half that needs no
 * capture is the ctest scs_dir_figures.
 */

/* SCA#23 CONNECT-ECHO (op=1): VAX2->VAX1, remote=0x63050008 echoed, local=0.
 * formation-ci1-joinwindow.pcap. Substituted at build time: dst-logical
 * [2:8], src-logical [10:16], counters [18:20]/[20:22]/[26:28]/[30:32]/
 * [34:36], remote Con.ID [50:54]. Local [54:58] stays 0 for the echo. */
static const uint8_t dir_echo_tmpl[SCS_DIR_ECHO_SCA_LEN] = {
    /* [0:2]   */ 0x40, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,                               /* opcode 0x5b, format 0x13 */
    /* [18:20] */ 0x01, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x01, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x01, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x01, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x01, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x16, 0x00,                               /* inner length = 22 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x01, 0x00,                               /* op = 1 */
    /* [48:50] */ 0x00, 0x00,                               /* credit = 0 (sec 4d) */
    /* [50:54] */ 0x08, 0x00, 0x05, 0x63,                   /* remote Con.ID (SUBST) */
    /* [54:58] */ 0x00, 0x00, 0x00, 0x00,                   /* local Con.ID = 0 (not yet assigned) */
    /* [58:62] */ 0x00, 0x00, 0x01, 0x00,
    /* [62:66] */ 0x53, 0x43, 0x53, 0x24                    /* 'SCS$' (truncated name in this class) */
};

/* SCA#25 CONNECT-RESPONSE (op=2): remote=0x63050008, local=0x33590007 supplied.
 * Substituted: dst/src logical, counters, remote Con.ID [50:54], local Con.ID
 * [54:58]. name [62:78]='SCS$DIR_LOOKUP  ', result [78:94]='SCS$DIRECTORY   '
 * are replayed byte-exact. */
static const uint8_t dir_resp_tmpl[SCS_DIR_RESP_SCA_LEN] = {
    /* [0:2]   */ 0x6c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,
    /* [18:20] */ 0x01, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x02, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x01, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x02, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x01, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x42, 0x00,                               /* inner length = 66 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x02, 0x00,                               /* op = 2 */
    /* [48:50] */ 0x01, 0x00,                               /* credit = 1 (sec 4d) */
    /* [50:54] */ 0x08, 0x00, 0x05, 0x63,                   /* remote Con.ID (SUBST) */
    /* [54:58] */ 0x07, 0x00, 0x59, 0x33,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x00, 0x00,
    /* [62:78] */ 'S','C','S','$','D','I','R','_','L','O','O','K','U','P',' ',' ',
    /* [78:94] */ 'S','C','S','$','D','I','R','E','C','T','O','R','Y',' ',' ',' ',
    /* [94:110]*/ ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
};

/* SCA#31 LOOKUP-RESPONSE (94 bytes): MSCP$TAPE -> "NOT PRESENT HERE".
 * Substituted: dst/src logical, opcode [16], counters, op [46:48], remote
 * [50:54], local [54:58], name [62:78], result [78:94]. */
static const uint8_t dir_lookup_tmpl[SCS_DIR_LOOKUP_SCA_LEN] = {
    /* [0:2]   */ 0x5c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,                               /* opcode (SUBST) / format 0x13 */
    /* [18:20] */ 0x03, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x03, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x03, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x03, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x03, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x32, 0x00,                               /* inner length = 50 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x0a, 0x00,                               /* op = 0x0a (SUBST-echo) */
    /* [48:50] */ 0x01, 0x00,                               /* credit = 1 (sec 4d) */
    /* [50:54] */ 0x08, 0x00, 0x05, 0x63,                   /* remote Con.ID (SUBST) */
    /* [54:58] */ 0x07, 0x00, 0x59, 0x33,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x01, 0x00, 0x00, 0x00,                   /* response marker */
    /* [62:78] */ 'M','S','C','P','$','T','A','P','E',' ',' ',' ',' ',' ',' ',' ',
    /* [78:94] */ 'N','O','T',' ','P','R','E','S','E','N','T',' ','H','E','R','E'
};

/* SCA#21 CONNECT-REQUEST (msgtype [46:48]=0, 110 bytes): VAX1->VAX2, the
 * POLLER's outbound connect. remote=0 (target CDT does not exist yet), local
 * 0x63050008 offered. Names: [62:78]='SCS$DIRECTORY   ' (destination SYSAP),
 * [78:94]='SCS$DIR_LOOKUP  ' (source SYSAP), [94:110] blanks. Substituted at
 * build time: dst-logical [2:8], src-logical [10:16], counters
 * [18:20]/[20:22]/[26:28]/[30:32]/[34:36], local Con.ID [54:58]. remote [50:54]
 * is FORCED to 0, never substituted (see scs_dir.h). Raw pcap frame 29 of
 * formation-ci1-joinwindow.pcap, byte-exact. */
static const uint8_t dir_connreq_tmpl[SCS_DIR_CONNREQ_SCA_LEN] = {
    /* [0:2]   */ 0x6c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,                               /* opcode 0x5b, format 0x13 */
    /* [18:20] */ 0x00, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x01, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* node-incarnation (SUBST if nonzero) */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x00, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x01, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x00, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x42, 0x00,                               /* inner length = 66 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x00, 0x00,                               /* msgtype 0 = CONNECT_REQ (sec 4h(1a)) */
    /* [48:50] */ 0x03, 0x00,                               /* Send Credits SCS$DIRECTORY extends = 3 (GROUNDED, sec 4d) */
    /* [50:54] */ 0x00, 0x00, 0x00, 0x00,                   /* remote Con.ID = 0 (FORCED) */
    /* [54:58] */ 0x08, 0x00, 0x05, 0x63,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x01, 0x00,                   /* replayed (connect-class value) */
    /* [62:78] */ 'S','C','S','$','D','I','R','E','C','T','O','R','Y',' ',' ',' ',
    /* [78:94] */ 'S','C','S','$','D','I','R','_','L','O','O','K','U','P',' ',' ',
    /* [94:110]*/ ' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' ',' '
};

/* SCA#29 LOOKUP-REQUEST (94 bytes): VAX1->VAX2 asking about MSCP$TAPE.
 * marker [58:62]=0, result [78:94] all-zero. Substituted: dst/src logical,
 * opcode [16], counters, op [46:48], remote [50:54], local [54:58], name
 * [62:78]. Raw pcap frame 37, byte-exact. */
static const uint8_t dir_lookupreq_tmpl[SCS_DIR_LOOKUP_SCA_LEN] = {
    /* [0:2]   */ 0x5c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,                               /* opcode (SUBST) / format 0x13 */
    /* [18:20] */ 0x02, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x03, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* node-incarnation (SUBST if nonzero) */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x02, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x03, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x02, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x32, 0x00,                               /* inner length = 50 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x0a, 0x00,                               /* op = 0x0a (SUBST) */
    /* [48:50] */ 0x00, 0x00,                               /* credit = 0, as SCA 29 carries it; NOT a request/response discriminator -- sec 4h(2a) census */
    /* [50:54] */ 0x07, 0x00, 0x59, 0x33,                   /* remote Con.ID (SUBST) */
    /* [54:58] */ 0x08, 0x00, 0x05, 0x63,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x00, 0x00,                   /* REQUEST marker (GROUNDED, sec 4h) */
    /* [62:78] */ 'M','S','C','P','$','T','A','P','E',' ',' ',' ',' ',' ',' ',' ',
    /* [78:94] */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0          /* result: empty in a request */
};

/* The AFFIRMATIVE VMS$VAXcluster result descriptor, [78:94] of SCA#38.
 * Reproduced byte-exact as observed; internal semantics NOT grounded
 * (spec sec 4h RE gap (c)). */
static const uint8_t dir_affirmative_result[SCS_DIR_RESULT_LEN] = {
    0x01, 0x1b, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x06, 0x00
};

/* vms-578 INTEGRATION: worktree-760 carried its OWN dir_connreq_tmpl
 * here, derived from formation-clean-2node.pcap. It is DELETED as a duplicate,
 * and that is a MEASURED claim, not an assumption: comparing the two arrays
 * byte-for-byte, the only offsets that differ are 6, 14, 54, 56 and 57
 * -- every one of them a field the builder SUBSTITUTES at send time (the two
 * logical addresses and the Con.ID pair). At every byte the builder does NOT
 * touch, the two captures agree. The vms-66f template above is kept because the
 * byte-exact tests (test_scs_dir.c, SCA#21) assert against it.
 */

/* vms-578 INTEGRATION: worktree-760 carried its OWN dir_lookupreq_tmpl
 * here, derived from formation-clean-2node.pcap. It is DELETED as a duplicate,
 * and that is a MEASURED claim, not an assumption: comparing the two arrays
 * byte-for-byte, the only offsets that differ are 6, 14, 50, 52, 53, 54, 56 and 57
 * -- every one of them a field the builder SUBSTITUTES at send time (the two
 * logical addresses and the Con.ID pair). At every byte the builder does NOT
 * touch, the two captures agree. The vms-66f template above is kept because the
 * byte-exact tests (test_scs_dir.c, SCA#29) assert against it.
 */

/* vms-760: OVMX's directory op=3 CONNECT-CONFIRM (62-byte SCA). Byte-exact to
 * the clean joiner's confirm (formation-clean-2node.pcap SCA idx26) EXCEPT
 * [8:10] connect-flag = 0x0001 (golden, matching the other OVMX dir templates;
 * the clean ref carried 0x03e8, a config artifact). Substituted at build time:
 * dst/src logical, counters, remote Con.ID [50:54], local Con.ID [54:58].
 * op[46:48]=3 and marker[58:62]=0x00010000 are baked in. NO SYSAP names (the
 * frame ends at the marker). */
static const uint8_t dir_confirm_tmpl[SCS_DIR_CONFIRM_SCA_LEN] = {
    /* [0:2]   */ 0x3c, 0x00,
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,                               /* connect flag (golden 0x0001) */
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x5b, 0x13,
    /* [18:20] */ 0x02, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x02, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* incarnation (SUBST) */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x02, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x02, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x02, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x12, 0x00,                               /* inner length = 18 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x03, 0x00,                               /* op = 3 (connect-confirm) */
    /* [48:50] */ 0x00, 0x00,                               /* credit = 0 (sec 4d); vms-578 relabelled */
    /* [50:54] */ 0x08, 0x00, 0xdc, 0xe2,                   /* remote Con.ID (SUBST, member's) */
    /* [54:58] */ 0x07, 0x00, 0x00, 0x00,                   /* local Con.ID (SUBST) */
    /* [58:62] */ 0x00, 0x00, 0x01, 0x00                    /* marker = 0x00010000 */
};

/*
 * vms-e81: the op-5 CONFIRM5 template -- 58 SCA bytes, NOT 62.
 *
 * It is tempting to build this as "the op-3 confirm with the opcode changed",
 * and that would have been wrong in the way this project keeps getting caught.
 * A 336-frame census (every op 5 in the capture library, across 4 sender nodes
 * and 15 captures) says op 5 differs from op 3 in FOUR places, not one:
 *
 *   [0:2]   0x003c -> 0x0038   outer length word   (SCA 62 -> 58)
 *   [42:44] 0x0012 -> 0x000e   inner length word   (18 -> 14)
 *   [46:48] 0x0003 -> 0x0005   opcode
 *   [58:62] the trailing marker word is ABSENT -- the frame ENDS at 58
 *
 * Copying the op-3 template and patching only the opcode would have emitted 62
 * bytes declaring 60, i.e. a frame four bytes longer than it claims. That is the
 * exact shape of the malformed op-7 that stalled the whole join for three
 * sessions: DERIVE LENGTH WORDS FROM WHAT YOU EMIT, NEVER INHERIT THEM. A peer
 * drops the over-long frame as a runt and the next frame dies on the sequence
 * gap -- in silence, because there is no NAK anywhere in this protocol.
 *
 * [16] is 0x4b (SEQAPP), NOT a mirror of the op-4 being answered: 86 of the
 * observed pairs answer a 0x5b op-4 with a 0x4b op-5, and all three op-5s a real
 * VAX has ever sent AT OVMX are 0x4b.
 *
 * The BYTE SHAPE above is unaffected by vms-754 (2026-08-06): every offset,
 * length and constant this comment measured is still exactly what a real op-5
 * frame carries. What vms-754 corrects is the NAME "CONFIRM5" -- op 4/5 are
 * the shared-namespace REJECT_REQ/REJECT_RSP (scs_dir.h, scs_env.h), not an
 * MSCP accept/confirm; see dir_build_common()'s collision comment above.
 */
static const uint8_t dir_confirm5_tmpl[SCS_DIR_CONFIRM5_SCA_LEN] = {
    /* [0:2]   */ 0x38, 0x00,                               /* outer length = 56 (SCA 58) */
    /* [2:8]   */ 0xaa, 0x00, 0x04, 0x00, 0x01, 0x04,       /* dst logical (SUBST) */
    /* [8:10]  */ 0x01, 0x00,                               /* connect flag (336/336) */
    /* [10:16] */ 0xaa, 0x00, 0x04, 0x00, 0x02, 0x04,       /* src logical (SUBST) */
    /* [16:18] */ 0x4b, 0x13,                               /* SEQAPP -- never mirrors the op-4 */
    /* [18:20] */ 0x02, 0x00,                               /* recv_ack (SUBST) */
    /* [20:22] */ 0x02, 0x00,                               /* send_seq (SUBST) */
    /* [22:24] */ 0x01, 0x00,                               /* incarnation (SUBST) -- OUR OWN */
    /* [24:26] */ 0x12, 0x00,
    /* [26:28] */ 0x02, 0x00,                               /* recv_ack mirror (SUBST) */
    /* [28:30] */ 0x00, 0x00,
    /* [30:32] */ 0x02, 0x00,                               /* send_seq mirror (SUBST) */
    /* [32:34] */ 0x00, 0x00,
    /* [34:36] */ 0x02, 0x00,                               /* recv_ack 3rd (SUBST) */
    /* [36:38] */ 0x00, 0x00,
    /* [38:40] */ 0x01, 0x00,
    /* [40:42] */ 0x00, 0x02,
    /* [42:44] */ 0x0e, 0x00,                               /* inner length = 14 */
    /* [44:46] */ 0x04, 0x00,
    /* [46:48] */ 0x05, 0x00,                               /* op = 5 (MSCP connect-CONFIRM5) */
    /* [48:50] */ 0x00, 0x00,                               /* credit = 0 (sec 4d), 336/336; vms-578 relabelled */
    /* [50:54] */ 0x08, 0x00, 0xdc, 0xe2,                   /* remote Con.ID (SUBST, peer's) */
    /* [54:58] */ 0x07, 0x00, 0x00, 0x00                    /* local Con.ID (SUBST, ours) */
    /* NO marker word -- the frame ends here. */
};

static void put_le16(uint8_t *dst, uint16_t v)
{
    dst[0] = (uint8_t)(v & 0xff);
    dst[1] = (uint8_t)((v >> 8) & 0xff);
}

static uint16_t get_le16(const uint8_t *src)
{
    return (uint16_t)((uint16_t)src[0] | ((uint16_t)src[1] << 8));
}

static uint32_t get_le32(const uint8_t *src)
{
    return (uint32_t)src[0] | ((uint32_t)src[1] << 8) |
           ((uint32_t)src[2] << 16) | ((uint32_t)src[3] << 24);
}

/* Lay the Ethernet header + template, then substitute the shared envelope
 * (dst/src logical) and the SCS sequence counters. Con.ID/name/result are
 * substituted by the per-class callers afterward. */
static void dir_build_common(const uint8_t *dst_mac, const uint8_t *src_mac,
                             const uint8_t *src_logical,
                             const uint8_t *peer_logical, const uint8_t *tmpl,
                             size_t sca_len, uint16_t recv_ack, uint16_t send_seq,
                             uint16_t incarnation,
                             const struct scs_env_fields *env, uint8_t *out)
{
    /* Ethernet header (abs 0-13). */
    memcpy(out + 0, dst_mac, 6);
    memcpy(out + 6, src_mac, 6);
    out[12] = 0x60;
    out[13] = 0x07;

    /* SCA content (abs 14+) from the captured joiner template. */
    memcpy(out + 14, tmpl, sca_len);

    /* Envelope address substitutions (payload-relative + 14). */
    memcpy(out + 14 + 2, peer_logical, 6);  /* dst logical [2:8]  (abs 16) */
    memcpy(out + 14 + 10, src_logical, 6);  /* src-logical [10:16] (abs 24) = aa:00:04:00:<sysid>
                                             * cluster-LOGICAL addr, NOT raw HW MAC (vms-9f3) */

    /* Sequence counters (spec sec 4h(4)): recv_ack at [18:20]/[26:28]/[34:36],
     * send_seq at [20:22] mirrored at [30:32]. */
    put_le16(out + 14 + 18, recv_ack);
    put_le16(out + 14 + 20, send_seq);
    put_le16(out + 14 + 26, recv_ack);
    put_le16(out + 14 + 30, send_seq);
    put_le16(out + 14 + 34, recv_ack);

    /* [22:24] node-incarnation echo (§4i established-join extension of the §4h
     * directory exchange). GROUNDED-BY-OBSERVATION, not from a golden template:
     * on an established-cluster join the member (VAX1) stamps its CURRENT
     * node-incarnation N in [22:24] of its own 0x5b SCS$DIRECTORY connect-
     * request (observed N=3 on the vms-246 lab wire; the fresh-formation golden
     * carries N=1, which is what the baked-in template holds). This is the same
     * per-node incarnation the member advertises in its directed HELLO [78:80]
     * and that §4i.B grounds for the 0x41 START [22:24]. OVMX echoes the value
     * the member itself put on the wire -- never a self-invented constant. A 0
     * here leaves the template's fresh value (1), preserving byte-exact golden
     * reproduction for the fresh-formation path and the unit tests. */
    if (incarnation != 0) {
        put_le16(out + 14 + 22, incarnation);
    }

    /* vms-ec7: THE SCS MESSAGE ENVELOPE, from the one build path -- inner
     * length (derived), format word, MTYPE, credit, Con.ID pair. This replaces
     * the nine separate copies of `put_le32(out + 14 + 50/54, ...)` that used to
     * sit in the per-class builders below, and the two open-coded MTYPE stores.
     *
     * A COLLISION THIS UNIFICATION MADE VISIBLE -- RESOLVED, vms-754
     * (2026-08-06). The field at [46:48] is ONE field, and this module used to
     * call its values "directory operations" (SCS_DIR_OP_*) while scs_conn.c /
     * spec sec 4(h)(1a) call the same values connection-control MESSAGE TYPES.
     * They agreed on 0/1/2/3 (CONNECT_REQ / CONNECT_RSP / ACCEPT_REQ /
     * ACCEPT_RSP) and disagreed on 4 and 5:
     * REFUTED-QUOTE-BEGIN
     *   vms-760 grounded op 4 as an MSCP connect-ACCEPT and op 5 as its
     *   CONFIRM (336 op-5 frames, 4 senders, 15 captures), while sec 4(h)(1a)
     *   maps 4 to REJECT_REQ and 5 to REJECT_RSP -- the latter BY POSITION
     *   ONLY. Both readings cannot be right. Nothing here picks one.
     * REFUTED-QUOTE-END
     * tools/cluster/scs_t45_measure.py (`ctest -R scs_t45_figures`) settles
     * it: sec 4(h)(1a) is correct. Over the 47-capture lab-1 library, 733/733
     * MTYPE-4 dialogues are TERMINAL -- 0 are ever followed by application
     * traffic (MTYPE 10) on the same Con.ID pair -- against 388/394 for the
     * undisputed ACCEPT_REQ positive control, and the exact frame vms-760
     * cited as its own grounding (af2-firsttimer-established-20260728.pcap,
     * frame 2584, rel~143.758) is a real-VAX-to-real-VAX exchange with no
     * OVMX participant in that capture at all, one of nine identical
     * rejections of a retried connect immediately followed by a tenth attempt
     * that switches message type to ACCEPT_REQ/RSP and succeeds. See
     * scs_dir.h's SCS_DIR_OP_ACCEPT / SCS_DIR_OP_MSCP_CONFIRM entries for the
     * full grounding and the wire-behaviour follow-up this decode opens
     * (NOT fixed here -- out of scope for vms-754): scsd.c's server-first
     * MSCP accept path still builds and consumes these bytes believing they
     * mean ACCEPT/CONFIRM. The builders below keep emitting exactly the bytes
     * they emitted before -- vms-754 is a decode, not a wire change. */
    if (env != NULL) {
        (void)scs_env_build(out + 14, sca_len, env);
    }
}

int scs_dir_build_connect_echo(const struct scs_dir_params *p,
                               uint8_t out[SCS_DIR_ECHO_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* MTYPE 1 = CONNECT_RSP (Fig 2-14's second arrow). Con.ID: remote = peer's
     * handle (echoed), local still 0 -- a CONNECT_RSP has not yet bound one. */
    struct scs_env_fields env = { SCS_DIR_MSGTYPE_CONNECT_RSP,
                                  SCS_DIR_ENV_CREDIT_ECHO, p->remote_conid, 0 };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_echo_tmpl, SCS_DIR_ECHO_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, &env, out);
    return 0;
}

/* vms-2f3 sec 4M. The msgtype OVMX must put on a directory response, given the
 * msgtype the request arrived with.
 *
 * ⚠ NOT GROUNDED -- THIS IS AN OVMX DESIGN CHOICE (vms-2f3 sec 4M.12).
 * The first version of this comment claimed the 336-frame op-5 census at the
 * head of this file proved "a real VAX never mirrors". THAT CENSUS IS ABOUT THE
 * op-5 CONFIRM FRAME, NOT ABOUT LOOKUP RESPONSES, and generalising it was wrong.
 * A byte census of every real-VAX lookup pair in the capture library (5
 * captures, OVMX excluded as responder) says:
 *
 *     req 0x5b -> resp 0x5b : 10      req 0x4b -> resp 0x4b : 50
 *     req 0x5b -> resp 0x4b : 11      req 0x4b -> resp 0x5b :  1
 *
 * So a real VAX mirrors a 0x5b lookup about half the time, and NEITHER
 * "always mirror" (OVMX's old behaviour, 60/72) nor "always 0x4b" (this
 * function, 61/72) is the rule. Both are a coin-flip against the corpus.
 *
 * "the response tracks the RESULT (present -> 0x4b, absent -> 0x5b)" was tested
 * next and is ALSO REFUTED: 18 of 63 real-VAX responses land off-diagonal.
 * Reference frame 8994 is the clean kill -- VAX1 answers a 0x5b MSCP$TAPE
 * request "NOT PRESENT HERE" (the same result value as frames 1193/1239, which
 * carried 0x5b) and puts 0x4b on the wire.
 *
 * NOTHING KNOWN PREDICTS IT. Over 72 real-VAX matched pairs, result, SYSAP name,
 * responder MAC and request msgtype all tie at 11 classification errors, and a
 * brute-force scan of every header byte at abs 14-29 and 31-59 over 129,084
 * real-VAX 0x4b/0x5b frames found no byte that separates them.
 *
 * THE RULING (vms-2f3 sec 4M.12, under CLAUDE.md Rule 8): this is an unresolved
 * RE gap. OVMX emits a fixed 0x4b as a LABELLED OVMX DESIGN CHOICE -- 0x4b is
 * the dominant form on the wire (18,785 vs 92 in the reference capture), it is
 * the best single fixed choice against the corpus (61/72 against the mirror's
 * 60/72), it is behaviourally neutral in the lab, and it is kill-switched.
 * It is NOT VMS-authentic and must never be described as such.
 *
 * `mirror` restores the pre-fix behaviour for the OVMX_DIR_MIRROR_MSGTYPE
 * kill-switch, so the failing case stays reproducible (guardrail 21). */
uint8_t scs_dir_response_msgtype(uint8_t request_msgtype, int mirror)
{
    return mirror ? request_msgtype : (uint8_t)SCS_DIR_OPCODE_SEQAPP;
}

int scs_dir_build_connect_response(const struct scs_dir_params *p,
                                   uint8_t out[SCS_DIR_RESP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* MTYPE 2 = ACCEPT_REQ. Con.ID pair now bound: remote = peer's handle,
     * local = OVMX's own. */
    struct scs_env_fields env = { SCS_DIR_MSGTYPE_ACCEPT_REQ,
                                  SCS_DIR_ENV_CREDIT_RESP, p->remote_conid,
                                  p->local_conid };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_resp_tmpl, SCS_DIR_RESP_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, &env, out);
    return 0;
}

int scs_dir_build_connect_request(const struct scs_dir_params *p,
                                  uint8_t out[SCS_DIR_CONNREQ_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* MTYPE 0 = CONNECT_REQ. The destination handle is 0 and is now WRITTEN as
     * 0 rather than inherited from the template: a CONNECT_REQ cannot name a CDT
     * that does not exist yet (sec 4h(1a)), and that is a property of the
     * message type, not of whichever capture the template came from. */
    struct scs_env_fields env = { SCS_DIR_MSGTYPE_CONNECT_REQ,
                                  SCS_DIR_ENV_CREDIT_CONNREQ, 0, p->local_conid };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_connreq_tmpl, SCS_DIR_CONNREQ_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, &env, out);
    return 0;
}

/* The two 94-byte lookup classes differ only in which template they start from;
 * every substitution below is identical, which is the point -- an inquiry and
 * its answer are the same frame shape (spec sec 4h(2)). */
static int dir_build_lookup(const struct scs_dir_lookup_params *p,
                            const uint8_t *tmpl, uint16_t credit, uint8_t *out)
{
    /* p->op IS the SCS MTYPE, and on this class it is 10 -- the p. 4-13
     * APPLICATION MESSAGE (SCS_DIR_OP_LOOKUP == 0x0a == SCS_ENV_MTYPE_APP_MESSAGE).
     * A name lookup is SYSAP-to-SYSAP traffic on an open SCS$DIRECTORY
     * connection, which is exactly what that taxon means. It stays a parameter
     * because the builder ECHOES the request's value rather than asserting one. */
    struct scs_env_fields env = { p->op, credit, p->remote_conid,
                                  p->local_conid };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     tmpl, SCS_DIR_LOOKUP_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, &env, out);

    /* Opcode echoes the request (0x5b before the SCS$DIRECTORY connection is up,
     * 0x4b once it is -- see spec sec 4h / 4g phase-3). This is the PPD marker
     * byte at content [16], NOT the SCS message type. */
    out[14 + 16] = p->opcode;

    /* Queried SYSAP name echoed into [62:78], 16-byte blank-padded. */
    {
        uint8_t namebuf[SCS_DIR_NAME_LEN];
        memset(namebuf, ' ', sizeof(namebuf));
        /* Bounded copy up to the first NUL or the 16-byte field width. A plain
         * loop (not strnlen) keeps this free of any POSIX feature-macro
         * dependency, so it stays clean under the musl-static build. */
        size_t n = 0;
        while (n < SCS_DIR_NAME_LEN && p->name[n] != '\0') {
            n++;
        }
        memcpy(namebuf, p->name, n);
        memcpy(out + 14 + 62, namebuf, SCS_DIR_NAME_LEN);
    }

    /* Result field [78:94].
     *
     * vms-578 INTEGRATION: both sides changed this and BOTH changes are kept.
     * The outer guard is vms-66f's; the inner selection is vms-760's.
     *
     * OUTER (vms-66f): a REQUEST carries no answer, so the template's all-zero
     * field is left exactly as captured; only a RESPONSE writes here.
     *
     * INNER (vms-760) per-name selection:
     *   - negative (p->affirmative == 0): the GROUNDED literal "NOT PRESENT HERE"
     *   - MSCP$DISK HIT: the queried NAME echoed, 16-byte blank-padded. GROUNDED
     *     byte-exact (af2-firsttimer-established.pcap: OVMX's MSCP$DISK lookup
     *     RESPONSE result@92 == 'MSCP$DISK       '). This DIFFERS from the
     *     VMS$VAXcluster HIT, so the affirmative descriptor is NOT one-size.
     *   - any other affirmative (VMS$VAXcluster): the SCA#38 descriptor blob.
     * The MSCP$DISK-name test uses a 9-char prefix compare so the caller may pass
     * either a NUL- or blank-terminated name. */
    if (!p->request) {
        if (!p->affirmative) {
            memcpy(out + 14 + 78, SCS_DIR_NOT_PRESENT, SCS_DIR_RESULT_LEN);
        } else if (memcmp(p->name, "MSCP$DISK", 9) == 0) {
            /* result == the queried name, same 16-byte blank-padded form as [62:78]. */
            memcpy(out + 14 + 78, out + 14 + 62, SCS_DIR_RESULT_LEN);
        } else {
            memcpy(out + 14 + 78, dir_affirmative_result, SCS_DIR_RESULT_LEN);
        }
    }
    return 0;
}

int scs_dir_build_lookup_response(const struct scs_dir_lookup_params *p,
                                  uint8_t out[SCS_DIR_LOOKUP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* A caller that set `request` and then asked for a RESPONSE would get a
     * frame with a request's marker and an answer's result field -- a shape
     * that exists on no wire. Refuse it rather than emit it. */
    if (p->request) {
        return -1;
    }
    return dir_build_lookup(p, dir_lookup_tmpl, SCS_DIR_ENV_CREDIT_LOOKUP_RSP, out);
}

/* vms-578 INTEGRATION: worktree-760 carried its own scs_dir_build_connect_request
 * here. It is byte-for-byte equivalent to the vms-66f one already defined above
 * (same dir_connreq_tmpl, remote Con.ID forced to 0, local = p->local_conid;
 * the closure version relies on the template's already-zero [50:54] instead of
 * re-zeroing it). The duplicate definition is dropped -- no behaviour is lost.
 * SCS_DIR_CONNREQ_SCA_LEN is #defined to SCS_DIR_RESP_SCA_LEN, so even the
 * length argument is the same value. */

int scs_dir_build_connect_confirm(const struct scs_dir_params *p,
                                  uint8_t out[SCS_DIR_CONFIRM_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* op 3 = ACCEPT_RSP in the sec 4(h)(1a) reading. Con.ID pair now bound:
     * remote = member's dir handle, local = OVMX's own. */
    struct scs_env_fields env = { SCS_DIR_OP_CONFIRM, SCS_DIR_ENV_CREDIT_CONFIRM,
                                  p->remote_conid, p->local_conid };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_confirm_tmpl, SCS_DIR_CONFIRM_SCA_LEN, p->recv_ack,
                     p->send_seq, p->incarnation, &env, out);
    return 0;
}

int scs_dir_build_lookup_request(const struct scs_dir_lookup_params *p,
                                 uint8_t out[SCS_DIR_LOOKUP_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    struct scs_dir_lookup_params q = *p;
    q.request = 1;         /* forces the result field to stay empty */
    q.affirmative = 0;     /* meaningless on a request; make that explicit */
    /* vms-578 INTEGRATION: worktree-760's copy of this builder HARD-CODED
     * op=SCS_DIR_OP_LOOKUP and never touched the opcode byte, because its
     * callers left both fields zero. The vms-66f version kept here makes both
     * caller-supplied (SCA#37 needs opcode 0x4b, SCA#29 needs 0x5b). Defaulting
     * zero preserves the worktree-760 wire bytes for any caller that still
     * leaves them unset, so no merged call site can silently emit opcode 0x00
     * or op 0x0000 -- neither of which exists on any capture. */
    if (q.opcode == 0) {
        q.opcode = SCS_DIR_OPCODE;
    }
    if (q.op == 0) {
        q.op = SCS_DIR_OP_LOOKUP;
    }
    return dir_build_lookup(&q, dir_lookupreq_tmpl,
                            SCS_DIR_ENV_CREDIT_LOOKUP_REQ, out);
}

const char *scs_dir_answer_name(enum scs_dir_answer a)
{
    switch (a) {
    case SCS_DIR_ANSWER_YES:     return "YES";
    case SCS_DIR_ANSWER_NO:      return "NO";
    case SCS_DIR_ANSWER_UNKNOWN: break;
    }
    return "UNKNOWN";
}

int scs_dir_build_mscp_echo(const struct scs_dir_params *p,
                            uint8_t out[SCS_DIR_ECHO_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* Same 66-byte SCA class, same MTYPE 1 = CONNECT_RSP, as the directory
     * CONNECT-ECHO. remote = member's MSCP client handle (echoed); local still
     * 0, because a CONNECT_RSP has bound none. */
    struct scs_env_fields env = { SCS_DIR_MSGTYPE_CONNECT_RSP,
                                  SCS_DIR_ENV_CREDIT_ECHO, p->remote_conid, 0 };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_echo_tmpl, SCS_DIR_ECHO_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, &env, out);
    /* vms-760 delta (1): opcode [16] = 0x4b (data-phase; the VC to OVMX is up),
     * NOT the 0x5b the directory-echo template carries. GROUNDED from the pcap. */
    out[14 + 16] = SCS_MSGTYPE_SEQAPP;
    /* vms-760 delta (2): the truncated SYSAP-name tail [62:66] = 'MSCP' (the
     * 66-byte SCA window clips 'MSCP$DISK' after 4 bytes), NOT the template's
     * 'SCS$'. GROUNDED from the pcap. */
    memcpy(out + 14 + 62, "MSCP", 4);
    return 0;
}

int scs_dir_build_vc_echo(const struct scs_dir_params *p,
                          uint8_t out[SCS_DIR_ECHO_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* vms-760: the joiner's op=1 CONNECT-ECHO answering the MEMBER-opened
     * VMS$VAXcluster VC (af2-firsttimer-established VC pair, ~143.7586). Same
     * 66-byte SCA as the MSCP echo; the only delta is the truncated SYSAP-name
     * tail [62:66] = 'VMS$' ("VMS$VAXcluster" clipped to the 66-byte window).
     * Every accept in this protocol echoes op=1 before its op=2/op=4 response. */
    struct scs_env_fields env = { SCS_DIR_MSGTYPE_CONNECT_RSP,
                                  SCS_DIR_ENV_CREDIT_ECHO,
                                  p->remote_conid, /* member's VC handle (echoed) */
                                  0 };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_echo_tmpl, SCS_DIR_ECHO_SCA_LEN, p->recv_ack, p->send_seq,
                     p->incarnation, &env, out);
    out[14 + 16] = SCS_MSGTYPE_SEQAPP;         /* opcode 0x4b (data-phase) */
    memcpy(out + 14 + 62, "VMS$", 4);          /* GROUNDED name tail */
    return 0;
}

int scs_dir_build_mscp_accept(const struct scs_dir_params *p,
                              uint8_t out[SCS_DIR_CONFIRM_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* Structurally the op=3 dir CONNECT-CONFIRM (same 62-byte SCA, opcode 0x5b,
     * marker 0x00010000, no SYSAP names). */
    /* vms-760: the SINGLE fixed-byte delta vs the confirm IS the MTYPE, op 4 --
     * see the collision note in dir_build_common, RESOLVED by vms-754: op 4 is
     * the shared-namespace REJECT_REQ, not an accept (SCS_DIR_OP_ACCEPT in
     * scs_dir.h carries the full grounding). Con.ID pair still bound the same
     * way: remote = member's MSCP client handle, local = OVMX's fresh MSCP
     * server handle -- the byte layout is unchanged, only the name was wrong. */
    struct scs_env_fields env = { SCS_DIR_OP_ACCEPT, SCS_DIR_ENV_CREDIT_CONFIRM,
                                  p->remote_conid, p->local_conid };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_confirm_tmpl, SCS_DIR_CONFIRM_SCA_LEN, p->recv_ack,
                     p->send_seq, p->incarnation, &env, out);
    return 0;
}

int scs_dir_parse(const uint8_t *frame, size_t len, struct scs_dir_view *v)
{
    if (frame == NULL || v == NULL) {
        return -1;
    }
    /* Need the Ethernet header + SCA envelope through the Con.ID pair
     * (abs 14 + 58 = 72). */
    if (len < 72) {
        return -1;
    }

    memset(v, 0, sizeof(*v));

    uint16_t lenword = get_le16(frame + 14);
    v->total_sca_len = (uint16_t)(lenword + 2);
    v->opcode = frame[14 + 16];
    v->format = frame[14 + 17];
    v->recv_ack = get_le16(frame + 14 + 18);
    v->send_seq = get_le16(frame + 14 + 20);
    v->op = get_le16(frame + 14 + 46);
    v->flag = get_le16(frame + 14 + 48);
    v->remote_conid = get_le32(frame + 14 + 50);
    v->local_conid = get_le32(frame + 14 + 54);
    v->marker = get_le32(frame + 14 + 58);

    /* Name field [62:78] (abs 76-91). */
    if (len >= 14 + 78) {
        v->has_name = 1;
        memcpy(v->name, frame + 14 + 62, SCS_DIR_NAME_LEN);
        v->name[SCS_DIR_NAME_LEN] = '\0';
    }

    /* Result field [78:94] (abs 92-107). */
    if (len >= 14 + 94) {
        v->has_result = 1;
        v->result_zero = 1;
        for (size_t i = 0; i < SCS_DIR_RESULT_LEN; i++) {
            if (frame[14 + 78 + i] != 0) {
                v->result_zero = 0;
                break;
            }
        }
    }

    /* Classification. A SCS$DIRECTORY CONNECT-REQUEST names "SCS$DIRECTORY" in
     * [62:78] and has not learned OVMX's handle yet (remote_conid == 0). */
    if (v->has_name && v->remote_conid == 0 &&
        memcmp(v->name, "SCS$DIRECTORY", 13) == 0) {
        v->is_dir_connect_request = 1;
    }
    /* A lookup REQUEST carries op==0x0a with a queried name and the [58:62]
     * request marker == 0 (a RESPONSE sets it to 1). NOTE: the result field
     * [78:94] is NOT a reliable discriminator -- the golden capture's requests
     * happened to carry a zero result, but a live VAX's lookup request fills
     * [78:94] with request-context bytes (observed on the vms-246 lab wire), so
     * result_zero would misclassify it. The [58:62] marker is the robust
     * request/response discriminator, grounded on both the golden capture and
     * the live join. */
    if (v->op == SCS_DIR_OP_LOOKUP && v->has_name && v->has_result && v->marker == 0) {
        v->is_lookup_request = 1;
    }
    /* vms-66f: the same marker, read the other way -- a lookup RESPONSE sets
     * [58:62] to 1. This is the frame the SCS Process Poller waits for. */
    if (v->op == SCS_DIR_OP_LOOKUP && v->has_name && v->has_result && v->marker == 1) {
        v->is_lookup_response = 1;
        if (v->result_zero) {
            v->answer = SCS_DIR_ANSWER_UNKNOWN;
        } else if (memcmp(frame + 14 + 78, SCS_DIR_NOT_PRESENT, SCS_DIR_RESULT_LEN) == 0) {
            v->answer = SCS_DIR_ANSWER_NO;
        } else {
            v->answer = SCS_DIR_ANSWER_YES;
        }
    }

    return 0;
}

/*
 * scs_dir_build_mscp_confirm5 -- vms-754 CORRECTION (2026-08-06). This
 * function's own name and the "form A/form B accept" model below are
 * REFUTED: op 4/5 are the shared-namespace REJECT_REQ/REJECT_RSP (see
 * SCS_DIR_OP_ACCEPT / SCS_DIR_OP_MSCP_CONFIRM in scs_dir.h for the full
 * grounding), not a second accept form. What is NOT refuted is the byte
 * layout and the wedge-avoidance fix -- OVMX must still answer an op-4 with
 * an op-5 on the wire (whatever it actually means) or a real peer that sent
 * one gets nothing back and the caller retransmits forever; that half of
 * this function's job stands. NOT rewired here -- see scsd.c's FORM B
 * comment for the open wire-behaviour follow-up (out of scope for vms-754).
 * REFUTED-QUOTE-BEGIN
 *   answer a peer's op-4 ACCEPT4 with an op-5 CONFIRM5, completing the
 *   "form B" accept of a connection WE opened.
 *
 *   There are two accept forms on an MSCP$DISK connection and OVMX only ever
 *   implemented half of each:
 *     form A: op 0 -> op 1 -> op 2 RESPONSE -> op 3 CONFIRM      (we handle this)
 *     form B: op 0 -> op 1 -> op 4 ACCEPT4  -> op 5 CONFIRM5     (we EMIT op 4 as
 *             a server, but could not CONSUME one as a client)
 * REFUTED-QUOTE-END
 *
 * The consequence was not a missing feature, it was a wedged node: when VAX3
 * answered our connect with an op-4 we silently dropped it, then retransmitted
 * the same request 60 times over 178 s with a frozen send_seq.
 *
 * Grounded on 336 op-5 frames from 4 sender nodes across 15 captures, including
 * three a real VAX sent AT OVMX. Con.ID convention is identical to the op-3
 * confirm: [50] = the peer's handle, taken from the op-4's [54]; [54] = our own,
 * the handle we put in our op-0. Nothing follows an op-5 -- across all 336 the
 * Con.ID pair never appears again (334 silent, 2 retransmits) -- which
 * tools/cluster/scs_t45_measure.py's corpus-wide census (733/733 terminal,
 * against 388/394 for the undisputed ACCEPT_REQ positive control) shows is the
 * REJECT signature, not "a bound connection needs no more".
 */
int scs_dir_build_mscp_confirm5(const struct scs_dir_params *p,
                                uint8_t out[SCS_DIR_CONFIRM5_FRAME_LEN])
{
    if (p == NULL || out == NULL) {
        return -1;
    }
    /* op 5 -- the collision noted in dir_build_common, RESOLVED by vms-754:
     * this is a REJECT_RSP, not an accept confirm (see SCS_DIR_OP_MSCP_CONFIRM
     * in scs_dir.h). Con.ID placement unchanged: [50] = the peer's handle,
     * taken from the answered frame's [54]; [54] = our own, the handle we put
     * in our op-0. */
    struct scs_env_fields env = { SCS_DIR_OP_MSCP_CONFIRM,
                                  SCS_DIR_ENV_CREDIT_CONFIRM, p->remote_conid,
                                  p->local_conid };
    dir_build_common(p->dst_mac, p->src_mac, p->src_logical, p->peer_logical,
                     dir_confirm5_tmpl, SCS_DIR_CONFIRM5_SCA_LEN, p->recv_ack,
                     p->send_seq, p->incarnation, &env, out);
    return 0;
}
