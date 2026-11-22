/*
 * test_fastloop.c
 *
 * Copyright 2026 Google LLC
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use,
 * copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following
 * conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT
 * HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY,
 * WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

/*
 * Unit tests for the DEFLATE decompressor's fastloop and related code: match
 * copying, litlen decode tables with compound (2-literal and literal+length)
 * entries, the 32 KiB split between decompress_fastloop_checked() and
 * decompress_fastloop_unchecked(), and precode RLE decoding at the end of the
 * codeword lengths array.
 */

#include "test_util.h"

#define NUM_LITLEN_SYMS		286
#define NUM_OFFSET_SYMS		30
#define NUM_PRECODE_SYMS	19
#define MAX_CODEWORD_LEN	15
#define EOB_SYM			256
#define GUARDED_BUF_SIZE	131072
/*
 * Extra input and output space for the "slack" decompression runs, so that
 * neither 'in_fastloop_end' nor 'out_fastloop_end' stops the fastloop before
 * EOB, and so that compound litlen entries (>= COMPOUND_LITLEN_MIN_OUT_BYTES ==
 * 8192) can be used in these runs regardless of the uncompressed size.
 */
#define FASTLOOP_IN_SLACK	64
#define FASTLOOP_OUT_SLACK	8192

struct guarded_buffers {
	u8 *cbuf_start;
	u8 *cbuf_end;
	u8 *dbuf_start;
	u8 *dbuf_end;
};

/* Length slots, indexed by (litlen symbol - 257) */
static const struct {
	u16 base;
	u8 extra_bits;
} length_slots[] = {
	{   3, 0 }, {   4, 0 }, {   5, 0 }, {   6, 0 },
	{   7, 0 }, {   8, 0 }, {   9, 0 }, {  10, 0 },
	{  11, 1 }, {  13, 1 }, {  15, 1 }, {  17, 1 },
	{  19, 2 }, {  23, 2 }, {  27, 2 }, {  31, 2 },
	{  35, 3 }, {  43, 3 }, {  51, 3 }, {  59, 3 },
	{  67, 4 }, {  83, 4 }, {  99, 4 }, { 115, 4 },
	{ 131, 5 }, { 163, 5 }, { 195, 5 }, { 227, 5 },
	{ 258, 0 },
};

/* Offset slots, indexed by offset symbol */
static const struct {
	u16 base;
	u8 extra_bits;
} offset_slots[] = {
	{     1,  0 }, {     2,  0 }, {     3,  0 }, {     4,  0 },
	{     5,  1 }, {     7,  1 }, {     9,  2 }, {    13,  2 },
	{    17,  3 }, {    25,  3 }, {    33,  4 }, {    49,  4 },
	{    65,  5 }, {    97,  5 }, {   129,  6 }, {   193,  6 },
	{   257,  7 }, {   385,  7 }, {   513,  8 }, {   769,  8 },
	{  1025,  9 }, {  1537,  9 }, {  2049, 10 }, {  3073, 10 },
	{  4097, 11 }, {  6145, 11 }, {  8193, 12 }, { 12289, 12 },
	{ 16385, 13 }, { 24577, 13 },
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

static void
compute_canonical_codewords(const u8 lens[], size_t num_syms, u16 codewords[])
{
	unsigned counts[MAX_CODEWORD_LEN + 1] = { 0 };
	unsigned next_code[MAX_CODEWORD_LEN + 1] = { 0 };
	unsigned code = 0;
	unsigned bits;
	size_t sym;

	for (sym = 0; sym < num_syms; sym++) {
		ASSERT(lens[sym] <= MAX_CODEWORD_LEN);
		if (lens[sym] != 0)
			counts[lens[sym]]++;
	}
	for (bits = 1; bits <= MAX_CODEWORD_LEN; bits++) {
		code = (code + counts[bits - 1]) << 1;
		next_code[bits] = code;
	}
	for (sym = 0; sym < num_syms; sym++) {
		unsigned l = lens[sym];

		if (l != 0) {
			codewords[sym] = reverse_bits((u16)next_code[l], l);
			next_code[l]++;
		} else {
			codewords[sym] = 0;
		}
	}
}

static void
init_default_lens(u8 litlen_lens[NUM_LITLEN_SYMS],
		  u8 offset_lens[NUM_OFFSET_SYMS])
{
	size_t i;

	/* 226 * 2^-8 + 60 * 2^-9 = 1 */
	for (i = 0; i < 226; i++)
		litlen_lens[i] = 8;
	for (; i < NUM_LITLEN_SYMS; i++)
		litlen_lens[i] = 9;

	/* 2 * 2^-4 + 28 * 2^-5 = 1 */
	for (i = 0; i < 2; i++)
		offset_lens[i] = 4;
	for (; i < NUM_OFFSET_SYMS; i++)
		offset_lens[i] = 5;
}

/*
 * Emit a dynamic Huffman block header using a complete 4-bit precode over
 * explicit codeword lengths 0..15 (16 * 2^-4 = 1), so any valid litlen/offset
 * length arrays can be encoded directly without RLE.
 */
static void
put_dynamic_block_header_n(struct output_bitstream *os, bool is_final,
			   const u8 *litlen_lens, size_t num_litlen_syms,
			   const u8 *offset_lens, size_t num_offset_syms)
{
	static const u8 precode_perm[NUM_PRECODE_SYMS] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	u8 precode_lens[NUM_PRECODE_SYMS] = { 0 };
	u16 precode_cws[NUM_PRECODE_SYMS];
	size_t i;

	ASSERT(num_litlen_syms >= 257 && num_litlen_syms <= 288);
	ASSERT(num_offset_syms >= 1 && num_offset_syms <= 32);

	for (i = 0; i < 16; i++)
		precode_lens[i] = 4;
	compute_canonical_codewords(precode_lens, NUM_PRECODE_SYMS,
				    precode_cws);

	ASSERT(put_bits(os, is_final ? 1 : 0, 1));
	ASSERT(put_bits(os, 2, 2)); /* BTYPE: DYNAMIC_HUFFMAN */
	ASSERT(put_bits(os, (u32)(num_litlen_syms - 257), 5));
	ASSERT(put_bits(os, (u32)(num_offset_syms - 1), 5));
	ASSERT(put_bits(os, NUM_PRECODE_SYMS - 4, 4));

	for (i = 0; i < NUM_PRECODE_SYMS; i++)
		ASSERT(put_bits(os, precode_lens[precode_perm[i]], 3));
	for (i = 0; i < num_litlen_syms; i++)
		ASSERT(put_bits(os, precode_cws[litlen_lens[i]], 4));
	for (i = 0; i < num_offset_syms; i++)
		ASSERT(put_bits(os, precode_cws[offset_lens[i]], 4));
}

static void
put_dynamic_block_header(struct output_bitstream *os, bool is_final,
			 const u8 litlen_lens[NUM_LITLEN_SYMS],
			 const u8 offset_lens[NUM_OFFSET_SYMS])
{
	put_dynamic_block_header_n(os, is_final,
				   litlen_lens, NUM_LITLEN_SYMS,
				   offset_lens, NUM_OFFSET_SYMS);
}

static void
emit_literal(struct output_bitstream *os,
	     const u8 litlen_lens[NUM_LITLEN_SYMS],
	     const u16 litlen_cws[NUM_LITLEN_SYMS],
	     u8 lit, u8 *expected_out, size_t *out_pos)
{
	ASSERT(litlen_lens[lit] != 0);
	ASSERT(put_bits(os, litlen_cws[lit], litlen_lens[lit]));
	expected_out[(*out_pos)++] = lit;
}

static void
emit_match(struct output_bitstream *os,
	   const u8 litlen_lens[NUM_LITLEN_SYMS],
	   const u16 litlen_cws[NUM_LITLEN_SYMS],
	   const u8 offset_lens[NUM_OFFSET_SYMS],
	   const u16 offset_cws[NUM_OFFSET_SYMS],
	   u32 length, u32 offset,
	   u8 *expected_out, size_t *out_pos)
{
	size_t slot_idx;
	u16 len_sym = 0, off_sym = 0;
	u32 len_extra = 0, off_extra = 0;
	u8 len_eb = 0, off_eb = 0;
	u32 k;

	ASSERT(length >= 3 && length <= 258);
	ASSERT(offset >= 1 && offset <= 32768 && offset <= *out_pos);

	for (slot_idx = ARRAY_LEN(length_slots); slot_idx-- > 0; ) {
		if (length >= length_slots[slot_idx].base) {
			len_sym = (u16)(257 + slot_idx);
			len_extra = length - length_slots[slot_idx].base;
			len_eb = length_slots[slot_idx].extra_bits;
			break;
		}
	}
	for (slot_idx = ARRAY_LEN(offset_slots); slot_idx-- > 0; ) {
		if (offset >= offset_slots[slot_idx].base) {
			off_sym = (u16)slot_idx;
			off_extra = offset - offset_slots[slot_idx].base;
			off_eb = offset_slots[slot_idx].extra_bits;
			break;
		}
	}

	ASSERT(litlen_lens[len_sym] != 0);
	ASSERT(offset_lens[off_sym] != 0);
	ASSERT(put_bits(os, litlen_cws[len_sym], litlen_lens[len_sym]));
	if (len_eb != 0)
		ASSERT(put_bits(os, len_extra, len_eb));
	ASSERT(put_bits(os, offset_cws[off_sym], offset_lens[off_sym]));
	if (off_eb != 0)
		ASSERT(put_bits(os, off_extra, off_eb));

	for (k = 0; k < length; k++) {
		expected_out[*out_pos] = expected_out[*out_pos - offset];
		(*out_pos)++;
	}
}

static void
emit_eob(struct output_bitstream *os,
	 const u8 litlen_lens[NUM_LITLEN_SYMS],
	 const u16 litlen_cws[NUM_LITLEN_SYMS])
{
	ASSERT(litlen_lens[EOB_SYM] != 0);
	ASSERT(put_bits(os, litlen_cws[EOB_SYM], litlen_lens[EOB_SYM]));
}

static void
verify_stream_zlib(const u8 *in, size_t in_nbytes,
		   const u8 *expected_out, size_t expected_out_nbytes)
{
#if __has_feature(memory_sanitizer)
	/*
	 * Skip zlib when testing with MemorySanitizer, since system zlib is
	 * normally uninstrumented and would leave 'out' marked uninitialized.
	 */
	(void)in;
	(void)in_nbytes;
	(void)expected_out;
	(void)expected_out_nbytes;
#else
	z_stream z;
	u8 *out;
	int ret;

	out = xmalloc(expected_out_nbytes ? expected_out_nbytes : 1);
	memset(&z, 0, sizeof(z));
	ret = inflateInit2(&z, -15);
	ASSERT(ret == Z_OK);

	z.next_in = (void *)in;
	z.avail_in = (uInt)in_nbytes;
	z.next_out = out;
	z.avail_out = (uInt)expected_out_nbytes;
	ret = inflate(&z, Z_FINISH);
	ASSERT(ret == Z_STREAM_END);
	ASSERT(z.avail_in == 0);
	ASSERT(z.avail_out == 0);
	ASSERT(memcmp(out, expected_out, expected_out_nbytes) == 0);

	inflateEnd(&z);
	free(out);
#endif
}

static void
verify_stream_libdeflate(struct libdeflate_decompressor *d,
			 const struct guarded_buffers *gb,
			 const u8 *in, size_t in_nbytes,
			 const u8 *expected_out, size_t expected_out_nbytes)
{
	u8 *malloc_in, *malloc_out;
	u8 *cbuf, *dbuf;
	size_t in_avail, out_avail;
	size_t actual_in, actual_out;
	enum libdeflate_result res;

	ASSERT(in_nbytes + FASTLOOP_IN_SLACK <= GUARDED_BUF_SIZE);
	ASSERT(expected_out_nbytes + FASTLOOP_OUT_SLACK <= GUARDED_BUF_SIZE);

	/* Uninitialized malloc() buffer, exact input and output sizes */
	malloc_out = xmalloc(expected_out_nbytes ? expected_out_nbytes : 1);
	actual_in = 0;
	res = libdeflate_deflate_decompress_ex(d, in, in_nbytes,
					       malloc_out, expected_out_nbytes,
					       &actual_in, NULL);
	ASSERT(res == LIBDEFLATE_SUCCESS);
	ASSERT(actual_in == in_nbytes);
	ASSERT(memcmp(malloc_out, expected_out, expected_out_nbytes) == 0);
	free(malloc_out);

	/*
	 * Uninitialized malloc() output buffer with both input and output slack
	 * (so the fastloop runs all the way through EOB).
	 */
	in_avail = in_nbytes + FASTLOOP_IN_SLACK;
	out_avail = expected_out_nbytes + FASTLOOP_OUT_SLACK;
	malloc_in = xmalloc(in_avail);
	memcpy(malloc_in, in, in_nbytes);
	memset(malloc_in + in_nbytes, 0xA5, FASTLOOP_IN_SLACK);
	malloc_out = xmalloc(out_avail);
	actual_in = actual_out = 0;
	res = libdeflate_deflate_decompress_ex(d, malloc_in, in_avail,
					       malloc_out, out_avail,
					       &actual_in, &actual_out);
	ASSERT(res == LIBDEFLATE_SUCCESS);
	ASSERT(actual_in == in_nbytes);
	ASSERT(actual_out == expected_out_nbytes);
	ASSERT(memcmp(malloc_out, expected_out, expected_out_nbytes) == 0);
	free(malloc_out);
	free(malloc_in);

	/* Guarded input & output buffers, exact sizes */
	cbuf = gb->cbuf_end - in_nbytes;
	memcpy(cbuf, in, in_nbytes);
	dbuf = gb->dbuf_end - expected_out_nbytes;
	actual_in = actual_out = 0;
	res = libdeflate_deflate_decompress_ex(d, cbuf, in_nbytes,
					       dbuf, expected_out_nbytes,
					       &actual_in, &actual_out);
	ASSERT(res == LIBDEFLATE_SUCCESS);
	ASSERT(actual_in == in_nbytes);
	ASSERT(actual_out == expected_out_nbytes);
	ASSERT(memcmp(dbuf, expected_out, expected_out_nbytes) == 0);

	/* Guarded input & output buffers with input and output slack */
	cbuf = gb->cbuf_end - in_avail;
	memcpy(cbuf, in, in_nbytes);
	memset(cbuf + in_nbytes, 0xA5, FASTLOOP_IN_SLACK);
	dbuf = gb->dbuf_end - out_avail;
	actual_in = actual_out = 0;
	res = libdeflate_deflate_decompress_ex(d, cbuf, in_avail,
					       dbuf, out_avail,
					       &actual_in, &actual_out);
	ASSERT(res == LIBDEFLATE_SUCCESS);
	ASSERT(actual_in == in_nbytes);
	ASSERT(actual_out == expected_out_nbytes);
	ASSERT(memcmp(dbuf, expected_out, expected_out_nbytes) == 0);
}

/*
 * Verify decompression of a valid DEFLATE stream against:
 *   1. zlib reference output
 *   2. Fresh uninitialized malloc() buffers (both exact sizes and with
 *      FASTLOOP_IN_SLACK / FASTLOOP_OUT_SLACK extra space so the entire block
 *      runs in the fastloop), exercising MSan/Valgrind uninitialized-memory
 *      tracking
 *   3. Guard-page-backed input and output buffers (both exact sizes and with
 *      slack) to catch any 1-byte overread or overwrite
 */
static void
verify_stream(struct libdeflate_decompressor *d,
	      const struct guarded_buffers *gb,
	      const u8 *in, size_t in_nbytes,
	      const u8 *expected_out, size_t expected_out_nbytes)
{
	verify_stream_zlib(in, in_nbytes, expected_out, expected_out_nbytes);
	verify_stream_libdeflate(d, gb, in, in_nbytes,
				 expected_out, expected_out_nbytes);
}

/*
 * Test match copying in the fastloop across short offsets (1..18),
 * vector/word boundary offsets, and boundary match lengths (3..258), starting
 * from minimal seed bytes in an uninitialized output buffer.  Also test
 * matches whose length and offset codewords require subtable lookups.
 */
static void
test_fastloop_match_copies(struct libdeflate_decompressor *d,
			   const struct guarded_buffers *gb)
{
	static const u32 test_offsets[] = {
		1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15,
		16, 17, 18, 24, 31, 32, 33, 47, 48, 49, 64,
	};
	static const u32 test_lengths[] = {
		3, 4, 5, 7, 8, 9, 15, 16, 17, 24, 31, 32, 33,
		40, 47, 48, 49, 63, 64, 65, 241, 258,
	};
	u8 litlen_lens[NUM_LITLEN_SYMS];
	u8 offset_lens[NUM_OFFSET_SYMS];
	u16 litlen_cws[NUM_LITLEN_SYMS];
	u16 offset_cws[NUM_OFFSET_SYMS];
	u8 *in_buf;
	u8 *expected_out;
	size_t oi, li, i;

	in_buf = xmalloc(GUARDED_BUF_SIZE);
	expected_out = xmalloc(GUARDED_BUF_SIZE);

	init_default_lens(litlen_lens, offset_lens);
	compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS, litlen_cws);
	compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS, offset_cws);

	for (oi = 0; oi < ARRAY_LEN(test_offsets); oi++) {
		u32 off = test_offsets[oi];
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		/*
		 * Emit 'off' seed bytes via a non-final uncompressed block
		 * (whose memcpy() writes no overrun bytes), so the first match
		 * in the following Huffman block copies from the start of the
		 * buffer without prior literal-overrun stores to out[off..].
		 */
		ASSERT(put_bits(&os, 0, 1)); /* BFINAL = 0 */
		ASSERT(put_bits(&os, 0, 2)); /* BTYPE = UNCOMPRESSED */
		ASSERT(flush_bits(&os));
		*os.next++ = (u8)off;
		*os.next++ = (u8)(off >> 8);
		*os.next++ = (u8)~off;
		*os.next++ = (u8)(~off >> 8);
		for (i = 0; i < off; i++) {
			u8 lit = (u8)((i * 53 + oi * 17 + 0xA5) & 0xFF);

			*os.next++ = lit;
			expected_out[out_pos++] = lit;
		}

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (li = 0; li < ARRAY_LEN(test_lengths); li++) {
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   test_lengths[li], off,
				   expected_out, &out_pos);
			/* Interleave 1..3 literals to vary alignment/offsets */
			for (i = 0; i < (li % 4); i++) {
				u8 lit = (u8)((li * 29 + i * 7 + 3) & 0xFF);

				emit_literal(&os, litlen_lens, litlen_cws, lit,
					     expected_out, &out_pos);
			}
		}
		/* Ensure plenty of compressed input remains for the fastloop */
		for (i = 0; i < 64; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)(i ^ 0x5A), expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * Test matches requiring both litlen subtables (len 12..15) and offset
	 * subtables (len 9..15), including max offset 32768.
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		/*
		 * 255 literals of len 8 (255/256) + 31 symbols (lit 255, EOB,
		 * and length symbols 257..285) at len 12..15:
		 * 6*2^-12 + 16*2^-13 + 7*2^-14 + 2*2^-15 = 128/32768 = 1/256.
		 */
		for (i = 0; i < 255; i++)
			litlen_lens[i] = 8;
		for (i = 255; i < 255 + 6; i++)
			litlen_lens[i] = 12;
		for (; i < 255 + 6 + 16; i++)
			litlen_lens[i] = 13;
		for (; i < 255 + 6 + 16 + 7; i++)
			litlen_lens[i] = 14;
		for (; i < NUM_LITLEN_SYMS; i++)
			litlen_lens[i] = 15;

		/*
		 * Offset symbols 0..9 at len 1..10 (1 - 1/1024), 10..21 at
		 * len 14 (12/16384), and 22..29 at len 15 (8/32768).
		 */
		for (i = 0; i < 10; i++)
			offset_lens[i] = (u8)(i + 1);
		for (; i < 22; i++)
			offset_lens[i] = 14;
		for (; i < NUM_OFFSET_SYMS; i++)
			offset_lens[i] = 15;

		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);

		/* Build 32768 bytes of history so offset 32768 is valid */
		for (i = 0; i < 64; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)((i * 73 + 19) & 0xFF),
				     expected_out, &out_pos);
		}
		while (out_pos < 32768) {
			/*
			 * Precede the subtable-offset match with 2 fast
			 * literals (16 bits) so that the 64-bit offset-subtable
			 * refill path in decompress_fastloop_body() runs.
			 */
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'B',
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   258, 64, expected_out, &out_pos);
		}
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   258, 32768, expected_out, &out_pos);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   49, 24577, expected_out, &out_pos);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   33, 4097, expected_out, &out_pos);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   19, 17, expected_out, &out_pos);
		emit_literal(&os, litlen_lens, litlen_cws, 255,
			     expected_out, &out_pos);
		for (i = 0; i < 48; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)(i & 0xFF), expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	free(in_buf);
	free(expected_out);
}

/*
 * Test double-literal decode table construction and decoding across edge-case
 * Huffman trees.
 */
static void
test_double_literal_tables(struct libdeflate_decompressor *d,
			   const struct guarded_buffers *gb)
{
	u8 litlen_lens[NUM_LITLEN_SYMS];
	u8 offset_lens[NUM_OFFSET_SYMS];
	u16 litlen_cws[NUM_LITLEN_SYMS];
	u16 offset_cws[NUM_OFFSET_SYMS];
	u8 *in_buf;
	u8 *expected_out;
	size_t i;

	in_buf = xmalloc(GUARDED_BUF_SIZE);
	expected_out = xmalloc(GUARDED_BUF_SIZE);

	/*
	 * 1. Minimal max_codeword_len == 1 code (table_bits expands 1 -> 2),
	 *    using odd literal 0x55 (bit 0 == 1, overlapping
	 *    HUFFDEC_EXCEPTIONAL), with both even and odd literal counts so
	 *    both double-literal and trailing single-literal paths run.
	 */
	for (i = 0; i < 2; i++) {
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;
		size_t count = 8300 + i;
		size_t k;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens[0x55] = 1;
		litlen_lens[EOB_SYM] = 1;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (k = 0; k < count; k++) {
			emit_literal(&os, litlen_lens, litlen_cws, 0x55,
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 2. Small max_codeword_len == 3 code (table_bits expands 3 -> 6) with
	 *    0x00 (len 1), 0xFF (len 2, odd), 0x01 (len 3, odd), EOB (len 3),
	 *    testing all 1st/2nd literal length combinations and boundary byte
	 *    values.
	 */
	{
		static const u8 syms[3] = { 0x00, 0xFF, 0x01 };
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens[0x00] = 1;
		litlen_lens[0xFF] = 2;
		litlen_lens[0x01] = 3;
		litlen_lens[EOB_SYM] = 3;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 8300; i++) {
			u8 lit = syms[(i * 5 + (i / 3)) % 3];

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 3. Skewed depth-15 tree with short literals (len 1..10), an empty
	 *    length level at len == 11 (testing fill_compound_litlen_entries()
	 *    during len_counts[11] == 0 table doubling), subtable literals at
	 *    len 12..15 (including 0xFF at len 15), and EOB at len 15.
	 */
	{
		static const u8 used_lits[] = {
			'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J',
			200, 201, 202, 203, 204, 255,
		};
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		for (i = 0; i < 10; i++)
			litlen_lens['A' + i] = (u8)(i + 1);
		/*
		 * Skip len == 11.  The remaining codespace is
		 * 1/1024 = 3*2^-12 + 2^-13 + 2^-14 + 2*2^-15.
		 */
		litlen_lens[200] = 12;
		litlen_lens[201] = 12;
		litlen_lens[202] = 12;
		litlen_lens[203] = 13;
		litlen_lens[204] = 14;
		litlen_lens[255] = 15;
		litlen_lens[EOB_SYM] = 15;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 8300; i++) {
			u8 lit = used_lits[(i * 7 + (i / 5)) %
					   ARRAY_LEN(used_lits)];

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 4. Maximum short_lits[] capacity (all 256 literals have codeword
	 *    length <= 11 == SHORT_LIT_MAX_CW_LEN):
	 *    lit 0 -> len 1, lit 1 -> len 2, lit 2 -> len 3,
	 *    lit 3..255 + EOB + syms 257..258 -> len 11
	 *    (1/2 + 1/4 + 1/8 + 256/2048 = 1).
	 *    Also exercises exact 1 + 11 == 12 (LITLEN_TABLEBITS) pairs for
	 *    both (lit, lit) and fused (lit, len).
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens[0] = 1;
		litlen_lens[1] = 2;
		litlen_lens[2] = 3;
		for (i = 3; i <= 258; i++)
			litlen_lens[i] = 11;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 2304; i++) {
			/*
			 * Pair len-1 literal 0 with every literal 0..255 in
			 * both orders: (0, lit) and (lit, 0) each use at most
			 * 1 + 11 == 12 bits, keeping every pair aligned.
			 */
			u8 lit = (u8)(i & 0xFF);

			emit_literal(&os, litlen_lens, litlen_cws, 0,
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 0,
				     expected_out, &out_pos);
		}
		/* Pair len-1 literal 0 with len-11 match syms 257 and 258 */
		for (i = 0; i < 32; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, 0,
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   (i & 1) ? 4 : 3, (i & 1) ? 2 : 1,
				   expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 5. Dynamic block with NO literals at all (only EOB and match length)
	 *    when allow_compound == true, preceded by a seed block.
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		/* Block 1 (non-final): emit 32 seed literals */
		init_default_lens(litlen_lens, offset_lens);
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		put_dynamic_block_header(&os, false, litlen_lens, offset_lens);
		for (i = 0; i < 32; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)(i * 37 + 11), expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);

		/* Block 2 (final): no literals, just EOB and len 4 (sym 258) */
		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens[EOB_SYM] = 1;
		litlen_lens[258] = 1;
		offset_lens[6] = 1;
		offset_lens[7] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);
		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 2100; i++) {
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   4, 16, expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 6. Dynamic block where a match-length symbol (sym 257, len 1) is
	 *    shorter than the shortest literal ('A', len 2), testing that
	 *    min_lit_cw_len skips non-literal lengths.
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		init_default_lens(litlen_lens, offset_lens);
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		put_dynamic_block_header(&os, false, litlen_lens, offset_lens);
		for (i = 0; i < 16; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)('A' + (i % 5)),
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens[257] = 1;
		litlen_lens['A'] = 2;
		litlen_lens['B'] = 3;
		litlen_lens[EOB_SYM] = 3;
		offset_lens[0] = 1;
		offset_lens[2] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);
		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 1400; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'B',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   3, 3, expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 7. Reduced table_bits: literals at lengths {1, 5, 9}, fusable length
	 *    symbol 257 at length 4, and non-fusable length symbols 281..285
	 *    (> 280) at lengths 3, 6, 7, 8, 9.  The longest compound pair that
	 *    fits in 12 bits is 1+9 == 5+5 == 9+1 == 10 bits, so table_bits is
	 *    reduced to 10.  ('C' followed by 257 would need 9+4 == 13 bits, so
	 *    it must be decoded as two separate symbols.)
	 *    512 * sum = 256 + 128 + 64 + 32 + 16 + 8 + 4 + 2 + 1 + 1 = 512.
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens['A'] = 1;
		litlen_lens[EOB_SYM] = 2;
		litlen_lens[281] = 3;
		litlen_lens[257] = 4;
		litlen_lens['B'] = 5;
		litlen_lens[282] = 6;
		litlen_lens[283] = 7;
		litlen_lens[284] = 8;
		litlen_lens['C'] = 9;
		litlen_lens[285] = 9;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 1400; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'C',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'B',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'B',
				     expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'C',
				     expected_out, &out_pos);
			if (i % 10 == 0) {
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   3, 2, expected_out, &out_pos);
			}
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 8. Tightened table_bits with fused (lit, len) expansion: single
	 *    literal 'A' at len 2 with short lengths up to len 6 (longest
	 *    compound pair is 'A'+261 = 2+6 = 8, so table_bits == 8, NOT
	 *    LITLEN_TABLEBITS == 12).
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens[257] = 1;
		litlen_lens['A'] = 2;
		litlen_lens[258] = 3;
		litlen_lens[259] = 4;
		litlen_lens[260] = 5;
		litlen_lens[EOB_SYM] = 6;
		litlen_lens[261] = 6;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		/*
		 * Alternate 1 and 2 'A' literals before each match (since
		 * gcd(2, 5) == 1, both fused ('A', 257..261) and ('A', 'A') +
		 * plain 257..261 are exercised for all 5 match symbols).
		 */
		for (i = 0; i < 1800; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			if (i & 1) {
				emit_literal(&os, litlen_lens, litlen_cws, 'A',
					     expected_out, &out_pos);
			}
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   (u32)(3 + (i % 5)), 1,
				   expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 9. Switch within a single stream between a dynamic double-literal
	 *    block, two consecutive static Huffman blocks (exercising
	 *    static_codes_loaded), and another dynamic double-literal block.
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;
		size_t b;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens['X'] = 1;
		litlen_lens['Y'] = 2;
		litlen_lens[EOB_SYM] = 2;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, false, litlen_lens, offset_lens);
		for (i = 0; i < 8300; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (i & 1) ? 'Y' : 'X',
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);

		/* Two consecutive static blocks (reusing the static tables) */
		for (b = 0; b < 2; b++) {
			ASSERT(put_bits(&os, 0, 1)); /* BFINAL = 0 */
			ASSERT(put_bits(&os, 1, 2)); /* BTYPE = STATIC */
			for (i = 0; i < 400; i++) {
				u8 ch = (u8)('A' + ((i + b) % 16));
				/* Static literals 0..143: cw = 0x30 + sym */
				u16 cw = reverse_bits((u16)(0x30 + ch), 8);

				ASSERT(put_bits(&os, cw, 8));
				expected_out[out_pos++] = ch;
			}
			/* Static EOB (sym 256): 7 zero bits */
			ASSERT(put_bits(&os, 0, 7));
		}

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 8300; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (i % 3 == 0) ? 'Y' : 'X',
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 10. Below COMPOUND_LITLEN_MIN_OUT_BYTES (exact-size run takes fastloop
	 *     without double literals; slack run takes double literals).
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens['A'] = 1;
		litlen_lens['B'] = 2;
		litlen_lens[EOB_SYM] = 2;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 4000; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (i % 3 == 0) ? 'B' : 'A',
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 11. Compound table with max_codeword_len == 11 (< LITLEN_TABLEBITS)
	 *     where min_second_cw_len == 3 and literals exist at lengths 3, 4,
	 *     10, and 11 (7*2^-3 + 2^-4 + 63*2^-10 + 2*2^-11 == 1), exercising
	 *     the 'l1 + min_second_cw_len > table_bits' early break in
	 *     build_decode_table() (10 + 3 == 13 > 12).
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		for (i = 0; i < 6; i++)
			litlen_lens['A' + i] = 3;
		litlen_lens[EOB_SYM] = 3;
		litlen_lens['G'] = 4;
		for (i = 128; i < 128 + 63; i++)
			litlen_lens[i] = 10;
		litlen_lens[191] = 11;
		litlen_lens[192] = 11;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 8400; i++) {
			u8 lit = (i % 13 == 0) ? (u8)(128 + (i % 65)) :
				 (u8)('A' + (i % 7));

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	free(in_buf);
	free(expected_out);
}

/*
 * Test generic-loop LIBDEFLATE_INSUFFICIENT_SPACE when only 1 output byte is
 * available for a 2-literal entry, and test overread termination when a stream
 * with an all-zeroes double-literal codeword runs out of input after exiting
 * the fastloop.
 */
static void
test_double_literal_edge_conditions(struct libdeflate_decompressor *d,
				    const struct guarded_buffers *gb)
{
	u8 litlen_lens[NUM_LITLEN_SYMS] = { 0 };
	u8 offset_lens[NUM_OFFSET_SYMS] = { 0 };
	u16 litlen_cws[NUM_LITLEN_SYMS];
	u8 *in_buf;
	u8 *expected_out;
	struct output_bitstream os;
	size_t out_pos = 0;
	size_t csize;
	size_t i, delta;
	u8 *cbuf, *dbuf;
	enum libdeflate_result res;

	in_buf = xmalloc(GUARDED_BUF_SIZE);
	expected_out = xmalloc(GUARDED_BUF_SIZE);

	/*
	 * Build a stream ending in consecutive short literals so the generic
	 * loop decodes double-literal entries right up to out_end.
	 */
	litlen_lens['A'] = 1;
	litlen_lens['B'] = 2;
	litlen_lens[EOB_SYM] = 2;
	offset_lens[0] = 1;
	offset_lens[1] = 1;
	compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS, litlen_cws);

	memset(&os, 0, sizeof(os));
	os.next = in_buf;
	os.end = in_buf + GUARDED_BUF_SIZE;
	put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
	for (i = 0; i < 8300; i++) {
		u8 lit = (i & 1) ? 'B' : 'A';

		emit_literal(&os, litlen_lens, litlen_cws, lit,
			     expected_out, &out_pos);
	}
	emit_eob(&os, litlen_lens, litlen_cws);
	ASSERT(flush_bits(&os));
	csize = (size_t)(os.next - in_buf);

	verify_stream(d, gb, in_buf, csize, expected_out, out_pos);

	cbuf = gb->cbuf_end - csize;
	memcpy(cbuf, in_buf, csize);

	for (delta = 1; delta <= 8; delta++) {
		size_t avail = out_pos - delta;
		size_t actual_out = 0;

		dbuf = gb->dbuf_end - avail;
		res = libdeflate_deflate_decompress(d, cbuf, csize,
						    dbuf, avail, &actual_out);
		ASSERT(res == LIBDEFLATE_INSUFFICIENT_SPACE);
	}
	dbuf = gb->dbuf_end - (out_pos + 1);
	res = libdeflate_deflate_decompress(d, cbuf, csize,
					    dbuf, out_pos + 1, NULL);
	ASSERT(res == LIBDEFLATE_SHORT_OUTPUT);

	/*
	 * Truncated stream where literal 0x00 has codeword 0 (so 0b00 is a
	 * double-literal entry), large enough to enter the fastloop,
	 * then run out of input in the generic loop before hitting EOB.
	 */
	memset(&os, 0, sizeof(os));
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens[0] = 1;
	litlen_lens[EOB_SYM] = 1;
	compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS, litlen_cws);
	ASSERT(litlen_cws[0] == 0);

	os.next = in_buf;
	os.end = in_buf + GUARDED_BUF_SIZE;
	put_dynamic_block_header(&os, false, litlen_lens, offset_lens);
	ASSERT(flush_bits(&os));
	memset(os.next, 0, 64);
	csize = (size_t)(os.next - in_buf) + 64;

	cbuf = gb->cbuf_end - csize;
	memcpy(cbuf, in_buf, csize);
	dbuf = gb->dbuf_end - 16384;
	res = libdeflate_deflate_decompress(d, cbuf, csize, dbuf, 16384, NULL);
	ASSERT(res == LIBDEFLATE_BAD_DATA);

	/*
	 * Tight FASTLOOP_MAX_BYTES_WRITTEN boundary test: 2 double-literal
	 * lookups (4 literals) immediately followed by a length-258 or
	 * length-241 match right against the guard page.
	 */
	{
		static const u32 offs[] = { 1, 2, 7, 8, 15, 16, 17, 32 };
		static const u32 lens[] = { 241, 258 };
		u16 offset_cws[NUM_OFFSET_SYMS];
		size_t oi, li;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens['A'] = 1;
		litlen_lens['B'] = 3;
		litlen_lens[284] = 3; /* len base 227 + 5 extra bits (241) */
		litlen_lens[285] = 3; /* len 258 */
		litlen_lens[EOB_SYM] = 3;
		offset_lens[0] = 3;  /* off 1 */
		offset_lens[1] = 3;  /* off 2 */
		offset_lens[5] = 3;  /* off 7..8 */
		offset_lens[7] = 3;  /* off 13..16 */
		offset_lens[8] = 2;  /* off 17..24 */
		offset_lens[9] = 2;  /* off 25..32 */
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		for (oi = 0; oi < ARRAY_LEN(offs); oi++) {
			for (li = 0; li < ARRAY_LEN(lens); li++) {
				size_t extra;

				memset(&os, 0, sizeof(os));
				os.next = in_buf;
				os.end = in_buf + GUARDED_BUF_SIZE;
				out_pos = 0;
				put_dynamic_block_header(&os, false,
							 litlen_lens,
							 offset_lens);
				for (i = 0; i < 32; i++) {
					emit_literal(&os, litlen_lens,
						     litlen_cws,
						     (i & 1) ? 'B' : 'A',
						     expected_out, &out_pos);
				}
				/* Cross COMPOUND_LITLEN_MIN_OUT_BYTES (8192 B) */
				for (i = 0; i < 36; i++) {
					size_t k;

					for (k = 0; k < 4; k++) {
						emit_literal(&os, litlen_lens,
							     litlen_cws, 'A',
							     expected_out,
							     &out_pos);
					}
					emit_match(&os, litlen_lens, litlen_cws,
						   offset_lens, offset_cws,
						   lens[li], offs[oi],
						   expected_out, &out_pos);
				}
				emit_eob(&os, litlen_lens, litlen_cws);
				/*
				 * Append empty uncompressed blocks so enough
				 * input bytes follow the last match for the
				 * fastloop to stay active right up to the exact
				 * FASTLOOP_MAX_BYTES_WRITTEN output boundary.
				 */
				for (i = 0; i < 6; i++) {
					bool last = (i == 5);

					ASSERT(put_bits(&os, last ? 1 : 0, 1));
					ASSERT(put_bits(&os, 0, 2));
					ASSERT(flush_bits(&os));
					ASSERT(os.end - os.next >= 4);
					*os.next++ = 0x00;
					*os.next++ = 0x00;
					*os.next++ = 0xFF;
					*os.next++ = 0xFF;
				}
				csize = (size_t)(os.next - in_buf);
				verify_stream_zlib(in_buf, csize, expected_out,
						   out_pos);
				cbuf = gb->cbuf_end - csize;
				memcpy(cbuf, in_buf, csize);

				for (extra = 0; extra <= 48; extra++) {
					size_t avail = out_pos + extra;
					size_t aout = 0;

					dbuf = gb->dbuf_end - avail;
					res = libdeflate_deflate_decompress(
							d, cbuf, csize,
							dbuf, avail, &aout);
					ASSERT(res == LIBDEFLATE_SUCCESS);
					ASSERT(aout == out_pos);
					ASSERT(memcmp(dbuf, expected_out,
						      out_pos) == 0);
				}
			}
		}

		/*
		 * Invalid match offset right after 2 double-literal lookups
		 * (4 literals written, offset 7 > 4 -> LIBDEFLATE_BAD_DATA).
		 */
		memset(&os, 0, sizeof(os));
		os.next = in_buf;
		os.end = in_buf + GUARDED_BUF_SIZE;
		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 4; i++)
			ASSERT(put_bits(&os, litlen_cws['A'],
					litlen_lens['A']));
		ASSERT(put_bits(&os, litlen_cws[285], litlen_lens[285]));
		ASSERT(put_bits(&os, offset_cws[5], offset_lens[5]));
		ASSERT(put_bits(&os, 0, 1)); /* offset = 7 > 4 */
		for (i = 0; i < 128; i++)
			ASSERT(put_bits(&os, litlen_cws['B'],
					litlen_lens['B']));
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));
		csize = (size_t)(os.next - in_buf);
		cbuf = gb->cbuf_end - csize;
		memcpy(cbuf, in_buf, csize);
		dbuf = gb->dbuf_end - 16384;
		res = libdeflate_deflate_decompress(d, cbuf, csize, dbuf, 16384,
						    NULL);
		ASSERT(res == LIBDEFLATE_BAD_DATA);
	}

	free(in_buf);
	free(expected_out);
}

/*
 * Test RLE precode symbol decoding (presyms 16, 17, 18) right at the end of the
 * codeword lengths, with the maximum 288 + 32 lengths.  In particular, this
 * exercises the unconditional memset(&d->u.l.lens[i], 0, 138) for presym 18
 * at its worst-case starting index (319), which writes up to the very end of
 * d->u.l.lens[288 + 32 + 137].
 */
static void
test_precode_rle_overrun(struct libdeflate_decompressor *d,
			 const struct guarded_buffers *gb)
{
	static const u8 precode_perm[NUM_PRECODE_SYMS] = {
		16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
	};
	u8 precode_lens[NUM_PRECODE_SYMS];
	u16 precode_cws[NUM_PRECODE_SYMS];
	u8 *in_buf;
	u8 *expected_out;
	struct output_bitstream os;
	size_t i;
	size_t csize;
	size_t actual_out;
	u8 *cbuf, *dbuf;
	int mode;

	/* Complete precode over all 19 symbols: 13 * 2^-4 + 6 * 2^-5 = 1 */
	for (i = 0; i < 13; i++)
		precode_lens[i] = 4;
	for (; i < NUM_PRECODE_SYMS; i++)
		precode_lens[i] = 5;
	compute_canonical_codewords(precode_lens, NUM_PRECODE_SYMS,
				    precode_cws);

	in_buf = xmalloc(GUARDED_BUF_SIZE);
	expected_out = xmalloc(GUARDED_BUF_SIZE);

	/*
	 * mode 0: valid header ending with presym 18 (11 zeroes at i == 309)
	 * mode 1: valid header ending with presym 17 (3 zeroes at i == 317)
	 * mode 2: valid header ending with presym 16 (3 copies at i == 317)
	 * mode 3: invalid, presym 18 (11 zeroes) overflowing at i == 319
	 * mode 4: invalid, presym 18 (138 zeroes) overflowing at i == 319
	 */
	for (mode = 0; mode < 5; mode++) {
		memset(&os, 0, sizeof(os));
		os.next = in_buf;
		os.end = in_buf + GUARDED_BUF_SIZE;

		/* Dynamic block with maximum HLIT=31 (288), HDIST=31 (32) */
		ASSERT(put_bits(&os, 1, 1));
		ASSERT(put_bits(&os, 2, 2));
		ASSERT(put_bits(&os, 288 - 257, 5));
		ASSERT(put_bits(&os, 32 - 1, 5));
		ASSERT(put_bits(&os, NUM_PRECODE_SYMS - 4, 4));
		for (i = 0; i < NUM_PRECODE_SYMS; i++)
			ASSERT(put_bits(&os, precode_lens[precode_perm[i]], 3));

		/* litlen 0..64 = 0 (65 zeroes) */
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 65 - 11, 7));
		/* litlen['A'=65] = 1 */
		ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
		/* litlen 66..255 = 0 (190 zeroes: 138 + 52) */
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 138 - 11, 7));
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 52 - 11, 7));
		/* litlen[256=EOB] = 1 */
		ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
		/* litlen 257..287 = 0 (31 zeroes) */
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 31 - 11, 7));

		/* 32 offset lengths: indices 288..319 */
		if (mode == 2) {
			/* 32 * 2^-5 = 1, ending with presym 16 (3 copies) */
			for (i = 0; i < 29; i++)
				ASSERT(put_bits(&os, precode_cws[5],
						precode_lens[5]));
			ASSERT(put_bits(&os, precode_cws[16],
					precode_lens[16]));
			ASSERT(put_bits(&os, 3 - 3, 2)); /* 317..319 */
		} else {
			/* offset 0..1 = 1 */
			ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
			ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
			if (mode == 0) {
				/* 290..308 */
				ASSERT(put_bits(&os, precode_cws[18],
						precode_lens[18]));
				ASSERT(put_bits(&os, 19 - 11, 7));
				/* 309..319 */
				ASSERT(put_bits(&os, precode_cws[18],
						precode_lens[18]));
				ASSERT(put_bits(&os, 11 - 11, 7));
			} else if (mode == 1) {
				/* 290..316 */
				ASSERT(put_bits(&os, precode_cws[18],
						precode_lens[18]));
				ASSERT(put_bits(&os, 27 - 11, 7));
				ASSERT(put_bits(&os, precode_cws[17],
						precode_lens[17]));
				ASSERT(put_bits(&os, 3 - 3, 3)); /* 317..319 */
			} else {
				/* 290..318, then presym 18 at i == 319 */
				ASSERT(put_bits(&os, precode_cws[18],
						precode_lens[18]));
				ASSERT(put_bits(&os, 29 - 11, 7));
				ASSERT(put_bits(&os, precode_cws[18],
						precode_lens[18]));
				ASSERT(put_bits(&os, (mode == 3) ? 0 : 127, 7));
			}
		}

		/* Body: 16 x 'A' (cw=0, len=1) + EOB (cw=1, len=1) */
		for (i = 0; i < 16; i++) {
			ASSERT(put_bits(&os, 0, 1));
			expected_out[i] = 'A';
		}
		ASSERT(put_bits(&os, 1, 1));
		ASSERT(flush_bits(&os));
		csize = (size_t)(os.next - in_buf);

		if (mode < 3) {
			/*
			 * zlib strictly rejects HLIT > 29 (>286) and
			 * HDIST > 29 (>30), whereas libdeflate allows up to
			 * 288 and 32.  Skip zlib for these boundary headers.
			 */
			verify_stream_libdeflate(d, gb, in_buf, csize,
						 expected_out, 16);
		} else {
			cbuf = gb->cbuf_end - csize;
			memcpy(cbuf, in_buf, csize);
			dbuf = gb->dbuf_end - 64;
			ASSERT(libdeflate_deflate_decompress(
					d, cbuf, csize, dbuf, 64,
					&actual_out) == LIBDEFLATE_BAD_DATA);
		}
	}

	/* Also verify RFC-valid max header (286 litlen, 30 offset) vs zlib */
	for (mode = 0; mode < 2; mode++) {
		memset(&os, 0, sizeof(os));
		os.next = in_buf;
		os.end = in_buf + GUARDED_BUF_SIZE;

		ASSERT(put_bits(&os, 1, 1));
		ASSERT(put_bits(&os, 2, 2));
		ASSERT(put_bits(&os, 286 - 257, 5));
		ASSERT(put_bits(&os, 30 - 1, 5));
		ASSERT(put_bits(&os, NUM_PRECODE_SYMS - 4, 4));
		for (i = 0; i < NUM_PRECODE_SYMS; i++)
			ASSERT(put_bits(&os, precode_lens[precode_perm[i]], 3));

		/* litlen 0..64=0, 'A'=1, 66..255=0, EOB=1, 257..285=0 */
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 65 - 11, 7));
		ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 138 - 11, 7));
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 52 - 11, 7));
		ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
		ASSERT(put_bits(&os, precode_cws[18], precode_lens[18]));
		ASSERT(put_bits(&os, 29 - 11, 7));

		/* offset 0..1 = 1, then 28 zeroes ending in presym 18 or 17 */
		ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
		ASSERT(put_bits(&os, precode_cws[1], precode_lens[1]));
		if (mode == 0) {
			ASSERT(put_bits(&os, precode_cws[18],
					precode_lens[18]));
			ASSERT(put_bits(&os, 17 - 11, 7));
			ASSERT(put_bits(&os, precode_cws[18],
					precode_lens[18]));
			ASSERT(put_bits(&os, 11 - 11, 7));
		} else {
			ASSERT(put_bits(&os, precode_cws[18],
					precode_lens[18]));
			ASSERT(put_bits(&os, 25 - 11, 7));
			ASSERT(put_bits(&os, precode_cws[17],
					precode_lens[17]));
			ASSERT(put_bits(&os, 3 - 3, 3));
		}

		for (i = 0; i < 16; i++) {
			ASSERT(put_bits(&os, 0, 1));
			expected_out[i] = 'A';
		}
		ASSERT(put_bits(&os, 1, 1));
		ASSERT(flush_bits(&os));
		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, 16);
	}

	free(in_buf);
	free(expected_out);
}

/*
 * Test adaptive 12-bit and 11-bit litlen tables and worst-case ENOUGH sizes:
 *  1. 6+6 == 12 double-literal boundary (table_bits expands 6 -> 12).
 *  2. Empty codeword length level at len == 12 with subtables at len 13..15.
 *  3. Worst-case enough(288, 12, 15) == 4382 tree, using the very last
 *     entry of d->u.litlen_decode_table[] (LITLEN_ENOUGH == 4382).
 *  4. Empty codeword length level at len == 13 (> table_bits == 12) with
 *     subtables at len 14..15.
 *  5. Below COMPOUND_LITLEN_MIN_OUT_BYTES (< 8192 bytes) with worst-case
 *     enough(288, 11, 15) == 2342 litlen tree and worst-case
 *     enough(32, 8, 15) == 402 offset tree, exercising allow_compound == false
 *     with 11-bit subtables as well as OFFSET_ENOUGH == 402.
 */
static void
test_12bit_and_enough_tables(struct libdeflate_decompressor *d,
			     const struct guarded_buffers *gb)
{
	u8 litlen_lens[288];
	u8 offset_lens[32];
	u16 litlen_cws[288];
	u16 offset_cws[32];
	u8 *in_buf;
	u8 *expected_out;
	size_t i;

	in_buf = xmalloc(GUARDED_BUF_SIZE);
	expected_out = xmalloc(GUARDED_BUF_SIZE);

	/* 1. 63 literals + EOB all of len 6 (64 * 2^-6 = 1), 6+6 == 12. */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		for (i = 0; i < 63; i++)
			litlen_lens[i] = 6;
		litlen_lens[EOB_SYM] = 6;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 8400; i++) {
			u8 lit = (u8)((i * 19 + (i / 7)) % 63);

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));
		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 2. Empty level at len == 12 with subtables at len 13..15:
	 *    len 1..11 (1 symbol each, leaving 2^-11 = 16 * 2^-15),
	 *    len 12: 0 symbols,
	 *    len 13: 1 symbol (4 * 2^-15),
	 *    len 14: 3 symbols (6 * 2^-15),
	 *    len 15: 6 symbols (6 * 2^-15).
	 */
	{
		static const u8 used_lits[] = {
			'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
			200, 201, 202, 203, 204, 205, 206, 207, 255,
		};
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		for (i = 0; i < 11; i++)
			litlen_lens['A' + i] = (u8)(i + 1);
		litlen_lens[200] = 13;
		litlen_lens[201] = 14;
		litlen_lens[202] = 14;
		litlen_lens[203] = 14;
		litlen_lens[204] = 15;
		litlen_lens[205] = 15;
		litlen_lens[206] = 15;
		litlen_lens[207] = 15;
		litlen_lens[255] = 15;
		litlen_lens[EOB_SYM] = 15;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 8400; i++) {
			u8 lit = used_lits[(i * 11 + (i / 3)) %
					   ARRAY_LEN(used_lits)];

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));
		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 3. Worst-case enough(288, 12, 15) == 4382 (4096 main + 286 subtable):
	 *    syms 0..5 at len 1..6 (6 syms = 4032/4096),
	 *    syms 6..8 at len 13 (3 syms),
	 *    syms 9..229 at len 14 (221 syms),
	 *    syms 230..287 at len 15 (58 syms, including EOB=256 and matches).
	 *    Skip zlib since HLIT > 29 (288 > 286).
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		for (i = 0; i < 6; i++)
			litlen_lens[i] = (u8)(i + 1);
		for (; i < 9; i++)
			litlen_lens[i] = 13;
		for (; i < 230; i++)
			litlen_lens[i] = 14;
		for (; i < 288; i++)
			litlen_lens[i] = 15;

		memset(offset_lens, 0, sizeof(offset_lens));
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, 288, litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header_n(&os, true, litlen_lens, 288,
					   offset_lens, NUM_OFFSET_SYMS);
		/* Emit all 256 literal values including subtable entries */
		for (i = 0; i < 256; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, (u8)i,
				     expected_out, &out_pos);
		}
		/*
		 * Matches whose length symbols require subtables (len 15),
		 * well before the end of the block so they run in the fastloop
		 * even without input/output slack.
		 */
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   3, 1, expected_out, &out_pos);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   258, 2, expected_out, &out_pos);
		/* Exceed COMPOUND_LITLEN_MIN_OUT_BYTES (8192) */
		for (i = 0; i < 8200; i++) {
			u8 lit = (i % 17 == 0) ? (u8)(6 + (i % 250)) :
						 (u8)(i % 6);

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		/* Matches whose length symbols require subtables (len 15) */
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   3, 1, expected_out, &out_pos);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   258, 2, expected_out, &out_pos);
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream_libdeflate(d, gb, in_buf,
					 (size_t)(os.next - in_buf),
					 expected_out, out_pos);
	}

	/*
	 * 4. Empty level at len == 13 (strictly above table_bits == 12),
	 *    after a non-empty level at len == 12:
	 *    len 1..11: 1 symbol each (leaving 2^-11 = 16 * 2^-15),
	 *    len 12: 1 symbol (8 * 2^-15),
	 *    len 13: 0 symbols,
	 *    len 14: 2 symbols (4 * 2^-15),
	 *    len 15: 4 symbols (4 * 2^-15).
	 */
	{
		static const u8 used_lits[] = {
			'A', 'B', 'C', 'D', 'E', 'F', 'G', 'H', 'I', 'J', 'K',
			200, 201, 202, 203, 204, 255,
		};
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		for (i = 0; i < 11; i++)
			litlen_lens['A' + i] = (u8)(i + 1);
		litlen_lens[200] = 12;
		litlen_lens[201] = 14;
		litlen_lens[202] = 14;
		litlen_lens[203] = 15;
		litlen_lens[204] = 15;
		litlen_lens[255] = 15;
		litlen_lens[EOB_SYM] = 15;
		offset_lens[0] = 1;
		offset_lens[1] = 1;
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 8400; i++) {
			u8 lit = used_lits[(i * 11 + (i / 3)) %
					   ARRAY_LEN(used_lits)];

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));
		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 5. Worst-case enough(288, 11, 15) == 2342 (2048 main + 294 subtable)
	 *    litlen tree paired with worst-case enough(32, 8, 15) == 402
	 *    (256 main + 146 subtable) offset tree, with output size <
	 *    COMPOUND_LITLEN_MIN_OUT_BYTES (8192) so the exact-size runs
	 *    exercise allow_compound == false with 11-bit subtables while the
	 *    slack runs exercise allow_compound == true.
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		/*
		 * Litlen: [1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 3, 221, 57, 2].
		 * Place EOB (256) and length 258 (285) at len 15 so symbol 285
		 * occupies the very last subtable slot (index 2341).
		 */
		for (i = 0; i < 5; i++)
			litlen_lens[i] = (u8)(i + 1);
		for (; i < 8; i++)
			litlen_lens[i] = 12;
		for (; i < 229; i++)
			litlen_lens[i] = 13;
		for (; i < 288; i++)
			litlen_lens[i] = 14;
		litlen_lens[EOB_SYM] = 15;
		litlen_lens[285] = 15;

		/*
		 * Offset: [1, 1, 1, 1, 1, 1, 0, 0, 3, 1, 17, 1, 1, 1, 2].
		 * Place offset symbols 2 (offset 3) and 3 (offset 4) at len 15
		 * so symbol 3 occupies offset_decode_table[401].
		 */
		offset_lens[0] = 13;
		offset_lens[1] = 14;
		offset_lens[2] = 15;
		offset_lens[3] = 15;
		for (i = 4; i < 10; i++)
			offset_lens[i] = (u8)(i - 3);
		for (; i < 13; i++)
			offset_lens[i] = 9;
		offset_lens[13] = 10;
		for (i = 14; i < 31; i++)
			offset_lens[i] = 11;
		offset_lens[31] = 12;

		compute_canonical_codewords(litlen_lens, 288, litlen_cws);
		compute_canonical_codewords(offset_lens, 32, offset_cws);

		put_dynamic_block_header_n(&os, true, litlen_lens, 288,
					   offset_lens, 32);
		for (i = 0; i < 256; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, (u8)i,
				     expected_out, &out_pos);
		}
		for (i = 0; i < 4; i++) {
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   258, (u32)(i + 1), expected_out, &out_pos);
		}
		for (i = 0; i < 2000; i++) {
			u8 lit = (i % 9 == 0) ? (u8)(5 + (i % 251)) :
						(u8)(i % 5);

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   258, 4, expected_out, &out_pos);
		ASSERT(out_pos < 8192);
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream_libdeflate(d, gb, in_buf,
					 (size_t)(os.next - in_buf),
					 expected_out, out_pos);
	}

	free(in_buf);
	free(expected_out);
}

/*
 * Test fused (literal, length) decode table entries (HUFFDEC_LIT_LENGTH):
 *  1. All 24 fusable length symbols (257..280: lengths 3..130, 0..4 extra
 *     bits) preceded by even/odd/high-bit literals (0x00, 0x55, 0xFF, 'A',
 *     'B', 'C'), plus non-fusable symbols 281..285 (lengths 131..258).
 *  2. (lit, len)-only tree where 2 * min_lit_cw_len > 12 (no double literals).
 *  3. A fused (lit, len) entry at the start of the output followed by
 *     offset == 1 (copying the fused literal), and invalid offset == 2 in the
 *     same position.
 *  4. Generic-loop INSUFFICIENT_SPACE / SHORT_OUTPUT exact boundaries on fused
 *     (lit, sym 280 len 130) entry, and guard-page fastloop overrun check after
 *     2 double-literal lookups + fused (lit, sym 280 len 130).
 *  5. Fused (lit, len) entry after 14 uncompressed seed bytes with offset == 15
 *     (leaving src[15] uninitialized when COPY_MATCH loads 16 bytes from src).
 *  6. Double-literal compound tree where fusable short length symbols exist at
 *     len <= 11, yet no fused (lit, len) pair fits in LITLEN_TABLEBITS.
 */
static void
test_litlen_fusion(struct libdeflate_decompressor *d,
		   const struct guarded_buffers *gb)
{
	u8 litlen_lens[NUM_LITLEN_SYMS];
	u8 offset_lens[NUM_OFFSET_SYMS];
	u16 litlen_cws[NUM_LITLEN_SYMS];
	u16 offset_cws[NUM_OFFSET_SYMS];
	u8 *in_buf;
	u8 *expected_out;
	size_t i;

	in_buf = xmalloc(GUARDED_BUF_SIZE);
	expected_out = xmalloc(GUARDED_BUF_SIZE);

	/*
	 * 1. All 24 fusable length syms 257..280 at len 6, plus non-fused
	 *    281..285 at len 5.  Also tests 6+6 == 12 fused (lit, len) pairs
	 *    ('C'/'D' + 257..280) and bit-30/high-bit literal 0xFF.
	 *    Sum*64: 8('A':3) + 4*4(0,0xFF,0x55,'B':4) + 24*1(257..280:6) +
	 *            7*2(EOB,281..285,'E':5) + 2*1('C','D':6) = 64.
	 */
	{
		static const u8 lits[] = {
			'A', 0x00, 0x55, 0xFF, 'B', 'C', 'D', 'E'
		};
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;
		size_t s;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens['A'] = 3;
		litlen_lens[0x00] = 4;
		litlen_lens[0x55] = 4;
		litlen_lens[0xFF] = 4;
		litlen_lens['B'] = 4;
		litlen_lens['E'] = 5;
		litlen_lens['C'] = 6;
		litlen_lens['D'] = 6;
		litlen_lens[EOB_SYM] = 5;
		for (s = 257; s <= 280; s++)
			litlen_lens[s] = 6;
		for (s = 281; s <= 285; s++)
			litlen_lens[s] = 5;

		offset_lens[0] = 2; /* off 1 */
		offset_lens[1] = 2; /* off 2 */
		offset_lens[2] = 2; /* off 3 */
		offset_lens[7] = 2; /* off 13..16 */
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);

		/*
		 * Immediately emit a single literal + offset==1 match at
		 * position 0 (exercises fused lit updating out_next before
		 * offset check).
		 */
		emit_literal(&os, litlen_lens, litlen_cws, 0xFF,
			     expected_out, &out_pos);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   10, 1, expected_out, &out_pos);

		/* Seed >= 16 varied bytes */
		for (i = 0; i < 24; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     lits[i % ARRAY_LEN(lits)],
				     expected_out, &out_pos);
		}

		/* Exercise every fusable length 3..130 (257..280) and extras */
		for (i = 3; i <= 130; i++) {
			u8 lit = lits[i % ARRAY_LEN(lits)];
			u32 off = (i % 4 == 0) ? 1 : (i % 4 == 1) ? 2 :
				  (i % 4 == 2) ? 3 : 16;

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   (u32)i, off, expected_out, &out_pos);
		}
		/* Exercise non-fusable boundary lengths 131..258 (281..285) */
		{
			static const u32 big_lens[] = {
				131, 132, 162, 163, 194, 195,
				226, 227, 257, 258
			};
			for (i = 0; i < ARRAY_LEN(big_lens); i++) {
				u8 lit = lits[(i + 3) % ARRAY_LEN(lits)];

				emit_literal(&os, litlen_lens, litlen_cws, lit,
					     expected_out, &out_pos);
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   big_lens[i], (i & 1) ? 16 : 3,
					   expected_out, &out_pos);
			}
		}
		ASSERT(out_pos >= 8192);
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);

		/*
		 * Generic-loop trailing exact-space checks on a fused
		 * (literal, len 130 / sym 280) entry ending the stream.
		 */
		{
			size_t csize, delta;
			u8 *cbuf, *dbuf;
			enum libdeflate_result res;

			memset(&os, 0, sizeof(os));
			os.next = in_buf;
			os.end = in_buf + GUARDED_BUF_SIZE;
			out_pos = 0;
			put_dynamic_block_header(&os, true, litlen_lens,
						 offset_lens);
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			while (out_pos < 8500) {
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   130, 1, expected_out, &out_pos);
			}
			/* Final pair: fused (0x55, len=130) + EOB */
			emit_literal(&os, litlen_lens, litlen_cws, 0x55,
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   130, 1, expected_out, &out_pos);
			emit_eob(&os, litlen_lens, litlen_cws);
			ASSERT(flush_bits(&os));
			csize = (size_t)(os.next - in_buf);
			verify_stream(d, gb, in_buf, csize,
				      expected_out, out_pos);

			cbuf = gb->cbuf_end - csize;
			memcpy(cbuf, in_buf, csize);

			/*
			 * E.g. 0 bytes left before the literal, 1 byte left
			 * (literal fits but match doesn't), etc.
			 */
			for (delta = 1; delta <= 132; delta++) {
				size_t avail = out_pos - delta;
				size_t actual_out = 0;

				if (delta > 3 && delta < 128)
					continue;
				dbuf = gb->dbuf_end - avail;
				res = libdeflate_deflate_decompress(
						d, cbuf, csize, dbuf, avail,
						&actual_out);
				ASSERT(res == LIBDEFLATE_INSUFFICIENT_SPACE);
			}
			dbuf = gb->dbuf_end - (out_pos + 1);
			res = libdeflate_deflate_decompress(
					d, cbuf, csize, dbuf, out_pos + 1,
					NULL);
			ASSERT(res == LIBDEFLATE_SHORT_OUTPUT);
		}

		/*
		 * Invalid offset == 2 right after a fused ('A', len 3) entry
		 * at the start of the output, in the fastloop with compound
		 * entries.
		 */
		{
			size_t csize, aout = 0;
			u8 *cbuf, *dbuf;
			enum libdeflate_result res;

			memset(&os, 0, sizeof(os));
			os.next = in_buf;
			os.end = in_buf + GUARDED_BUF_SIZE;
			put_dynamic_block_header(&os, true, litlen_lens,
						 offset_lens);
			/* 1 byte written by fused lit -> offset 2 is invalid */
			ASSERT(put_bits(&os, litlen_cws['A'],
					litlen_lens['A']));
			ASSERT(put_bits(&os, litlen_cws[257],
					litlen_lens[257]));
			ASSERT(put_bits(&os, offset_cws[1],
					offset_lens[1])); /* offset 2 */
			for (i = 0; i < 64; i++) {
				ASSERT(put_bits(&os, litlen_cws['A'],
						litlen_lens['A']));
			}
			emit_eob(&os, litlen_lens, litlen_cws);
			ASSERT(flush_bits(&os));
			csize = (size_t)(os.next - in_buf);
			cbuf = gb->cbuf_end - csize;
			memcpy(cbuf, in_buf, csize);

			/* Fastloop (compound table enabled, >= 8192 bytes) */
			dbuf = gb->dbuf_end - 16384;
			res = libdeflate_deflate_decompress(
					d, cbuf, csize, dbuf, 16384, &aout);
			ASSERT(res == LIBDEFLATE_BAD_DATA);

			/*
			 * Same, but with the stream ending right after the
			 * invalid match, so that too little input remains for
			 * the fastloop (and for compound entries) and the
			 * generic loop decodes the literal and match
			 * separately.
			 */
			memset(&os, 0, sizeof(os));
			os.next = in_buf;
			os.end = in_buf + GUARDED_BUF_SIZE;
			put_dynamic_block_header(&os, true, litlen_lens,
						 offset_lens);
			ASSERT(put_bits(&os, litlen_cws['A'],
					litlen_lens['A']));
			ASSERT(put_bits(&os, litlen_cws[257],
					litlen_lens[257]));
			ASSERT(put_bits(&os, offset_cws[1], offset_lens[1]));
			emit_eob(&os, litlen_lens, litlen_cws);
			ASSERT(flush_bits(&os));
			csize = (size_t)(os.next - in_buf);
			cbuf = gb->cbuf_end - csize;
			memcpy(cbuf, in_buf, csize);
			dbuf = gb->dbuf_end - 16384;
			res = libdeflate_deflate_decompress(
					d, cbuf, csize, dbuf, 16384, &aout);
			ASSERT(res == LIBDEFLATE_BAD_DATA);
		}

		/*
		 * Guard-page overrun sweep for 2 double-literal lookups
		 * (4 lits) + fused ('A', sym 280 len 130) = 135 bytes.
		 */
		{
			size_t csize, extra;
			u8 *cbuf, *dbuf;

			memset(&os, 0, sizeof(os));
			os.next = in_buf;
			os.end = in_buf + GUARDED_BUF_SIZE;
			out_pos = 0;
			put_dynamic_block_header(&os, false, litlen_lens,
						 offset_lens);
			for (i = 0; i < 16; i++) {
				emit_literal(&os, litlen_lens, litlen_cws, 'E',
					     expected_out, &out_pos);
			}
			for (i = 0; i < 65; i++) {
				size_t k;

				for (k = 0; k < 5; k++) {
					emit_literal(&os, litlen_lens,
						     litlen_cws, 'A',
						     expected_out, &out_pos);
				}
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   130, (i & 1) ? 16 : 1,
					   expected_out, &out_pos);
			}
			emit_eob(&os, litlen_lens, litlen_cws);
			for (i = 0; i < 6; i++) {
				ASSERT(put_bits(&os, (i == 5) ? 1 : 0, 1));
				ASSERT(put_bits(&os, 0, 2));
				ASSERT(flush_bits(&os));
				ASSERT(os.end - os.next >= 4);
				*os.next++ = 0x00;
				*os.next++ = 0x00;
				*os.next++ = 0xFF;
				*os.next++ = 0xFF;
			}
			csize = (size_t)(os.next - in_buf);
			verify_stream_zlib(in_buf, csize, expected_out,
					   out_pos);
			cbuf = gb->cbuf_end - csize;
			memcpy(cbuf, in_buf, csize);
			for (extra = 0; extra <= 48; extra++) {
				size_t avail = out_pos + extra;
				size_t aout = 0;

				dbuf = gb->dbuf_end - avail;
				ASSERT(libdeflate_deflate_decompress(
						d, cbuf, csize, dbuf, avail,
						&aout) == LIBDEFLATE_SUCCESS);
				ASSERT(aout == out_pos);
				ASSERT(memcmp(dbuf, expected_out,
					      out_pos) == 0);
			}
		}
	}

	/*
	 * 2. Fused (lit, len)-only tree (no double literals possible):
	 *    257..262 at len 3 (6/8), EOB and 263 at len 4 (2/16),
	 *    16 literals 'A'..'P' at len 7 (16/128 = 1/8).
	 *    2 * min_lit_cw_len == 14 > 12, while
	 *    min_lit_cw_len + min_lensym_cw_len == 7 + 3 == 10 <= 12
	 *    (max pair 7 + 4 == 11, table_bits == 11).
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		for (i = 257; i <= 262; i++)
			litlen_lens[i] = 3;
		litlen_lens[EOB_SYM] = 4;
		litlen_lens[263] = 4;
		for (i = 0; i < 16; i++)
			litlen_lens['A' + i] = 7;

		offset_lens[0] = 2; /* off 1 */
		offset_lens[1] = 2; /* off 2 */
		offset_lens[2] = 2; /* off 3 */
		offset_lens[3] = 2; /* off 4 */
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		for (i = 0; i < 16; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)('A' + i), expected_out, &out_pos);
		}
		for (i = 0; i < 1400; i++) {
			u8 lit = (u8)('A' + (i % 16));
			u32 len = (u32)(3 + (i % 7)); /* syms 257..263 */
			u32 off = (u32)(1 + (i % 4));

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   len, off, expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));
		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 3. Fused (lit, len) entry after 14 uncompressed seed bytes followed
	 *    immediately by an offset == 15 match: the fused literal writes
	 *    out[14] and advances out_next to out + 15 while out[15] (src[15])
	 *    is still completely uninitialized when COPY_MATCH loads 16 bytes
	 *    from src.
	 */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		ASSERT(put_bits(&os, 0, 1)); /* BFINAL = 0 */
		ASSERT(put_bits(&os, 0, 2)); /* BTYPE = UNCOMPRESSED */
		ASSERT(flush_bits(&os));
		*os.next++ = 14;
		*os.next++ = 0;
		*os.next++ = (u8)~14U;
		*os.next++ = 0xFF;
		for (i = 0; i < 14; i++) {
			u8 lit = (u8)(0x30 + i);

			*os.next++ = lit;
			expected_out[out_pos++] = lit;
		}

		memset(litlen_lens, 0, sizeof(litlen_lens));
		memset(offset_lens, 0, sizeof(offset_lens));
		litlen_lens['A'] = 1;
		litlen_lens[0xFF] = 2;
		litlen_lens[269] = 3; /* len 19..22 (fusable) */
		litlen_lens[EOB_SYM] = 3;
		offset_lens[0] = 1;
		offset_lens[7] = 1; /* off 13..16 */
		compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS,
					    litlen_cws);
		compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS,
					    offset_cws);

		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		emit_literal(&os, litlen_lens, litlen_cws, 0xFF,
			     expected_out, &out_pos);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   19, 15, expected_out, &out_pos);
		for (i = 0; i < 8300; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));
		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 4. Double-literal compound tree where fusable short length symbols
	 *    exist at len <= 11 (lensym_cw_len_mask != 0), yet no fused
	 *    (lit, len) pair fits in LITLEN_TABLEBITS (12):
	 *    15 literals 'A'..'O' at len 4 (15/16 = 480/512),
	 *    285 at len 6 (8/512), 281..284 at len 7 (16/512),
	 *    EOB + 257..262 at len 9 (7/512 = 28/2048),
	 *    263 at len 10 (1/1024), 264 at len 11 (1/2048),
	 *    and 265..266 at len 12 (2/4096).
	 *    Here min_lit_cw_len == 4 and min_lensym_cw_len == 9, so 2*4 == 8
	 *    <= 12 while 4 + 9 == 13 > 12 (tested both with max_codeword_len
	 *    == 12 and with max_codeword_len == 9 where table_bits shrinks to
	 *    9).
	 */
	{
		int mode;

		for (mode = 0; mode < 2; mode++) {
			struct output_bitstream os = {
				.next = in_buf,
				.end = in_buf + GUARDED_BUF_SIZE,
			};
			size_t out_pos = 0;

			memset(litlen_lens, 0, sizeof(litlen_lens));
			memset(offset_lens, 0, sizeof(offset_lens));
			for (i = 0; i < 15; i++)
				litlen_lens['A' + i] = 4;
			litlen_lens[285] = 6;
			for (i = 281; i <= 284; i++)
				litlen_lens[i] = 7;
			litlen_lens[EOB_SYM] = 9;
			for (i = 257; i <= 262; i++)
				litlen_lens[i] = 9;
			if (mode == 0) {
				litlen_lens[263] = 9;
			} else {
				litlen_lens[263] = 10;
				litlen_lens[264] = 11;
				litlen_lens[265] = 12;
				litlen_lens[266] = 12;
			}
			offset_lens[0] = 1;
			offset_lens[1] = 1;
			compute_canonical_codewords(litlen_lens,
						    NUM_LITLEN_SYMS,
						    litlen_cws);
			compute_canonical_codewords(offset_lens,
						    NUM_OFFSET_SYMS,
						    offset_cws);

			put_dynamic_block_header(&os, true, litlen_lens,
						 offset_lens);
			for (i = 0; i < 1200; i++) {
				u8 lit1 = (u8)('A' + (i % 15));
				u8 lit2 = (u8)('A' + ((i * 7 + 3) % 15));
				u32 mlen = (u32)(3 + (i % (mode ? 10 : 7)));

				emit_literal(&os, litlen_lens, litlen_cws,
					     lit1, expected_out, &out_pos);
				if (i & 1) {
					emit_literal(&os, litlen_lens,
						     litlen_cws, lit2,
						     expected_out, &out_pos);
				}
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   mlen, (i & 1) ? 2 : 1,
					   expected_out, &out_pos);
			}
			emit_eob(&os, litlen_lens, litlen_cws);
			ASSERT(flush_bits(&os));
			verify_stream(d, gb, in_buf,
				      (size_t)(os.next - in_buf),
				      expected_out, out_pos);
		}
	}

	free(in_buf);
	free(expected_out);
}

/*
 * Test the 32 KiB split between decompress_fastloop_checked() and
 * decompress_fastloop_unchecked():
 *  1. Valid streams crossing DEFLATE_MAX_MATCH_OFFSET (32768) both inside a
 *     block and across block boundaries, with offsets 1..32768.
 *  2. Exact boundary error checks right before 32768 (offset 32768 at out_pos
 *     == 32767 MUST fail; offset 32768 after fused lit+len advancing 32766 ->
 *     32767 MUST fail; offset 32768 after fused lit+len advancing 32767 ->
 *     32768 MUST succeed).
 *  3. Unused offset symbols 30 and 31 (HDIST=31) emitting max offset 32768 in
 *     unchecked fastloop without out-of-bounds memory access.
 */
static void
test_32k_fastloop_split(struct libdeflate_decompressor *d,
			const struct guarded_buffers *gb)
{
	u8 litlen_lens[NUM_LITLEN_SYMS];
	u8 offset_lens[32];
	u16 litlen_cws[NUM_LITLEN_SYMS];
	u16 offset_cws[32];
	u8 *in_buf;
	u8 *expected_out;
	size_t i;

	in_buf = xmalloc(GUARDED_BUF_SIZE);
	expected_out = xmalloc(GUARDED_BUF_SIZE);

	/* Skewed tree: 'A':2, 'B':3, 'C'..'G'+EOB+257+258+280+285:4 (sum=1) */
	memset(litlen_lens, 0, sizeof(litlen_lens));
	litlen_lens['A'] = 2;
	litlen_lens['B'] = 3;
	litlen_lens['C'] = 4;
	litlen_lens['D'] = 4;
	litlen_lens['E'] = 4;
	litlen_lens['F'] = 4;
	litlen_lens['G'] = 4;
	litlen_lens[EOB_SYM] = 4;
	litlen_lens[257] = 4; /* len 3 */
	litlen_lens[258] = 4; /* len 4 */
	litlen_lens[280] = 4; /* len 115..130 (fusable) */
	litlen_lens[285] = 4; /* len 258 (non-fusable) */
	compute_canonical_codewords(litlen_lens, NUM_LITLEN_SYMS, litlen_cws);

	/* Standard 30 offset symbols: 2*2^-4 + 28*2^-5 = 1 */
	for (i = 0; i < 2; i++)
		offset_lens[i] = 4;
	for (; i < NUM_OFFSET_SYMS; i++)
		offset_lens[i] = 5;
	compute_canonical_codewords(offset_lens, NUM_OFFSET_SYMS, offset_cws);

	/* 1. Multiple blocks crossing 32768, using offset 32768 and fusion */
	{
		struct output_bitstream os = {
			.next = in_buf,
			.end = in_buf + GUARDED_BUF_SIZE,
		};
		size_t out_pos = 0;

		/* Block 1: write distinct seed pattern stopping at 32761 */
		put_dynamic_block_header(&os, false, litlen_lens, offset_lens);
		for (i = 0; i < 258; i++) {
			u8 lit = (u8)('A' + ((i * 3 + (i / 5)) % 7));

			emit_literal(&os, litlen_lens, litlen_cws, lit,
				     expected_out, &out_pos);
		}
		while (out_pos + 258 <= 32761) {
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   258, 257, expected_out, &out_pos);
		}
		while (out_pos < 32761) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)('A' + (out_pos % 7)),
				     expected_out, &out_pos);
		}
		ASSERT(out_pos == 32761);
		emit_eob(&os, litlen_lens, litlen_cws);

		/*
		 * Block 2: starts at 32761 (in checked fastloop). Emit 6
		 * literals (3 double-literal lookups) to reach 32767 cleanly at
		 * a lookup boundary, then a fused ('A', len 3) whose literal
		 * moves out_next to 32768 so offset 32768 is valid.
		 */
		put_dynamic_block_header(&os, false, litlen_lens, offset_lens);
		for (i = 0; i < 6; i++) {
			emit_literal(&os, litlen_lens, litlen_cws,
				     (u8)('A' + i), expected_out, &out_pos);
		}
		ASSERT(out_pos == 32767);
		emit_literal(&os, litlen_lens, litlen_cws, 'A',
			     expected_out, &out_pos);
		ASSERT(out_pos == 32768);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   3, 32768, expected_out, &out_pos);
		/* Continue inside unchecked fastloop with max offset 32768 */
		for (i = 0; i < 16; i++) {
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   258, (i & 1) ? 32768 : 32767,
				   expected_out, &out_pos);
			emit_literal(&os, litlen_lens, litlen_cws, 'C',
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   120, 32768, expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);

		/* Block 3: starts past 32768 (unchecked fastloop from start) */
		put_dynamic_block_header(&os, true, litlen_lens, offset_lens);
		emit_match(&os, litlen_lens, litlen_cws,
			   offset_lens, offset_cws,
			   258, 32768, expected_out, &out_pos);
		for (i = 0; i < 12; i++) {
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			emit_match(&os, litlen_lens, litlen_cws,
				   offset_lens, offset_cws,
				   4, 32768, expected_out, &out_pos);
		}
		emit_eob(&os, litlen_lens, litlen_cws);
		ASSERT(flush_bits(&os));

		verify_stream(d, gb, in_buf, (size_t)(os.next - in_buf),
			      expected_out, out_pos);
	}

	/*
	 * 2. Invalid offset 32768 right BEFORE byte 32768:
	 *    mode 0: plain match at out_pos == 32767 (offset 32768 > 32767)
	 *    mode 1: fused ('A', len 3) at out_pos == 32766 -> literal writes
	 *            byte 32766 (out_pos becomes 32767), offset 32768 > 32767.
	 */
	{
		int mode;

		for (mode = 0; mode < 2; mode++) {
			struct output_bitstream os = {
				.next = in_buf,
				.end = in_buf + GUARDED_BUF_SIZE,
			};
			size_t out_pos = 0;
			size_t init_lits = (mode == 0) ? 17 : 16;
			size_t target = (mode == 0) ? 32767 : 32766;
			size_t csize, aout = 0;
			u8 *cbuf, *dbuf;

			put_dynamic_block_header(&os, true, litlen_lens,
						 offset_lens);
			/*
			 * Use 17 seed literals in mode 0 (285 is non-fusable,
			 * so the 17th 'B' stays separate) so that both modes
			 * emit an even count (242) of trailing 'B's and reach
			 * 'target' cleanly on a lookup boundary.
			 */
			for (i = 0; i < init_lits; i++) {
				emit_literal(&os, litlen_lens, litlen_cws,
					     'B', expected_out, &out_pos);
			}
			while (out_pos + 258 <= target) {
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   258, 1, expected_out, &out_pos);
			}
			while (out_pos < target) {
				emit_literal(&os, litlen_lens, litlen_cws,
					     'B', expected_out, &out_pos);
			}
			if (mode == 1) {
				ASSERT(put_bits(&os, litlen_cws['A'],
						litlen_lens['A']));
			}
			/* length 3, offset 32768 (sym 29, extra = 8191) */
			ASSERT(put_bits(&os, litlen_cws[257],
					litlen_lens[257]));
			ASSERT(put_bits(&os, offset_cws[29], offset_lens[29]));
			ASSERT(put_bits(&os, 8191, 13));
			/* Trailing literals so fastloop stays active */
			for (i = 0; i < 128; i++) {
				ASSERT(put_bits(&os, litlen_cws['C'],
						litlen_lens['C']));
			}
			emit_eob(&os, litlen_lens, litlen_cws);
			ASSERT(flush_bits(&os));
			csize = (size_t)(os.next - in_buf);
			cbuf = gb->cbuf_end - csize;
			memcpy(cbuf, in_buf, csize);
			dbuf = gb->dbuf_end - 65536;
			ASSERT(libdeflate_deflate_decompress(
					d, cbuf, csize, dbuf, 65536,
					&aout) == LIBDEFLATE_BAD_DATA);
		}
	}

	/*
	 * 3. Unused offset symbols 30 and 31 (HDIST=31, 32 offset syms of len
	 *    5).  Verify they decode offset 24577+8191 == 32768 cleanly inside
	 *    decompress_fastloop_unchecked() and reject at out_pos == 32767.
	 */
	{
		u32 sym;

		for (i = 0; i < 32; i++)
			offset_lens[i] = 5;
		compute_canonical_codewords(offset_lens, 32, offset_cws);

		for (sym = 30; sym <= 31; sym++) {
			struct output_bitstream os = {
				.next = in_buf,
				.end = in_buf + GUARDED_BUF_SIZE,
			};
			size_t out_pos = 0;
			size_t csize, aout = 0;
			size_t k;
			u8 *cbuf, *dbuf;

			/* Valid at out_pos == 32768 */
			put_dynamic_block_header_n(&os, true,
						   litlen_lens, NUM_LITLEN_SYMS,
						   offset_lens, 32);
			for (i = 0; i < 258; i++) {
				emit_literal(&os, litlen_lens, litlen_cws,
					     (u8)('A' + (i % 7)),
					     expected_out, &out_pos);
			}
			while (out_pos + 258 <= 32768) {
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   258, 257, expected_out, &out_pos);
			}
			while (out_pos < 32768) {
				emit_literal(&os, litlen_lens, litlen_cws,
					     'D', expected_out, &out_pos);
			}
			ASSERT(out_pos == 32768);
			/*
			 * Emit offset symbol 30 or 31 with max extra (8191)
			 * twice: the first runs at out_pos == 32768 (still in
			 * decompress_fastloop_checked(), which then exits at
			 * out_pos == 33026), and the second runs inside
			 * decompress_fastloop_unchecked().
			 */
			for (k = 0; k < 2; k++) {
				ASSERT(put_bits(&os, litlen_cws[285],
						litlen_lens[285]));
				ASSERT(put_bits(&os, offset_cws[sym],
						offset_lens[sym]));
				ASSERT(put_bits(&os, 8191, 13));
				for (i = 0; i < 258; i++) {
					expected_out[out_pos] =
						expected_out[out_pos - 32768];
					out_pos++;
				}
			}
			for (i = 0; i < 64; i++) {
				emit_literal(&os, litlen_lens, litlen_cws, 'E',
					     expected_out, &out_pos);
			}
			emit_eob(&os, litlen_lens, litlen_cws);
			ASSERT(flush_bits(&os));
			verify_stream_libdeflate(d, gb, in_buf,
						 (size_t)(os.next - in_buf),
						 expected_out, out_pos);

			/* Rejected at out_pos == 32767 */
			memset(&os, 0, sizeof(os));
			os.next = in_buf;
			os.end = in_buf + GUARDED_BUF_SIZE;
			out_pos = 0;
			put_dynamic_block_header_n(&os, true,
						   litlen_lens, NUM_LITLEN_SYMS,
						   offset_lens, 32);
			emit_literal(&os, litlen_lens, litlen_cws, 'A',
				     expected_out, &out_pos);
			while (out_pos + 258 <= 32767) {
				emit_match(&os, litlen_lens, litlen_cws,
					   offset_lens, offset_cws,
					   258, 1, expected_out, &out_pos);
			}
			while (out_pos < 32767) {
				emit_literal(&os, litlen_lens, litlen_cws,
					     'A', expected_out, &out_pos);
			}
			ASSERT(put_bits(&os, litlen_cws[257],
					litlen_lens[257]));
			ASSERT(put_bits(&os, offset_cws[sym],
					offset_lens[sym]));
			ASSERT(put_bits(&os, 8191, 13));
			for (i = 0; i < 64; i++) {
				ASSERT(put_bits(&os, litlen_cws['E'],
						litlen_lens['E']));
			}
			emit_eob(&os, litlen_lens, litlen_cws);
			ASSERT(flush_bits(&os));
			csize = (size_t)(os.next - in_buf);
			cbuf = gb->cbuf_end - csize;
			memcpy(cbuf, in_buf, csize);
			dbuf = gb->dbuf_end - 65536;
			ASSERT(libdeflate_deflate_decompress(
					d, cbuf, csize, dbuf, 65536,
					&aout) == LIBDEFLATE_BAD_DATA);
		}
	}

	free(in_buf);
	free(expected_out);
}

int
tmain(int argc, tchar *argv[])
{
	struct libdeflate_decompressor *d;
	struct guarded_buffers gb;

	(void)argc;
	begin_program(argv);

	alloc_guarded_buffer(GUARDED_BUF_SIZE, &gb.cbuf_start, &gb.cbuf_end);
	alloc_guarded_buffer(GUARDED_BUF_SIZE, &gb.dbuf_start, &gb.dbuf_end);

	d = libdeflate_alloc_decompressor();
	ASSERT(d != NULL);

	test_fastloop_match_copies(d, &gb);
	test_double_literal_tables(d, &gb);
	test_double_literal_edge_conditions(d, &gb);
	test_precode_rle_overrun(d, &gb);
	test_12bit_and_enough_tables(d, &gb);
	test_litlen_fusion(d, &gb);
	test_32k_fastloop_split(d, &gb);

	libdeflate_free_decompressor(d);
	free_guarded_buffer(gb.cbuf_start, gb.cbuf_end);
	free_guarded_buffer(gb.dbuf_start, gb.dbuf_end);
	return 0;
}
