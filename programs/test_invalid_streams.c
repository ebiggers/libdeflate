/*
 * test_invalid_streams.c
 *
 * Test that invalid DEFLATE streams are rejected with LIBDEFLATE_BAD_DATA.
 *
 * This isn't actually very important, since DEFLATE doesn't have built-in error
 * detection, so corruption of a DEFLATE stream can only be reliably detected
 * using a separate checksum anyway.  As long as the DEFLATE decompressor
 * handles all streams safely (no crashes, etc.), in practice it is fine for it
 * to automatically remap invalid streams to valid streams, instead of returning
 * an error.  Corruption detection is the responsibility of the zlib or gzip
 * layer, or the application when an external checksum is used.
 *
 * Nevertheless, to reduce surprises when people intentionally compare zlib's
 * and libdeflate's handling of invalid DEFLATE streams, libdeflate implements
 * zlib's strict behavior when decoding DEFLATE, except when it would have a
 * significant performance cost.
 */

#include "test_util.h"

static void
assert_decompression_error(const u8 *in, size_t in_nbytes)
{
	struct libdeflate_decompressor *d;
	z_stream z;
	u8 out[128];
	const size_t out_nbytes_avail = sizeof(out);
	size_t actual_out_nbytes;
	enum libdeflate_result res;

	/* libdeflate */
	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);
	res = libdeflate_deflate_decompress(d, in, in_nbytes,
					    out, out_nbytes_avail,
					    &actual_out_nbytes);
	ASSERT(res == LIBDEFLATE_BAD_DATA);
	libdeflate_free_decompressor(d);

	/* zlib, as a control */
	memset(&z, 0, sizeof(z));
	res = inflateInit2(&z, -15);
	ASSERT(res == Z_OK);
	z.next_in = (void *)in;
	z.avail_in = in_nbytes;
	z.next_out = (void *)out;
	z.avail_out = out_nbytes_avail;
	res = inflate(&z, Z_FINISH);
	ASSERT(res == Z_DATA_ERROR);
	inflateEnd(&z);
}

static void
assert_zlib_decompression_error(const u8 *in, size_t in_nbytes)
{
	struct libdeflate_decompressor *d;
	z_stream z;
	u8 out[128];
	const size_t out_nbytes_avail = sizeof(out);
	size_t actual_out_nbytes;
	enum libdeflate_result res;

	/* libdeflate */
	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);
	res = libdeflate_zlib_decompress(d, in, in_nbytes,
					 out, out_nbytes_avail,
					 &actual_out_nbytes);
	ASSERT(res == LIBDEFLATE_BAD_DATA);
	libdeflate_free_decompressor(d);

	/* zlib, as a control */
	memset(&z, 0, sizeof(z));
	res = inflateInit2(&z, 15);
	ASSERT(res == Z_OK);
	z.next_in = (void *)in;
	z.avail_in = in_nbytes;
	z.next_out = (void *)out;
	z.avail_out = out_nbytes_avail;
	res = inflate(&z, Z_FINISH);
	ASSERT(res == Z_DATA_ERROR);
	inflateEnd(&z);
}

/*
 * Test that DEFLATE decompression returns an error if a block header contains
 * too many encoded litlen and offset codeword lengths.
 */
static void
test_too_many_codeword_lengths(void)
{
	u8 in[128];
	struct output_bitstream os = { .next = in, .end = in + sizeof(in) };
	int i;

	ASSERT(put_bits(&os, 1, 1));	/* BFINAL: 1 */
	ASSERT(put_bits(&os, 2, 2));	/* BTYPE: DYNAMIC_HUFFMAN */

	/*
	 * Litlen code:
	 *	litlensym_255			len=1 codeword=0
	 *	litlensym_256 (end-of-block)	len=1 codeword=1
	 * Offset code:
	 *	(empty)
	 *
	 * Litlen and offset codeword lengths:
	 *	[0..254] = 0	presym_{18,18}
	 *	[255]	 = 1	presym_1
	 *	[256]	 = 1	presym_1
	 *	[257...] = 0	presym_18 [TOO MANY]
	 *
	 * Precode:
	 *	presym_1	len=1 codeword=0
	 *	presym_18	len=1 codeword=1
	 */

	ASSERT(put_bits(&os, 0, 5));	/* num_litlen_syms: 0 + 257 */
	ASSERT(put_bits(&os, 0, 5));	/* num_offset_syms: 0 + 1 */
	ASSERT(put_bits(&os, 14, 4));	/* num_explicit_precode_lens: 14 + 4 */

	/*
	 * Precode codeword lengths: order is
	 * [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
	 */
	for (i = 0; i < 2; i++)		/* presym_{16,17}: len=0 */
		ASSERT(put_bits(&os, 0, 3));
	ASSERT(put_bits(&os, 1, 3));	/* presym_18: len=1 */
	ASSERT(put_bits(&os, 0, 3));	/* presym_0: len=0 */
	for (i = 0; i < 13; i++)	/* presym_{8,...,14}: len=0 */
		ASSERT(put_bits(&os, 0, 3));
	ASSERT(put_bits(&os, 1, 3));	/* presym_1: len=1 */

	/* Litlen and offset codeword lengths */
	ASSERT(put_bits(&os, 0x1, 1) &&	/* presym_18, 128 zeroes */
	       put_bits(&os, 117, 7));
	ASSERT(put_bits(&os, 0x1, 1) &&	/* presym_18, 127 zeroes */
	       put_bits(&os, 116, 7));
	ASSERT(put_bits(&os, 0x0, 1));	/* presym_1 */
	ASSERT(put_bits(&os, 0x0, 1));	/* presym_1 */
	ASSERT(put_bits(&os, 0x1, 1) &&	/* presym_18, 128 zeroes [TOO MANY] */
	       put_bits(&os, 117, 7));

	/* Literal */
	ASSERT(put_bits(&os, 0x0, 0));	/* litlensym_255 */

	/* End of block */
	ASSERT(put_bits(&os, 0x1, 1));	/* litlensym_256 */

	ASSERT(flush_bits(&os));

	assert_decompression_error(in, os.next - in);
}

static const u8 poc1[100] = {
	0x78, 0x9c, 0x15, 0xca, 0xc1, 0x0d, 0xc3, 0x20, 0x0c, 0x00, 0xc0, 0x7f,
	0xa6, 0xf0, 0x02, 0x40, 0xd2, 0x77, 0xe9, 0x2a, 0xc8, 0xa1, 0xa0, 0x5a,
	0x4a, 0x89, 0x65, 0x1b, 0x29, 0xf2, 0xf4, 0x51, 0xee, 0x3d, 0x31, 0x3d,
	0x3a, 0x7c, 0xb6, 0xc4, 0xe0, 0x9c, 0xd4, 0x0e, 0x00, 0x00, 0x00, 0x3d,
	0x85, 0xa7, 0x26, 0x08, 0x33, 0x87, 0xde, 0xd7, 0xfa, 0x80, 0x80, 0x62,
	0xd4, 0xb1, 0x32, 0x87, 0xe6, 0xd7, 0xfa, 0x80, 0x80, 0x62, 0xd4, 0xb1,
	0x26, 0x61, 0x69, 0x9d, 0xae, 0x1c, 0x53, 0x15, 0xd4, 0x5f, 0x7b, 0x22,
	0x0b, 0x0d, 0x2b, 0x9d, 0x12, 0x32, 0x56, 0x0d, 0x4d, 0xff, 0xb6, 0xdc,
	0x8a, 0x02, 0x27, 0x38,
};

static const u8 poc2[86] = {
	0x78, 0x9c, 0x15, 0xca, 0xc1, 0x0d, 0xc3, 0x20, 0x0c, 0x00, 0xc0, 0x7f,
	0xa6, 0xf0, 0x02, 0x40, 0xd2, 0x77, 0xe9, 0x2a, 0xc8, 0xa1, 0xa0, 0x5a,
	0x4a, 0x89, 0x65, 0x1b, 0x29, 0xf2, 0xf4, 0x51, 0xee, 0xff, 0xff, 0xff,
	0x03, 0x37, 0x08, 0x5f, 0x78, 0xc3, 0x16, 0xfc, 0xa0, 0x3d, 0x3a, 0x7c,
	0x9d, 0xae, 0x1c, 0x53, 0x15, 0xd4, 0x5f, 0x7b, 0x22, 0x0b, 0x0d, 0x2b,
	0x9d, 0x12, 0x34, 0x56, 0x0d, 0x4d, 0xff, 0xb6, 0xdc, 0x4e, 0xc9, 0x14,
	0x67, 0x9c, 0x3e, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xba, 0xec,
	0x0b, 0x1d,
};

static const u8 poc3[31] = {
	0x78, 0x9c, 0xea, 0xca, 0xc1, 0x0d, 0xc3, 0x00, 0x5b, 0x2d, 0xee, 0x7d,
	0x31, 0x78, 0x9c, 0x15, 0xca, 0xc1, 0x0d, 0xc3, 0x20, 0x0c, 0x00, 0x18,
	0x31, 0x85, 0x07, 0x02, 0x40, 0x39, 0x13,
};

static const u8 poc4[86] = {
	0x78, 0x9c, 0x15, 0xc6, 0xc1, 0x0d, 0xc3, 0x20, 0x0c, 0x00, 0xc0, 0x7f,
	0xa6, 0xf0, 0x02, 0x40, 0xd2, 0x77, 0xe9, 0x2a, 0xc8, 0xa1, 0xa0, 0x5a,
	0x4a, 0x89, 0x65, 0x1b, 0x29, 0xf2, 0xf4, 0x51, 0xee, 0x7d, 0x30, 0x39,
	0x13, 0x37, 0x08, 0x5f, 0x78, 0xc3, 0x16, 0xfc, 0xa0, 0x3d, 0x3a, 0x7c,
	0xe0, 0x9c, 0x1b, 0x29, 0xf2, 0xf4, 0x51, 0xee, 0xdf, 0xd2, 0x0c, 0x4e,
	0x26, 0x08, 0x32, 0x87, 0x53, 0x15, 0xd4, 0x4d, 0xff, 0xb6, 0xdc, 0x45,
	0x8d, 0xc0, 0x3b, 0xa6, 0xf0, 0x40, 0xee, 0x51, 0x02, 0x7d, 0x45, 0x8d,
	0x2b, 0xca,
};

int
tmain(int argc, tchar *argv[])
{
	begin_program(argv);

	test_too_many_codeword_lengths();

	assert_zlib_decompression_error(poc1, sizeof(poc1));
	assert_zlib_decompression_error(poc2, sizeof(poc2));
	assert_zlib_decompression_error(poc3, sizeof(poc3));
	assert_zlib_decompression_error(poc4, sizeof(poc4));
	return 0;
}
