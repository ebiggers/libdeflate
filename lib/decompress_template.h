/*
 * decompress_template.h
 *
 * Copyright 2016 Eric Biggers
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
 * This is the actual DEFLATE decompression routine, lifted out of
 * deflate_decompress.c so that it can be compiled multiple times with different
 * target instruction sets.
 */

static enum libdeflate_result ATTRIBUTES
FUNCNAME(struct libdeflate_decompressor * restrict d,
	 const void * restrict in, size_t in_nbytes,
	 void * restrict out, size_t out_nbytes_avail,
	 size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret)
{
	u8 *out_next = out;
	u8 * const out_end = out_next + out_nbytes_avail;
	u8 * const out_fastloop_end =
		out_end - MIN(out_nbytes_avail, FASTLOOP_MAX_BYTES_WRITTEN);
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	const u8 * const in_fastloop_end =
		in_end - MIN(in_nbytes, FASTLOOP_MAX_BYTES_READ);
	bitbuf_t bitbuf = 0;
	bitbuf_t saved_bitbuf;
	u32 bitsleft = 0;
	size_t overread_count = 0;
	unsigned i;
	bool is_final_block;
	unsigned block_type;
	u16 len;
	u16 nlen;
	unsigned num_litlen_syms;
	unsigned num_offset_syms;
	bitbuf_t tmpbits;

next_block:
	/* Starting to read the next block */
	;

	STATIC_ASSERT(CAN_ENSURE(1 + 2 + 5 + 5 + 4 + 3));
	REFILL_BITS();

	/* BFINAL: 1 bit */
	is_final_block = POP_BITS(1);

	/* BTYPE: 2 bits */
	block_type = POP_BITS(2);

	if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN) {

		/* Dynamic Huffman block */

		/* The order in which precode lengths are stored */
		static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
			16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
		};

		unsigned num_explicit_precode_lens;

		/* Read the codeword length counts. */

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == ((1 << 5) - 1) + 257);
		num_litlen_syms = POP_BITS(5) + 257;

		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == ((1 << 5) - 1) + 1);
		num_offset_syms = POP_BITS(5) + 1;

		STATIC_ASSERT(DEFLATE_NUM_PRECODE_SYMS == ((1 << 4) - 1) + 4);
		num_explicit_precode_lens = POP_BITS(4) + 4;

		d->static_codes_loaded = false;

		/*
		 * Read the precode codeword lengths.
		 *
		 * A 64-bit bitbuffer is just one bit too small to hold the
		 * maximum number of precode lens, so to minimize branches we
		 * merge one len with the previous fields.
		 */
		STATIC_ASSERT(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);
		if (CAN_ENSURE(3 * (DEFLATE_NUM_PRECODE_SYMS - 1))) {
			d->u.precode_lens[deflate_precode_lens_permutation[0]] = POP_BITS(3);
			REFILL_BITS();
			for (i = 1; i < num_explicit_precode_lens; i++)
				d->u.precode_lens[deflate_precode_lens_permutation[i]] = POP_BITS(3);
		} else {
			for (i = 0; i < num_explicit_precode_lens; i++) {
				ENSURE_BITS(3);
				d->u.precode_lens[deflate_precode_lens_permutation[i]] = POP_BITS(3);
			}
		}
		for (; i < DEFLATE_NUM_PRECODE_SYMS; i++)
			d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

		/* Build the decode table for the precode. */
		SAFETY_CHECK(build_precode_decode_table(d));

		/* Decode the litlen and offset codeword lengths. */
		for (i = 0; i < num_litlen_syms + num_offset_syms; ) {
			u32 entry;
			unsigned presym;
			u8 rep_val;
			unsigned rep_count;

			ENSURE_BITS(DEFLATE_MAX_PRE_CODEWORD_LEN + 7);

			/*
			 * The code below assumes that the precode decode table
			 * doesn't have any subtables.
			 */
			STATIC_ASSERT(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

			/* Read the next precode symbol. */
			entry = d->u.l.precode_decode_table[BITS(DEFLATE_MAX_PRE_CODEWORD_LEN)];
			REMOVE_BITS((u8)entry);
			presym = entry >> 16;

			if (presym < 16) {
				/* Explicit codeword length */
				d->u.l.lens[i++] = presym;
				continue;
			}

			/* Run-length encoded codeword lengths */

			/*
			 * Note: we don't need verify that the repeat count
			 * doesn't overflow the number of elements, since we've
			 * sized the lens array to have enough extra space to
			 * allow for the worst-case overrun (138 zeroes when
			 * only 1 length was remaining).
			 *
			 * In the case of the small repeat counts (presyms 16
			 * and 17), it is fastest to always write the maximum
			 * number of entries.  That gets rid of branches that
			 * would otherwise be required.
			 *
			 * It is not just because of the numerical order that
			 * our checks go in the order 'presym < 16', 'presym ==
			 * 16', and 'presym == 17'.  For typical data this is
			 * ordered from most frequent to least frequent case.
			 */
			STATIC_ASSERT(DEFLATE_MAX_LENS_OVERRUN == 138 - 1);

			if (presym == 16) {
				/* Repeat the previous length 3 - 6 times. */
				SAFETY_CHECK(i != 0);
				rep_val = d->u.l.lens[i - 1];
				STATIC_ASSERT(3 + ((1 << 2) - 1) == 6);
				rep_count = 3 + POP_BITS(2);
				d->u.l.lens[i + 0] = rep_val;
				d->u.l.lens[i + 1] = rep_val;
				d->u.l.lens[i + 2] = rep_val;
				d->u.l.lens[i + 3] = rep_val;
				d->u.l.lens[i + 4] = rep_val;
				d->u.l.lens[i + 5] = rep_val;
				i += rep_count;
			} else if (presym == 17) {
				/* Repeat zero 3 - 10 times. */
				STATIC_ASSERT(3 + ((1 << 3) - 1) == 10);
				rep_count = 3 + POP_BITS(3);
				d->u.l.lens[i + 0] = 0;
				d->u.l.lens[i + 1] = 0;
				d->u.l.lens[i + 2] = 0;
				d->u.l.lens[i + 3] = 0;
				d->u.l.lens[i + 4] = 0;
				d->u.l.lens[i + 5] = 0;
				d->u.l.lens[i + 6] = 0;
				d->u.l.lens[i + 7] = 0;
				d->u.l.lens[i + 8] = 0;
				d->u.l.lens[i + 9] = 0;
				i += rep_count;
			} else {
				/* Repeat zero 11 - 138 times. */
				STATIC_ASSERT(11 + ((1 << 7) - 1) == 138);
				rep_count = 11 + POP_BITS(7);
				memset(&d->u.l.lens[i], 0,
				       rep_count * sizeof(d->u.l.lens[i]));
				i += rep_count;
			}
		}
	} else if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {
		/*
		 * Uncompressed block: copy 'len' bytes literally from the input
		 * buffer to the output buffer.
		 */

		ALIGN_INPUT();

		SAFETY_CHECK(in_end - in_next >= 4);
		len = get_unaligned_le16(in_next);
		nlen = get_unaligned_le16(in_next + 2);
		in_next += 4;

		SAFETY_CHECK(len == (u16)~nlen);
		if (unlikely(len > out_end - out_next))
			return LIBDEFLATE_INSUFFICIENT_SPACE;
		SAFETY_CHECK(len <= in_end - in_next);

		memcpy(out_next, in_next, len);
		in_next += len;
		out_next += len;

		goto block_done;

	} else {
		SAFETY_CHECK(block_type == DEFLATE_BLOCKTYPE_STATIC_HUFFMAN);

		/*
		 * Static Huffman block: build the decode tables for the static
		 * codes.  Skip doing so if the tables are already set up from
		 * an earlier static block; this speeds up decompression of
		 * degenerate input of many empty or very short static blocks.
		 *
		 * Afterwards, the remainder is the same as decompressing a
		 * dynamic Huffman block.
		 */

		if (d->static_codes_loaded)
			goto have_decode_tables;

		d->static_codes_loaded = true;

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 288);
		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 32);

		for (i = 0; i < 144; i++)
			d->u.l.lens[i] = 8;
		for (; i < 256; i++)
			d->u.l.lens[i] = 9;
		for (; i < 280; i++)
			d->u.l.lens[i] = 7;
		for (; i < 288; i++)
			d->u.l.lens[i] = 8;

		for (; i < 288 + 32; i++)
			d->u.l.lens[i] = 5;

		num_litlen_syms = 288;
		num_offset_syms = 32;
	}

	/* Decompressing a Huffman block (either dynamic or static) */

	SAFETY_CHECK(build_offset_decode_table(d, num_litlen_syms, num_offset_syms));
	SAFETY_CHECK(build_litlen_decode_table(d, num_litlen_syms, num_offset_syms));
have_decode_tables:

	/*
	 * This is the "fastloop" for decoding literals and matches.  It does
	 * bounds checks on in_next and out_next in the loop conditions so that
	 * additional bounds checks aren't needed inside the loop body.
	 */
	while (in_next < in_fastloop_end && out_next < out_fastloop_end) {
		u32 entry, length, offset;
		u8 lit;
		const u8 *src;
		u8 *dst;

		/* Refill the bitbuffer and decode a litlen symbol. */
		REFILL_BITS_IN_FASTLOOP();
		entry = d->u.litlen_decode_table[BITS(LITLEN_TABLEBITS)];
preloaded:
		if (CAN_ENSURE(3 * LITLEN_TABLEBITS +
			       DEFLATE_MAX_LITLEN_CODEWORD_LEN +
			       DEFLATE_MAX_EXTRA_LENGTH_BITS) &&
		    (entry & HUFFDEC_LITERAL)) {
			/*
			 * 64-bit only: fast path for decoding literals that
			 * don't need subtables.  We do up to 3 of these before
			 * proceeding to the general case.  This is the largest
			 * number of times that LITLEN_TABLEBITS bits can be
			 * extracted from a refilled 64-bit bitbuffer while
			 * still leaving enough bits to decode any match length.
			 *
			 * Note: the definitions of FASTLOOP_MAX_BYTES_WRITTEN
			 * and FASTLOOP_MAX_BYTES_READ need to be updated if the
			 * maximum number of literals decoded here is changed.
			 */
			REMOVE_ENTRY_BITS_FAST(entry);
			lit = entry >> 16;
			entry = d->u.litlen_decode_table[BITS(LITLEN_TABLEBITS)];
			*out_next++ = lit;
			if (entry & HUFFDEC_LITERAL) {
				REMOVE_ENTRY_BITS_FAST(entry);
				lit = entry >> 16;
				entry = d->u.litlen_decode_table[BITS(LITLEN_TABLEBITS)];
				*out_next++ = lit;
				if (entry & HUFFDEC_LITERAL) {
					REMOVE_ENTRY_BITS_FAST(entry);
					lit = entry >> 16;
					entry = d->u.litlen_decode_table[BITS(LITLEN_TABLEBITS)];
					*out_next++ = lit;
				}
			}
		}
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			/* Subtable pointer or end-of-block entry */
			if (entry & HUFFDEC_SUBTABLE_POINTER) {
				REMOVE_BITS(LITLEN_TABLEBITS);
				entry = d->u.litlen_decode_table[(entry >> 16) + BITS((u8)entry)];
			}
			SAVE_BITBUF();
			REMOVE_ENTRY_BITS_FAST(entry);
			if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
				goto block_done;
			/* Literal or length entry, from a subtable */
		} else {
			/* Literal or length entry, from the main table */
			SAVE_BITBUF();
			REMOVE_ENTRY_BITS_FAST(entry);
		}
		length = entry >> 16;
		if (entry & HUFFDEC_LITERAL) {
			/*
			 * Literal that didn't get handled by the literal fast
			 * path earlier
			 */
			*out_next++ = length;
			continue;
		}
		/*
		 * Match length.  Finish decoding it.  We don't need to check
		 * for too-long matches here, as this is inside the fastloop
		 * where it's already been verified that the output buffer has
		 * enough space remaining to copy a max-length match.
		 */
		length += SAVED_BITS((u8)entry) >> (u8)(entry >> 8);

		/* Decode the match offset. */

		/* Refill the bitbuffer if it may be needed for the offset. */
		if (unlikely(GET_REAL_BITSLEFT() <
			     DEFLATE_MAX_OFFSET_CODEWORD_LEN +
			     DEFLATE_MAX_EXTRA_OFFSET_BITS))
			REFILL_BITS_IN_FASTLOOP();

		STATIC_ASSERT(CAN_ENSURE(OFFSET_TABLEBITS +
					 DEFLATE_MAX_EXTRA_OFFSET_BITS));
		STATIC_ASSERT(CAN_ENSURE(DEFLATE_MAX_OFFSET_CODEWORD_LEN -
					 OFFSET_TABLEBITS +
					 DEFLATE_MAX_EXTRA_OFFSET_BITS));

		entry = d->offset_decode_table[BITS(OFFSET_TABLEBITS)];
		if (entry & HUFFDEC_EXCEPTIONAL) {
			/* Offset codeword requires a subtable */
			REMOVE_BITS(OFFSET_TABLEBITS);
			entry = d->offset_decode_table[(entry >> 16) + BITS((u8)entry)];
			/*
			 * On 32-bit, we might not be able to decode the offset
			 * symbol and extra offset bits without refilling the
			 * bitbuffer in between.  However, this is only an issue
			 * when a subtable is needed, so do the refill here.
			 */
			if (!CAN_ENSURE(DEFLATE_MAX_OFFSET_CODEWORD_LEN +
					DEFLATE_MAX_EXTRA_OFFSET_BITS))
				REFILL_BITS_IN_FASTLOOP();
		}
		SAVE_BITBUF();
		REMOVE_ENTRY_BITS_FAST(entry);
		offset = (entry >> 16) + (SAVED_BITS((u8)entry) >> (u8)(entry >> 8));

		/* Validate the match offset; needed even in the fastloop. */
		SAFETY_CHECK(offset <= out_next - (const u8 *)out);

		/*
		 * Before starting to copy the match, refill the bitbuffer and
		 * preload the litlen decode table entry for the next loop
		 * iteration.  This can increase performance by allowing the
		 * latency of the two operations to overlap.
		 */
		REFILL_BITS_IN_FASTLOOP();
		entry = d->u.litlen_decode_table[BITS(LITLEN_TABLEBITS)];

		/*
		 * Copy the match.  On most CPUs the fastest method is a
		 * word-at-a-time copy, unconditionally copying at least 3 words
		 * since this is enough for most matches without being too much.
		 *
		 * The normal word-at-a-time copy works for offset >= WORDBYTES,
		 * which is most cases.  The case of offset == 1 is also common
		 * and is worth optimizing for, since it is just RLE encoding of
		 * the previous byte, which is the result of compressing long
		 * runs of the same byte.  We currently don't optimize for the
		 * less common cases of offset > 1 && offset < WORDBYTES; we
		 * just fall back to a traditional byte-at-a-time copy for them.
		 */
		src = out_next - offset;
		dst = out_next;
		out_next += length;
		if (UNALIGNED_ACCESS_IS_FAST && offset >= WORDBYTES) {
			copy_word_unaligned(src, dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			copy_word_unaligned(src, dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			do {
				copy_word_unaligned(src, dst);
				src += WORDBYTES;
				dst += WORDBYTES;
			} while (dst < out_next);
		} else if (UNALIGNED_ACCESS_IS_FAST && offset == 1) {
			machine_word_t v = repeat_byte(*src);

			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			do {
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
			} while (dst < out_next);
		} else {
			STATIC_ASSERT(DEFLATE_MIN_MATCH_LEN == 3);
			*dst++ = *src++;
			*dst++ = *src++;
			do {
				*dst++ = *src++;
			} while (dst < out_next);
		}
		if (in_next < in_fastloop_end && out_next < out_fastloop_end)
			goto preloaded;
		break;
	}
	/* MASK_BITSLEFT() is needed when leaving the fastloop. */
	MASK_BITSLEFT();

	/*
	 * This is the generic loop for decoding literals and matches.  This
	 * handles cases where in_next and out_next are close to the end of
	 * their respective buffers.  Usually this loop isn't performance-
	 * critical, as most time is spent in the fastloop above instead.  We
	 * therefore omit some optimizations here in favor of smaller code.
	 */
	for (;;) {
		u32 entry, length, offset;
		const u8 *src;
		u8 *dst;

		REFILL_BITS();
		entry = d->u.litlen_decode_table[BITS(LITLEN_TABLEBITS)];
		if (unlikely(entry & HUFFDEC_SUBTABLE_POINTER)) {
			REMOVE_BITS(LITLEN_TABLEBITS);
			entry = d->u.litlen_decode_table[(entry >> 16) + BITS((u8)entry)];
		}
		SAVE_BITBUF();
		REMOVE_BITS((u8)entry);
		length = entry >> 16;
		if (entry & HUFFDEC_LITERAL) {
			if (unlikely(out_next == out_end))
				return LIBDEFLATE_INSUFFICIENT_SPACE;
			*out_next++ = length;
			continue;
		}
		if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
			goto block_done;
		length += SAVED_BITS((u8)entry) >> (u8)(entry >> 8);
		if (unlikely(length > out_end - out_next))
			return LIBDEFLATE_INSUFFICIENT_SPACE;

		if (CAN_ENSURE(DEFLATE_MAX_OFFSET_CODEWORD_LEN +
			       DEFLATE_MAX_EXTRA_OFFSET_BITS)) {
			ENSURE_BITS(DEFLATE_MAX_OFFSET_CODEWORD_LEN +
				    DEFLATE_MAX_EXTRA_OFFSET_BITS);
		} else {
			ENSURE_BITS(OFFSET_TABLEBITS +
				    DEFLATE_MAX_EXTRA_OFFSET_BITS);
		}
		entry = d->offset_decode_table[BITS(OFFSET_TABLEBITS)];
		if (entry & HUFFDEC_EXCEPTIONAL) {
			REMOVE_BITS(OFFSET_TABLEBITS);
			entry = d->offset_decode_table[(entry >> 16) + BITS((u8)entry)];
			if (!CAN_ENSURE(DEFLATE_MAX_OFFSET_CODEWORD_LEN +
					DEFLATE_MAX_EXTRA_OFFSET_BITS))
				ENSURE_BITS(DEFLATE_MAX_OFFSET_CODEWORD_LEN -
					    OFFSET_TABLEBITS +
					    DEFLATE_MAX_EXTRA_OFFSET_BITS);
		}
		SAVE_BITBUF();
		REMOVE_BITS((u8)entry);
		offset = (entry >> 16) + (SAVED_BITS((u8)entry) >> (u8)(entry >> 8));

		SAFETY_CHECK(offset <= out_next - (const u8 *)out);

		src = out_next - offset;
		dst = out_next;
		out_next += length;

		STATIC_ASSERT(DEFLATE_MIN_MATCH_LEN == 3);
		*dst++ = *src++;
		*dst++ = *src++;
		do {
			*dst++ = *src++;
		} while (dst < out_next);
	}

block_done:
	/* MASK_BITSLEFT() is needed when leaving the fastloop. */
	MASK_BITSLEFT();

	/* Finished decoding a block */

	if (!is_final_block)
		goto next_block;

	/* That was the last block. */

	/* Discard any readahead bits and check for excessive overread. */
	ALIGN_INPUT();

	/* Optionally return the actual number of bytes read. */
	if (actual_in_nbytes_ret)
		*actual_in_nbytes_ret = in_next - (u8 *)in;

	/* Optionally return the actual number of bytes written. */
	if (actual_out_nbytes_ret) {
		*actual_out_nbytes_ret = out_next - (u8 *)out;
	} else {
		if (out_next != out_end)
			return LIBDEFLATE_SHORT_OUTPUT;
	}
	return LIBDEFLATE_SUCCESS;
}

#undef FUNCNAME
#undef ATTRIBUTES
