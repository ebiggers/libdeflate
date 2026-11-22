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

#define NUM_PRECODE_SYMS	19

static const u8 precode_perm[NUM_PRECODE_SYMS] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

static u16
reverse_bits(u16 val, unsigned nbits)
{
	u16 rev = 0;
	unsigned i;

	for (i = 0; i < nbits; i++) {
		rev = (u16)((rev << 1) | (val & 1));
		val >>= 1;
	}
	return rev;
}

/*
 * Assert that both libdeflate and zlib reject the given invalid DEFLATE stream.
 * For libdeflate, test both a small output buffer (where compound litlen
 * entries are disabled) and a large output buffer with trailing input slack
 * (where compound litlen entries are enabled).
 */
static void
assert_decompression_error(const u8 *in, size_t in_nbytes)
{
	struct libdeflate_decompressor *d;
	z_stream z;
	u8 small_out[1024];
	u8 *large_out;
	u8 *padded_in;
	size_t padded_in_nbytes = in_nbytes + 64;
	size_t actual_out_nbytes;
	enum libdeflate_result res;
	int zres;

	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	/* libdeflate (small output buffer) */
	res = libdeflate_deflate_decompress(d, in, in_nbytes,
					    small_out, sizeof(small_out),
					    &actual_out_nbytes);
	ASSERT(res == LIBDEFLATE_BAD_DATA);

	/* libdeflate (large output buffer + trailing zero input bytes) */
	large_out = xmalloc(16384);
	padded_in = xmalloc(padded_in_nbytes);
	memcpy(padded_in, in, in_nbytes);
	memset(padded_in + in_nbytes, 0, 64);
	res = libdeflate_deflate_decompress(d, padded_in, padded_in_nbytes,
					    large_out, 16384,
					    &actual_out_nbytes);
	ASSERT(res == LIBDEFLATE_BAD_DATA);
	free(padded_in);

	libdeflate_free_decompressor(d);

	/* zlib, as a control */
	memset(&z, 0, sizeof(z));
	zres = inflateInit2(&z, -15);
	ASSERT(zres == Z_OK);
	z.next_in = (void *)in;
	z.avail_in = (uInt)in_nbytes;
	z.next_out = large_out;
	z.avail_out = 16384;
	zres = inflate(&z, Z_FINISH);
	ASSERT(zres == Z_DATA_ERROR);
	inflateEnd(&z);
	free(large_out);
}

/*
 * Emit a dynamic Huffman block using a complete 4-bit precode over explicit
 * codeword lengths 0..15 (16 * 2^-4 == 1), followed by zero bits for the body,
 * and assert that decompression fails with LIBDEFLATE_BAD_DATA.
 */
static void
assert_dynamic_codes_rejected(const u8 *litlen_lens, size_t num_litlen_syms,
			      const u8 *offset_lens, size_t num_offset_syms)
{
	u8 in[512];
	struct output_bitstream os = { .next = in, .end = in + sizeof(in) };
	u16 precode_cws[16];
	size_t i;

	ASSERT(num_litlen_syms >= 257 && num_litlen_syms <= 286);
	ASSERT(num_offset_syms >= 1 && num_offset_syms <= 30);

	for (i = 0; i < 16; i++)
		precode_cws[i] = reverse_bits((u16)i, 4);

	ASSERT(put_bits(&os, 1, 1));	/* BFINAL: 1 */
	ASSERT(put_bits(&os, 2, 2));	/* BTYPE: DYNAMIC_HUFFMAN */
	ASSERT(put_bits(&os, (u32)(num_litlen_syms - 257), 5));
	ASSERT(put_bits(&os, (u32)(num_offset_syms - 1), 5));
	ASSERT(put_bits(&os, NUM_PRECODE_SYMS - 4, 4));

	for (i = 0; i < NUM_PRECODE_SYMS; i++) {
		unsigned sym = precode_perm[i];

		ASSERT(put_bits(&os, (sym < 16) ? 4 : 0, 3));
	}
	for (i = 0; i < num_litlen_syms; i++)
		ASSERT(put_bits(&os, precode_cws[litlen_lens[i]], 4));
	for (i = 0; i < num_offset_syms; i++)
		ASSERT(put_bits(&os, precode_cws[offset_lens[i]], 4));
	ASSERT(flush_bits(&os));
	memset(os.next, 0, 4);
	os.next += 4;

	assert_decompression_error(in, (size_t)(os.next - in));
}

static void
assert_precode_rejected(const u8 precode_lens[NUM_PRECODE_SYMS])
{
	u8 in[128];
	struct output_bitstream os = { .next = in, .end = in + sizeof(in) };
	size_t i;

	ASSERT(put_bits(&os, 1, 1));	/* BFINAL: 1 */
	ASSERT(put_bits(&os, 2, 2));	/* BTYPE: DYNAMIC_HUFFMAN */
	ASSERT(put_bits(&os, 0, 5));	/* num_litlen_syms: 257 */
	ASSERT(put_bits(&os, 0, 5));	/* num_offset_syms: 1 */
	ASSERT(put_bits(&os, NUM_PRECODE_SYMS - 4, 4));

	for (i = 0; i < NUM_PRECODE_SYMS; i++)
		ASSERT(put_bits(&os, precode_lens[precode_perm[i]], 3));
	ASSERT(flush_bits(&os));
	/*
	 * Append enough zero bytes for 258 1-bit codeword lengths plus a body
	 * symbol so zlib reaches Z_DATA_ERROR rather than Z_BUF_ERROR when
	 * precode_lens is all zeros.
	 */
	memset(os.next, 0, 36);
	os.next += 36;

	assert_decompression_error(in, (size_t)(os.next - in));
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

/*
 * Test that overfull precode, litlen, and offset Huffman codes (where
 * codespace_used > 2^max_codeword_len) are rejected.
 */
static void
test_overfull_huffman_codes(void)
{
	u8 precode_lens[NUM_PRECODE_SYMS];
	u8 litlen_lens[257];
	u8 offset_lens[4];

	/* Overfull precode: 3 symbols of length 1 (3/2 > 1) */
	memset(precode_lens, 0, sizeof(precode_lens));
	precode_lens[0] = 1;
	precode_lens[1] = 1;
	precode_lens[2] = 1;
	assert_precode_rejected(precode_lens);

	/* Overfull precode exceeding codespace by 2^-7 (1 + 2^-7 > 1) */
	memset(precode_lens, 0, sizeof(precode_lens));
	precode_lens[0] = 1;
	precode_lens[1] = 2;
	precode_lens[2] = 2;
	precode_lens[3] = 7;
	assert_precode_rejected(precode_lens);

	/* Overfull litlen code: 3 symbols of length 1 */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	memset(offset_lens, 0, sizeof(offset_lens));
	litlen_lens[0] = 1;
	litlen_lens[1] = 1;
	litlen_lens[256] = 1;
	offset_lens[0] = 1;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);

	/* Overfull litlen code exceeding codespace by 2^-15 */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens[0] = 1;
	litlen_lens[1] = 2;
	litlen_lens[2] = 2;
	litlen_lens[256] = 15;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);

	/* Overfull offset code: 3 symbols of length 1 */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens[0] = 1;
	litlen_lens[256] = 1;
	offset_lens[0] = 1;
	offset_lens[1] = 1;
	offset_lens[2] = 1;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 3);

	/* Overfull offset code exceeding codespace by 2^-15 */
	offset_lens[0] = 1;
	offset_lens[1] = 2;
	offset_lens[2] = 2;
	offset_lens[3] = 15;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 4);
}

/*
 * Test that invalid incomplete precode, litlen, and offset Huffman codes are
 * rejected, including:
 *   - 1 codeword of length > 1 (codespace_used != 2^(max_codeword_len - 1)),
 *     both below and above LITLEN_TABLEBITS,
 *   - >= 2 codewords summing to 1/2 of the codespace (codespace_used ==
 *     2^(max_codeword_len - 1) with len_counts[1] == 0, e.g. 2 codewords of
 *     length 2 or 4 codewords of length 3), and
 *   - all-zero precode and all-zero litlen codeword lengths (which have no
 *     end-of-block symbol and must fail once input runs out).
 */
static void
test_invalid_incomplete_huffman_codes(void)
{
	u8 precode_lens[NUM_PRECODE_SYMS];
	u8 litlen_lens[257];
	u8 offset_lens[4];

	/* Precode: 1 codeword of length 2, and 1 codeword of length 7 */
	memset(precode_lens, 0, sizeof(precode_lens));
	precode_lens[0] = 2;
	assert_precode_rejected(precode_lens);
	memset(precode_lens, 0, sizeof(precode_lens));
	precode_lens[0] = 7;
	assert_precode_rejected(precode_lens);

	/* Precode: 2 codewords of length 2 (codespace 1/2, len_counts[1]==0) */
	memset(precode_lens, 0, sizeof(precode_lens));
	precode_lens[0] = 2;
	precode_lens[1] = 2;
	assert_precode_rejected(precode_lens);

	/* Precode: all codeword lengths 0 */
	memset(precode_lens, 0, sizeof(precode_lens));
	assert_precode_rejected(precode_lens);

	/* Litlen: 1 codeword of length 2, and 1 codeword of length 15 */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	memset(offset_lens, 0, sizeof(offset_lens));
	offset_lens[0] = 1;
	litlen_lens[256] = 2;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);
	litlen_lens[256] = 15;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);

	/* Litlen: 2 codewords of length 2, and 4 codewords of length 3 */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens[0] = 2;
	litlen_lens[256] = 2;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens[0] = 3;
	litlen_lens[1] = 3;
	litlen_lens[2] = 3;
	litlen_lens[256] = 3;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);

	/* Litlen: 3 codewords of length 2 (codespace 3/4) */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens[0] = 2;
	litlen_lens[1] = 2;
	litlen_lens[256] = 2;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);

	/* Litlen: all codeword lengths 0 */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);

	/* Offset: 1 codeword of length 2, and 1 codeword of length 15 */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens[0] = 1;
	litlen_lens[256] = 1;
	memset(offset_lens, 0, sizeof(offset_lens));
	offset_lens[0] = 2;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);
	offset_lens[0] = 15;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 1);

	/* Offset: 2 codewords of length 2 (codespace 1/2, len_counts[1]==0) */
	memset(offset_lens, 0, sizeof(offset_lens));
	offset_lens[0] = 2;
	offset_lens[1] = 2;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 2);

	/* Offset: 3 codewords of length 2 (codespace 3/4) */
	offset_lens[2] = 2;
	assert_dynamic_codes_rejected(litlen_lens, 257, offset_lens, 3);
}

int
tmain(int argc, tchar *argv[])
{
	begin_program(argv);

	test_too_many_codeword_lengths();
	test_overfull_huffman_codes();
	test_invalid_incomplete_huffman_codes();
	return 0;
}
