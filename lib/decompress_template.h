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
	machine_word_t litlen_tablemask;
	u32 entry;

next_block:
	/* Starting to read the next block */
	;

	STATIC_ASSERT(CAN_DECODE(1 + 2 + 5 + 5 + 4 + 3));
	REFILL_BITS();

	/* BFINAL: 1 bit */
	is_final_block = bitbuf & BITMASK(1);

	/* BTYPE: 2 bits */
	block_type = (bitbuf >> 1) & BITMASK(2);

	if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN) {

		/* Dynamic Huffman block */

		/* The order in which precode lengths are stored */
		static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
			16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
		};

		unsigned num_explicit_precode_lens;

		/* Read the codeword length counts. */

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 257 + BITMASK(5));
		num_litlen_syms = 257 + ((bitbuf >> 3) & BITMASK(5));

		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 1 + BITMASK(5));
		num_offset_syms = 1 + ((bitbuf >> 8) & BITMASK(5));

		STATIC_ASSERT(DEFLATE_NUM_PRECODE_SYMS == 4 + BITMASK(4));
		num_explicit_precode_lens = 4 + ((bitbuf >> 13) & BITMASK(4));

		d->static_codes_loaded = false;

		/*
		 * Read the precode codeword lengths.
		 *
		 * A 64-bit bitbuffer is just one bit too small to hold the
		 * maximum number of precode lens, so to minimize branches we
		 * merge one len with the previous fields.
		 */
		STATIC_ASSERT(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);
		if (CAN_DECODE(3 * (DEFLATE_NUM_PRECODE_SYMS - 1))) {
			d->u.precode_lens[deflate_precode_lens_permutation[0]] =
				(bitbuf >> 17) & BITMASK(3);
			bitbuf >>= 20;
			bitsleft -= 20;
			REFILL_BITS();
			i = 1;
			do {
				d->u.precode_lens[deflate_precode_lens_permutation[i]] =
					bitbuf & BITMASK(3);
				bitbuf >>= 3;
				bitsleft -= 3;
			} while (++i < num_explicit_precode_lens);
		} else {
			bitbuf >>= 17;
			bitsleft -= 17;
			i = 0;
			do {
				ENSURE_BITS(3);
				d->u.precode_lens[deflate_precode_lens_permutation[i]] =
					bitbuf & BITMASK(3);
				bitbuf >>= 3;
				bitsleft -= 3;
			} while (++i < num_explicit_precode_lens);
		}
		for (; i < DEFLATE_NUM_PRECODE_SYMS; i++)
			d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

		/* Build the decode table for the precode. */
		SAFETY_CHECK(build_precode_decode_table(d));

		/* Decode the litlen and offset codeword lengths. */
		i = 0;
		do {
			unsigned presym;
			u8 rep_val;
			unsigned rep_count;

			ENSURE_BITS(DEFLATE_MAX_PRE_CODEWORD_LEN + 7);

			/*
			 * The code below assumes that the precode decode table
			 * doesn't have any subtables.
			 */
			STATIC_ASSERT(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

			/* Decode the next precode symbol. */
			entry = d->u.l.precode_decode_table[
				bitbuf & BITMASK(DEFLATE_MAX_PRE_CODEWORD_LEN)];
			bitbuf >>= (u8)entry;
			bitsleft -= (u8)entry;
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
				STATIC_ASSERT(3 + BITMASK(2) == 6);
				rep_count = 3 + (bitbuf & BITMASK(2));
				bitbuf >>= 2;
				bitsleft -= 2;
				d->u.l.lens[i + 0] = rep_val;
				d->u.l.lens[i + 1] = rep_val;
				d->u.l.lens[i + 2] = rep_val;
				d->u.l.lens[i + 3] = rep_val;
				d->u.l.lens[i + 4] = rep_val;
				d->u.l.lens[i + 5] = rep_val;
				i += rep_count;
			} else if (presym == 17) {
				/* Repeat zero 3 - 10 times. */
				STATIC_ASSERT(3 + BITMASK(3) == 10);
				rep_count = 3 + (bitbuf & BITMASK(3));
				bitbuf >>= 3;
				bitsleft -= 3;
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
				STATIC_ASSERT(11 + BITMASK(7) == 138);
				rep_count = 11 + (bitbuf & BITMASK(7));
				bitbuf >>= 7;
				bitsleft -= 7;
				memset(&d->u.l.lens[i], 0,
				       rep_count * sizeof(d->u.l.lens[i]));
				i += rep_count;
			}
		} while (i < num_litlen_syms + num_offset_syms);

	} else if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {
		/*
		 * Uncompressed block: copy 'len' bytes literally from the input
		 * buffer to the output buffer.
		 */

		bitsleft -= 3; /* for BTYPE and BFINAL */

		/*
		 * Align the bitstream to the next byte boundary.  This means
		 * the next byte boundary as if we were reading a byte at a
		 * time.  Therefore, we have to rewind 'in_next' by any bytes
		 * that have been refilled but not actually consumed yet (not
		 * counting overread bytes, which don't increment 'in_next').
		 */
		SAFETY_CHECK(overread_count <= (bitsleft >> 3));
		in_next -= (bitsleft >> 3) - overread_count;
		overread_count = 0;
		bitbuf = 0;
		bitsleft = 0;

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

		bitbuf >>= 3; /* for BTYPE and BFINAL */
		bitsleft -= 3;

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
	litlen_tablemask = (1 << d->litlen_tablebits) - 1;

	/*
	 * This is the "fastloop" for decoding literals and matches.  It does
	 * bounds checks on in_next and out_next in the loop conditions so that
	 * additional bounds checks aren't needed inside the loop body.
	 *
	 * The fastloop also uses an optimization where bits 8 and higher of
	 * 'bitsleft' are allowed to contain garbage.  This is sometimes a
	 * useful microoptimization because it means the whole 32-bit decode
	 * table entry can be subtracted from 'bitsleft' without an intermediate
	 * step to convert it to 8 bits.  (It still needs to be converted to 8
	 * bits for the shift of 'bitbuf', but most CPUs ignore high bits in
	 * shift amounts, so that happens implicitly with zero overhead.)
	 *
	 * Finally, to reduce latency, the bitbuffer is refilled and the next
	 * litlen decode table entry is preloaded before each loop iteration.
	 */
	if (in_next >= in_fastloop_end || out_next >= out_fastloop_end)
		goto generic_loop;
	REFILL_BITS_IN_FASTLOOP();
	entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
	do {
		u32 length, offset;
		const u8 *src;
		u8 *dst;

		saved_bitbuf = bitbuf;
		bitbuf >>= (u8)entry;
		bitsleft -= entry; /* optimization: subtract full entry */

		/*
		 * On 64-bit, we start with a fast path for decoding literals
		 * that don't need subtables.  We do up to 2 of these before
		 * proceeding to the general case.  We could actually do up to 3
		 * and still be guaranteed to have enough bits left for what
		 * follows (assuming LITLEN_TABLEBITS=11), but on typical data
		 * that actually decreases performance -- probably due to the
		 * effect it has on the branch prediction of the conditional
		 * refill when decoding the match offset.
		 *
		 * Note: the definitions of FASTLOOP_MAX_BYTES_WRITTEN and
		 * FASTLOOP_MAX_BYTES_READ need to be updated if the maximum
		 * number of literals decoded here is changed.
		 */
		if (/* enough bits for 2 fast literals + length + offset preload */
		    CAN_DECODE_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS +
						LENGTH_MAXBITS,
						OFFSET_TABLEBITS) &&
		    /* enough bits for 2 fast literals + slow literal + litlen preload */
		    CAN_DECODE_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS +
						DEFLATE_MAX_LITLEN_CODEWORD_LEN,
						LITLEN_TABLEBITS) &&
		    (entry & HUFFDEC_LITERAL)) {
			*out_next++ = entry >> 16;
			entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
			saved_bitbuf = bitbuf;
			bitbuf >>= (u8)entry;
			bitsleft -= entry; /* optimization: subtract full entry */
			if (entry & HUFFDEC_LITERAL) {
				*out_next++ = entry >> 16;
				entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
				saved_bitbuf = bitbuf;
				bitbuf >>= (u8)entry;
				bitsleft -= entry; /* optimization: subtract full entry */
			}
		}
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			/* Subtable pointer or end-of-block entry */
			if (entry & HUFFDEC_SUBTABLE_POINTER) {
				entry = d->u.litlen_decode_table[(entry >> 16) +
						(bitbuf & BITMASK((entry >> 8) & 0xF))];
				saved_bitbuf = bitbuf;
				bitbuf >>= (u8)entry;
				bitsleft -= entry; /* optimization: subtract full entry */
			}
			if (unlikely(entry & HUFFDEC_END_OF_BLOCK)) {
				bitsleft = (u8)bitsleft;
				goto block_done;
			}
			if (!CAN_DECODE_AND_THEN_PRELOAD(LENGTH_MAXBITS,
							 OFFSET_TABLEBITS))
				REFILL_BITS_IN_FASTLOOP();
		} else {
			/* No refill needed until after preloading offset */
			STATIC_ASSERT(CAN_DECODE_AND_THEN_PRELOAD(
				LITLEN_TABLEBITS + DEFLATE_MAX_EXTRA_LENGTH_BITS,
				OFFSET_TABLEBITS));
		}

		/* Literal or length entry */
		length = entry >> 16;
		if (entry & HUFFDEC_LITERAL) {
			/*
			 * Handle a literal that didn't get handled by the
			 * literal fast path earlier.  After doing so, preload
			 * the next litlen decode table entry and refill the
			 * bitbuffer.  To reduce latency, we've arranged for
			 * there to be enough bits remaining to do the table
			 * preload independently of the refill.
			 */
			STATIC_ASSERT(CAN_DECODE_AND_THEN_PRELOAD(
					LITLEN_TABLEBITS, LITLEN_TABLEBITS));
			*out_next++ = length;
			entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
			REFILL_BITS_IN_FASTLOOP();
			continue;
		}
		/*
		 * Match length.  Finish decoding it.  We don't need to check
		 * for too-long matches here, as this is inside the fastloop
		 * where it's already been verified that the output buffer has
		 * enough space remaining to copy a max-length match.
		 */
		length += (saved_bitbuf & BITMASK((u8)entry)) >> (u8)(entry >> 8);

		/* Decode the match offset. */
		entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
		if (CAN_DECODE_AND_THEN_PRELOAD(OFFSET_MAXBITS, LITLEN_TABLEBITS)) {
			/*
			 * 64-bit.  We may need to refill once, but then we can
			 * decode the whole offset as well as preload the next
			 * litlen decode table entry.
			 */
			if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
				/* Offset codeword requires a subtable */
				if (unlikely((u8)bitsleft < OFFSET_MAXBITS +
					     (LITLEN_TABLEBITS - PRELOAD_SLACK)))
					REFILL_BITS_IN_FASTLOOP();
				bitbuf >>= OFFSET_TABLEBITS;
				bitsleft -= OFFSET_TABLEBITS;
				entry = d->offset_decode_table[(entry >> 16) +
					(bitbuf & BITMASK((entry >> 8) & 0xF))];
			} else if (unlikely((u8)bitsleft < OFFSET_TABLEBITS +
					    DEFLATE_MAX_EXTRA_OFFSET_BITS +
					    (LITLEN_TABLEBITS - PRELOAD_SLACK)))
				REFILL_BITS_IN_FASTLOOP();
		} else {
			/* 32-bit */
			STATIC_ASSERT(
				CAN_DECODE(MAX(OFFSET_TABLEBITS,
					       DEFLATE_MAX_OFFSET_CODEWORD_LEN -
						OFFSET_TABLEBITS) +
					   DEFLATE_MAX_EXTRA_OFFSET_BITS));
			REFILL_BITS_IN_FASTLOOP();
			if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
				/* Offset codeword requires a subtable */
				bitbuf >>= OFFSET_TABLEBITS;
				bitsleft -= OFFSET_TABLEBITS;
				entry = d->offset_decode_table[(entry >> 16) +
					(bitbuf & BITMASK((entry >> 8) & 0xF))];
				REFILL_BITS_IN_FASTLOOP();
			}
		}
		saved_bitbuf = bitbuf;
		bitbuf >>= (u8)entry;
		bitsleft -= entry; /* optimization: subtract full entry */
		offset = entry >> 16;
		offset += (saved_bitbuf & BITMASK((u8)entry)) >> (u8)(entry >> 8);

		/* Validate the match offset; needed even in the fastloop. */
		SAFETY_CHECK(offset <= out_next - (const u8 *)out);
		src = out_next - offset;
		dst = out_next;
		out_next += length;

		/*
		 * Before starting to issue the instructions to copy the match,
		 * refill the bitbuffer and preload the litlen decode table
		 * entry for the next loop iteration.  This can increase
		 * performance by allowing the latency of the match copy to
		 * overlap with these other operations.
		 *
		 * Usually enough bits remain to do the preload without
		 * depending on the refill.  Reduce latency by using these bits.
		 */
		if (!CAN_DECODE_AND_THEN_PRELOAD(
			MAX(OFFSET_TABLEBITS,
			    DEFLATE_MAX_OFFSET_CODEWORD_LEN - OFFSET_TABLEBITS) +
			DEFLATE_MAX_EXTRA_OFFSET_BITS,
			LITLEN_TABLEBITS) &&
		    unlikely((u8)bitsleft < LITLEN_TABLEBITS - PRELOAD_SLACK))
			REFILL_BITS_IN_FASTLOOP();
		entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
		REFILL_BITS_IN_FASTLOOP();

		/*
		 * Copy the match.  On most CPUs the fastest method is a
		 * word-at-a-time copy, unconditionally copying about 4 words
		 * since this is enough for most matches without being too much.
		 *
		 * The normal word-at-a-time copy works for offset >= WORDBYTES,
		 * which is most cases.  The case of offset == 1 is also common
		 * and is worth optimizing for, since it is just RLE encoding of
		 * the previous byte, which is the result of compressing long
		 * runs of the same byte.
		 */
		if (UNALIGNED_ACCESS_IS_FAST && offset >= WORDBYTES) {
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			do {
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
			} while (dst < out_next);
		} else if (UNALIGNED_ACCESS_IS_FAST && offset == 1) {
			machine_word_t v;

			v = (machine_word_t)0x0101010101010101 * src[0];
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			do {
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
			} while (dst < out_next);
		} else if (UNALIGNED_ACCESS_IS_FAST) {
			store_word_unaligned(load_word_unaligned(src), dst);
			src += offset;
			dst += offset;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += offset;
			dst += offset;
			do {
				store_word_unaligned(load_word_unaligned(src), dst);
				src += offset;
				dst += offset;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += offset;
				dst += offset;
			} while (dst < out_next);
		} else {
			*dst++ = *src++;
			*dst++ = *src++;
			do {
				*dst++ = *src++;
			} while (dst < out_next);
		}
	} while (in_next < in_fastloop_end && out_next < out_fastloop_end);

	/* Clear any garbage from the high bits of 'bitsleft'. */
	bitsleft = (u8)bitsleft;

	/*
	 * This is the generic loop for decoding literals and matches.  This
	 * handles cases where in_next and out_next are close to the end of
	 * their respective buffers.  Usually this loop isn't performance-
	 * critical, as most time is spent in the fastloop above instead.  We
	 * therefore omit some optimizations here in favor of smaller code.
	 */
generic_loop:
	for (;;) {
		u32 length, offset;
		const u8 *src;
		u8 *dst;

		REFILL_BITS();
		entry = d->u.litlen_decode_table[bitbuf & litlen_tablemask];
		saved_bitbuf = bitbuf;
		bitbuf >>= (u8)entry;
		bitsleft -= (u8)entry;
		if (unlikely(entry & HUFFDEC_SUBTABLE_POINTER)) {
			entry = d->u.litlen_decode_table[(entry >> 16) +
					(bitbuf & BITMASK((entry >> 8) & 0xF))];
			saved_bitbuf = bitbuf;
			bitbuf >>= (u8)entry;
			bitsleft -= (u8)entry;
		}
		length = entry >> 16;
		if (entry & HUFFDEC_LITERAL) {
			if (unlikely(out_next == out_end))
				return LIBDEFLATE_INSUFFICIENT_SPACE;
			*out_next++ = length;
			continue;
		}
		if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
			goto block_done;
		length += (saved_bitbuf & BITMASK((u8)entry)) >> (u8)(entry >> 8);
		if (unlikely(length > out_end - out_next))
			return LIBDEFLATE_INSUFFICIENT_SPACE;

		if (!CAN_DECODE(LENGTH_MAXBITS + OFFSET_MAXBITS))
			REFILL_BITS();
		entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			bitbuf >>= OFFSET_TABLEBITS;
			bitsleft -= OFFSET_TABLEBITS;
			entry = d->offset_decode_table[(entry >> 16) +
					(bitbuf & BITMASK((entry >> 8) & 0xF))];
			if (!CAN_DECODE(OFFSET_MAXBITS))
				REFILL_BITS();
		}
		offset = entry >> 16;
		offset += (bitbuf & BITMASK((u8)entry)) >> (u8)(entry >> 8);
		bitbuf >>= (u8)entry;
		bitsleft -= (u8)entry;

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
	/* Finished decoding a block */

	if (!is_final_block)
		goto next_block;

	/* That was the last block. */

	/*
	 * If any of the implicit appended zero bytes were consumed (not just
	 * refilled) before hitting end of stream, then the data is bad.
	 */
	SAFETY_CHECK(overread_count <= (bitsleft >> 3));

	/* Optionally return the actual number of bytes consumed. */
	if (actual_in_nbytes_ret) {
		/* Don't count bytes that were refilled but not consumed. */
		in_next -= (bitsleft >> 3) - overread_count;

		*actual_in_nbytes_ret = in_next - (u8 *)in;
	}

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
