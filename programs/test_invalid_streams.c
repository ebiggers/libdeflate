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

int
tmain(int argc, tchar *argv[])
{
	begin_program(argv);

	test_too_many_codeword_lengths();
	return 0;
}
