// SPDX-License-Identifier: GPL-2.0
/*
 * test_codec_wire.c - the typed get/put primitives and their bounds discipline
 * (FC-P0.6, rung R1).
 *
 * These are the functions every later codec entry is built out of, so their
 * failure mode matters more than their success mode: an out-of-range read must
 * latch a STICKY error and yield nothing a caller could mistake for data
 * (INV-6), and a write past the buffer must not happen at all.
 */

#include "cluster_test.h"
#include "vms_cluster_codec.h"

#include <string.h>

static void test_little_endian_reads(void)
{
	static const uint8_t b[8] = { 0x09, 0x00, 0xc5, 0x62,
				      0x76, 0x00, 0x60, 0x07 };
	vms_wire_view_t v;

	printf("-- little-endian reads\n");
	vms_wire_view_init(&v, b, sizeof(b));
	ct_check_eq_u32(vms_wire_get_u8(&v, 0), 0x09, "get_u8");
	ct_check_eq_u32(vms_wire_get_le16(&v, 4), 0x0076,
			"get_le16 == SCA length field 0x0076");
	ct_check_eq_u32(vms_wire_get_le32(&v, 0), 0x62c50009u,
			"get_le32 == Con.ID 0x62C50009");
	ct_check_eq_u32(vms_wire_get_be16(&v, 6), 0x6007,
			"get_be16 == ethertype 0x6007 (big-endian on the wire)");
	ct_check(vms_wire_view_ok(&v), "view still clean after in-range reads");
}

static void test_sticky_range_error(void)
{
	static const uint8_t b[4] = { 1, 2, 3, 4 };
	vms_wire_view_t v;
	uint8_t dst[4];

	printf("-- out-of-range reads latch and stay latched\n");
	vms_wire_view_init(&v, b, sizeof(b));
	ct_check_eq_u32(vms_wire_get_le32(&v, 1), 0,
			"straddling read yields 0, not garbage");
	ct_check(!vms_wire_view_ok(&v), "view error latched");
	ct_check_eq_u32(vms_wire_get_u8(&v, 0), 0,
			"a later IN-range read still yields 0 once latched");
	ct_check(v.err == VMS_CODEC_E_RANGE, "status is E_RANGE");

	vms_wire_view_init(&v, b, sizeof(b));
	memset(dst, 0xaa, sizeof(dst));
	vms_wire_get_bytes(&v, 2, 4, dst);
	ct_check(!vms_wire_view_ok(&v), "get_bytes past the end fails");
	ct_check(dst[0] == 0 && dst[3] == 0,
		 "get_bytes zeroes its destination on failure");
}

static void test_offset_overflow(void)
{
	static const uint8_t b[8] = { 0 };
	vms_wire_view_t v;

	printf("-- offset arithmetic cannot wrap\n");
	vms_wire_view_init(&v, b, sizeof(b));
	(void)vms_wire_get_le32(&v, 0xfffffffcu);
	ct_check(v.err == VMS_CODEC_E_RANGE,
		 "huge offset is rejected, not wrapped into range");
}

static void test_writes_and_highwater(void)
{
	uint8_t out[16];
	vms_wire_buf_t w;

	printf("-- put primitives and the high-water mark\n");
	memset(out, 0xee, sizeof(out));
	vms_wire_buf_init(&w, out, sizeof(out));
	vms_wire_put_be16(&w, 0, 0x6007);
	vms_wire_put_le16(&w, 2, 0x0076);
	vms_wire_put_le32(&w, 4, 0x62c50009u);
	vms_wire_put_u8(&w, 8, 0x4b);
	vms_wire_put_zero(&w, 9, 3);
	ct_check(vms_wire_buf_ok(&w), "all writes in range");
	ct_check(out[0] == 0x60 && out[1] == 0x07, "be16 byte order");
	ct_check(out[2] == 0x76 && out[3] == 0x00, "le16 byte order");
	ct_check(out[4] == 0x09 && out[7] == 0x62, "le32 byte order");
	ct_check(out[9] == 0 && out[11] == 0, "put_zero wrote zeroes");
	ct_check_eq_u32(vms_wire_buf_len(&w), 12, "high-water mark");
	ct_check(out[12] == 0xee, "bytes past the high-water mark untouched");
}

static void test_write_bounds(void)
{
	uint8_t out[4] = { 1, 2, 3, 4 };
	vms_wire_buf_t w;

	printf("-- writes past the buffer are refused, not truncated\n");
	vms_wire_buf_init(&w, out, sizeof(out));
	vms_wire_put_le32(&w, 2, 0xdeadbeefu);
	ct_check(!vms_wire_buf_ok(&w), "straddling write fails");
	ct_check(out[2] == 3 && out[3] == 4, "buffer untouched by the failure");
	ct_check_eq_u32(vms_wire_buf_len(&w), 0, "high-water not advanced");
}

static void test_null_safety(void)
{
	vms_wire_view_t v;
	vms_wire_buf_t w;

	printf("-- NULL buffers\n");
	vms_wire_view_init(&v, NULL, 100);
	ct_check(!vms_wire_view_ok(&v) && v.err == VMS_CODEC_E_INVAL,
		 "NULL view is E_INVAL, and length claim ignored");
	vms_wire_buf_init(&w, NULL, 100);
	ct_check(!vms_wire_buf_ok(&w), "NULL build buffer is not ok");
}

int main(void)
{
	printf("test_codec_wire: typed wire primitives (FC-P0.6)\n");
	test_little_endian_reads();
	test_sticky_range_error();
	test_offset_overflow();
	test_writes_and_highwater();
	test_write_bounds();
	test_null_safety();
	return ct_summary("test_codec_wire");
}
