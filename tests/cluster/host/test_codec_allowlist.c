// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_allowlist.c - the (SYSAP, category, opcode) response allowlist
 * TYPE (FC-P0.6, rung R1).
 *
 * The rows themselves belong to the layers that speak each SYSAP (FC-P3.1 for
 * the connection manager, FC-P4.5/P5.2 for the DLM). What is proved here is
 * the MECHANISM, because the mechanism is what failed catastrophically once:
 * OVMX answered ungrounded (category, opcode) pairs with a cat-0x01 full-body
 * echo and crashed two real VAXes (INCONSTATE on VAX3, INVEXCEPTN on VAX1),
 * because those bodies carry the peer's own live Con.IDs.
 *
 * So: no default, no wildcard, no response-bit key, no duplicate row, no row
 * without a spec cite, and no RESPOND row without a builder to run.
 */

#include "cluster_test.h"
#include "vms_cluster_codec.h"

/*
 * A DELIBERATELY FICTITIOUS table. These are not grounded rows and must never
 * be copied into a layer: they exercise the lookup and the validator only.
 * Grounded rows arrive with the item that owns the SYSAP.
 */
static const struct vms_wire_allow_entry test_rows[] = {
	{ VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03, VMS_WIRE_ACT_RESPOND, 1,
	  "test-only row, not grounded" },
	{ VMS_SYSAP_VMS_VAXCLUSTER, 0x04, 0x00, VMS_WIRE_ACT_CONSUME, 0,
	  "test-only row, not grounded" },
	{ VMS_SYSAP_SCS_DIRECTORY,  0x01, 0x03, VMS_WIRE_ACT_RESPOND, 2,
	  "test-only row, not grounded" },
};
static const struct vms_wire_allow_table test_table = { test_rows, 3 };

static void test_response_bit(void)
{
	printf("-- the response bit (spec sec 4j: bit 0x80 marks a response)\n");
	ct_check(!vms_wire_is_response(0x01), "cat 0x01 is a request");
	ct_check(vms_wire_is_response(0x81), "cat 0x81 is a response");
	ct_check_eq_u32(vms_wire_response_category(0x01), 0x81,
			"0x01 -> 0x81");
	ct_check_eq_u32(vms_wire_response_category(0x02), 0x82,
			"0x02 -> 0x82");
	ct_check_eq_u32(vms_wire_response_category(0x06), 0x86,
			"0x06 -> 0x86");
}

static void test_exact_match_only(void)
{
	printf("-- lookup is an exact triple match, with no default\n");
	ct_check(vms_wire_allow_find(&test_table, VMS_SYSAP_VMS_VAXCLUSTER,
				     0x01, 0x03) == &test_rows[0],
		 "listed (sysap, cat, op) hits its row");
	ct_check(vms_wire_allow_find(&test_table, VMS_SYSAP_VMS_VAXCLUSTER,
				     0x01, 0x0f) == NULL,
		 "an UNLISTED opcode is NULL -- send nothing, log it");
	ct_check(vms_wire_allow_find(&test_table, VMS_SYSAP_VMS_VAXCLUSTER,
				     0x02, 0x03) == NULL,
		 "the same opcode under an unlisted CATEGORY is NULL");
	ct_check(vms_wire_allow_find(&test_table, VMS_SYSAP_MSCP_DISK,
				     0x01, 0x03) == NULL,
		 "the same (cat, op) on a different SYSAP is NULL: the SYSAP "
		 "dimension is real, not decoration");
	ct_check(vms_wire_allow_find(&test_table, VMS_SYSAP_SCS_DIRECTORY,
				     0x01, 0x03) == &test_rows[2],
		 "  and the SCS$DIRECTORY row is a separate hit");
	ct_check(vms_wire_allow_find(&test_table, VMS_SYSAP_VMS_VAXCLUSTER,
				     0x81, 0x03) == NULL,
		 "a RESPONSE category never matches: responses are correlated "
		 "by (txn, checksum, opcode), never answered");
	ct_check(vms_wire_allow_find(NULL, VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03)
		 == NULL, "NULL table is NULL, not a crash");
}

static void test_empty_table_denies_everything(void)
{
	struct vms_wire_allow_table empty = { NULL, 0 };
	unsigned cat, op;
	int any = 0;

	printf("-- an empty table answers NOTHING (silence is the default)\n");
	for (cat = 0; cat < 0x80u; cat++) {
		for (op = 0; op < 0x100u; op++) {
			if (vms_wire_allow_find(&empty,
						VMS_SYSAP_VMS_VAXCLUSTER,
						(uint8_t)cat, (uint8_t)op))
				any = 1;
		}
	}
	ct_check(!any, "no (cat, op) of 32768 is answered by an empty table");
	ct_check(vms_wire_allow_table_validate(&empty) == VMS_CODEC_OK,
		 "an empty table is valid");
}

static void check_reject(const struct vms_wire_allow_entry *row,
			 const char *what)
{
	struct vms_wire_allow_table t = { row, 1 };

	ct_check(vms_wire_allow_table_validate(&t) == VMS_CODEC_E_INVAL, what);
}

static void test_validator(void)
{
	static const struct vms_wire_allow_entry bad_sysap = {
		VMS_SYSAP_UNKNOWN, 0x01, 0x03, VMS_WIRE_ACT_RESPOND, 1, "x" };
	static const struct vms_wire_allow_entry bad_respbit = {
		VMS_SYSAP_VMS_VAXCLUSTER, 0x81, 0x03, VMS_WIRE_ACT_RESPOND, 1, "x" };
	static const struct vms_wire_allow_entry bad_action = {
		VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03, VMS_WIRE_ACT_NONE, 0, "x" };
	static const struct vms_wire_allow_entry bad_recipe = {
		VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03, VMS_WIRE_ACT_RESPOND, 0, "x" };
	static const struct vms_wire_allow_entry bad_consume_recipe = {
		VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03, VMS_WIRE_ACT_CONSUME, 7, "x" };
	static const struct vms_wire_allow_entry bad_nospec = {
		VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03, VMS_WIRE_ACT_RESPOND, 1, NULL };
	static const struct vms_wire_allow_entry dupes[2] = {
		{ VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03, VMS_WIRE_ACT_RESPOND, 1, "x" },
		{ VMS_SYSAP_VMS_VAXCLUSTER, 0x01, 0x03, VMS_WIRE_ACT_CONSUME, 0, "x" },
	};
	struct vms_wire_allow_table dup_table = { dupes, 2 };

	printf("-- table validator\n");
	ct_check(vms_wire_allow_table_validate(&test_table) == VMS_CODEC_OK,
		 "a well-formed table validates");
	check_reject(&bad_sysap, "sysap UNKNOWN rejected");
	check_reject(&bad_respbit,
		     "a category with the response bit set rejected");
	check_reject(&bad_action, "action NONE rejected (that is 'not listed')");
	check_reject(&bad_recipe, "RESPOND with no builder id rejected");
	check_reject(&bad_consume_recipe, "CONSUME with a builder id rejected");
	check_reject(&bad_nospec, "a row with no spec cite rejected");
	ct_check(vms_wire_allow_table_validate(&dup_table) == VMS_CODEC_E_INVAL,
		 "duplicate keys rejected (an ambiguous table is a coin flip)");
	ct_check(vms_wire_allow_table_validate(NULL) == VMS_CODEC_E_INVAL,
		 "NULL table rejected");
}

int main(void)
{
	printf("test_codec_allowlist: response allowlist type (FC-P0.6)\n");
	test_response_bit();
	test_exact_match_only();
	test_empty_table_denies_everything();
	test_validator();
	return ct_summary("test_codec_allowlist");
}
