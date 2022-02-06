/*
 * test_overread.c
 *
 * Test that the decompressor doesn't produce an unbounded amount of output if
 * it runs out of input, even when implicit zeroes appended to the input would
 * continue producing output (as is the case when the input ends during a
 * DYNAMIC_HUFFMAN block where a literal has an all-zeroes codeword).
 *
 * This is a regression test for commit 3f21ec9d6121 ("deflate_decompress: error
 * out if overread count gets too large").
 */

#include "test_util.h"

static void
generate_test_input(struct output_bitstream *os)
{
	int i;

	put_bits(os, 0, 1);	/* BFINAL: 0 */
	put_bits(os, 2, 2);	/* BTYPE: DYNAMIC_HUFFMAN */

	/*
	 * Write the Huffman codes.
	 *
	 * Litlen code:
	 *	litlensym_0   (0)		len=1 codeword=0
	 *	litlensym_256 (end-of-block)	len=1 codeword=1
	 * Offset code:
	 *	offsetsym_0 (unused)		len=1 codeword=0
	 *
	 * Litlen and offset codeword lengths:
	 *	[0]	 = 1	presym_1
	 *	[1..255] = 0	presym_{18,18}
	 *	[256]	 = 1	presym_1
	 *	[257]	 = 1	presym_1
	 *
	 * Precode:
	 *	presym_1	len=1 codeword=0
	 *	presym_18	len=1 codeword=1
	 */
	put_bits(os, 0, 5);	/* num_litlen_syms: 0 + 257 */
	put_bits(os, 0, 5);	/* num_offset_syms: 0 + 1 */
	put_bits(os, 14, 4);	/* num_explicit_precode_lens: 14 + 4 */
	/*
	 * Precode codeword lengths: order is
	 * [16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15]
	 */
	put_bits(os, 0, 3);		/* presym_16: len=0 */
	put_bits(os, 0, 3);		/* presym_17: len=0 */
	put_bits(os, 1, 3);		/* presym_18: len=1 */
	for (i = 0; i < 14; i++)	/* presym_{0,...,14}: len=0 */
		put_bits(os, 0, 3);
	put_bits(os, 1, 3);		/* presym_1: len=1 */

	/* Litlen and offset codeword lengths */
	put_bits(os, 0, 1);		/* presym_1 */
	put_bits(os, 1, 1);		/* presym_18 ... */
	put_bits(os, 117, 7);		/* ... 11 + 117 zeroes */
	put_bits(os, 1, 1);		/* presym_18 ... */
	put_bits(os, 116, 7);		/* ... 11 + 116 zeroes */
	put_bits(os, 0, 1);		/* presym_1 */
	put_bits(os, 0, 1);		/* presym_1 */

	/* Implicit zeroes would generate endless literals from here. */

	ASSERT(flush_bits(os));
}

int
tmain(int argc, tchar *argv[])
{
	u8 cdata[16];
	u8 udata[256];
	struct output_bitstream os =
		{ .next = cdata, .end = cdata + sizeof(cdata) };
	struct libdeflate_decompressor *d;
	enum libdeflate_result res;
	size_t actual_out_nbytes;

	begin_program(argv);

	generate_test_input(&os);
	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	res = libdeflate_deflate_decompress(d, cdata, os.next - cdata,
					    udata, sizeof(udata),
					    &actual_out_nbytes);
	/* Before the fix, the result was LIBDEFLATE_INSUFFICIENT_SPACE here. */
	ASSERT(res == LIBDEFLATE_BAD_DATA);

	libdeflate_free_decompressor(d);
	return 0;
}
