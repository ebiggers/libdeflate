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
 * target instruction sets.  The decode table building functions are included
 * here too, since they also benefit slightly from some of these instructions
 * (e.g. shlx, shrx, and lzcnt on x86).
 */

#ifndef ATTRIBUTES
#  define ATTRIBUTES
#endif
/*
 * EXTRACT_VARBITS(word, count) extracts the low 'count' bits of 'word' as a
 * u32, where 0 <= count < 32.
 *
 * EXTRACT_VARBITS8(word, count) extracts the low '(u8)count' bits of 'word' as
 * a u32, where (u8)count < 32 (i.e. bits 7-5 of 'count' must be 0, while bits
 * 8+ are ignored).  Note: the x86 BMI2 override uses bzhi, which reads bits 7-0
 * of 'count', whereas the generic fallback masks 'count' by 0x1F so the shift
 * count is provably in 0..31 without an explicit mask instruction.
 */
#ifndef EXTRACT_VARBITS
#  define EXTRACT_VARBITS(word, count)	((u32)(word) & (((u32)1 << (count)) - 1))
#endif
#ifndef EXTRACT_VARBITS8
#  define EXTRACT_VARBITS8(word, count)	((u32)(word) & (((u32)1 << ((count) & 0x1F)) - 1))
#endif

/*
 * Fill all compound litlen main-table entries whose two codewords have combined
 * length 'total_cw_len' (2-literal pairs and fused literal+length pairs for
 * length symbols in [FIRST_SHORT_LENSYM, LAST_SHORT_LENSYM]), writing at index
 * '(cw2 << len1) | cw1' in 'decode_table[0 .. (1 << total_cw_len) - 1]'.
 *
 * When this is called, 'decode_table[0 .. (1 << total_cw_len) - 1]' already
 * contains all single-symbol entries for codewords of length <= total_cw_len,
 * and 'short_lits' and 'cs' already record every eligible literal and short
 * length symbol of codeword length < total_cw_len (which covers all pairs
 * summing to 'total_cw_len', since len1 >= 1 and len2 >= 1).  See
 * build_decode_table() for how this fits into incremental table doubling.
 *
 * This is noinline since it's called from several places in
 * build_decode_table() but isn't needed at all for most calls.
 */
static NOINLINE ATTRIBUTES void
ADD_SUFFIX(fill_compound_litlen_entries)(u32 * restrict decode_table,
					 const u32 * restrict short_lits,
					 const struct litlen_compound_syms * restrict cs,
					 unsigned total_cw_len)
{
	const unsigned min_lit_cw_len = cs->min_lit_cw_len;
	const unsigned min_lensym_cw_len = cs->min_lensym_cw_len;
	unsigned len1;

	STATIC_ASSERT(FIRST_SHORT_LENSYM == DEFLATE_FIRST_LEN_SYM);
	STATIC_ASSERT(ARRAY_LEN(cs->lit_start) == SHORT_LIT_MAX_CW_LEN + 1);
	STATIC_ASSERT(ARRAY_LEN(cs->lit_end) == SHORT_LIT_MAX_CW_LEN + 1);
	STATIC_ASSERT(ARRAY_LEN(cs->lensym_start) == SHORT_LIT_MAX_CW_LEN + 1);
	STATIC_ASSERT(ARRAY_LEN(cs->lensym_end) == SHORT_LIT_MAX_CW_LEN + 1);
	STATIC_ASSERT(HUFFDEC_LITERAL2_SHIFT == HUFFDEC_LITERAL1_SHIFT + 8);
	STATIC_ASSERT(HUFFDEC_LIT_LENGTH == HUFFDEC_END_OF_BLOCK);
	STATIC_ASSERT(((HUFFDEC_LIT_LENGTH >> 8) & 0x1F) == 0);
	STATIC_ASSERT((SHORT_LIT_CODEWORD_MASK &
		       (HUFFDEC_LITERAL1_MASK | HUFFDEC_LITERAL2_MASK)) == 0);
	STATIC_ASSERT(((HUFFDEC_LITERAL1_MASK | HUFFDEC_LITERAL2_MASK) &
		       (HUFFDEC_LITERAL | HUFFDEC_SUBTABLE_POINTER |
			HUFFDEC_END_OF_BLOCK | HUFFDEC_DOUBLE_LITERAL |
			0xFF)) == 0);
	STATIC_ASSERT((HUFFDEC_LITERAL1_MASK & HUFFDEC_LITERAL2_MASK) == 0);
	STATIC_ASSERT((HUFFDEC_LITERAL2_MASK &
		       (SHORT_LENSYM_BASE_MASK << 16)) == 0);
	STATIC_ASSERT(1 + SHORT_LENSYM_MAX_MATCH_LEN <= DEFLATE_MAX_MATCH_LEN);
	ASSERT(min_lit_cw_len >= 1 && total_cw_len <= LITLEN_TABLEBITS);

	/*
	 * Generate 2-literal entries for all (len1, len2) summing to
	 * 'total_cw_len'.  Loop over the 2nd literal on the outside so that
	 * '&decode_table[cw2 << len1]' and the upper entry bits 'base' are
	 * hoisted out of the inner loop over the 1st literal; 'dst[cw1]' then
	 * writes 'decode_table[(cw2 << len1) | cw1]'.
	 *
	 * Note: 'short_lits[]' already stores each literal byte shifted to
	 * HUFFDEC_LITERAL1_SHIFT above SHORT_LIT_CODEWORD_MASK, so masking off
	 * SHORT_LIT_CODEWORD_MASK leaves the 1st literal in place, and shifting
	 * left by 8 moves the 2nd literal to HUFFDEC_LITERAL2_SHIFT.
	 */
	if (total_cw_len >= 2 * min_lit_cw_len) {
		for (len1 = min_lit_cw_len;
		     len1 <= total_cw_len - min_lit_cw_len; len1++) {
			unsigned len2 = total_cw_len - len1;
			unsigned s1 = cs->lit_start[len1];
			unsigned e1 = cs->lit_end[len1];
			unsigned i2, e2 = cs->lit_end[len2];

			if (s1 >= e1)
				continue;
			for (i2 = cs->lit_start[len2]; i2 < e2; i2++) {
				u32 lit2 = short_lits[i2];
				u32 *dst = &decode_table[
					(lit2 & SHORT_LIT_CODEWORD_MASK) << len1];
				u32 base = HUFFDEC_LITERAL |
					   HUFFDEC_DOUBLE_LITERAL |
					   ((lit2 & ~SHORT_LIT_CODEWORD_MASK) <<
					    (HUFFDEC_LITERAL2_SHIFT -
					     HUFFDEC_LITERAL1_SHIFT)) |
					   total_cw_len;
				unsigned i1;

				for (i1 = s1; i1 < e1; i1++) {
					u32 lit1 = short_lits[i1];

					dst[lit1 & SHORT_LIT_CODEWORD_MASK] =
						base |
						(lit1 & ~SHORT_LIT_CODEWORD_MASK);
				}
			}
		}
	}

	/*
	 * Generate fused (literal, length) entries in the same way.
	 *
	 * 'cs->short_lensym_res[i2]' is the litlen_decode_results[] template
	 * '(length_base << 16) | num_extra_bits' (with length_base <= 115 in
	 * bits 22-16 and num_extra_bits <= 4 in bits 2-0).  Adding
	 * '(total_cw_len << 8) + total_cw_len' (just like
	 * make_decode_table_entry() does for plain length entries) puts
	 * 'total_cw_len' in bits 11-8 and 'total_cw_len + num_extra_bits' in
	 * bits 4-0; OR'ing HUFFDEC_LIT_LENGTH sets bit 13, and shifting the
	 * 1st literal by 8 places it in bits 30-23 (HUFFDEC_LITERAL2_SHIFT)
	 * above 'length_base'.
	 */
	if (total_cw_len >= min_lit_cw_len + min_lensym_cw_len) {
		for (len1 = min_lit_cw_len;
		     len1 <= total_cw_len - min_lensym_cw_len; len1++) {
			unsigned len2 = total_cw_len - len1;
			unsigned s1 = cs->lit_start[len1];
			unsigned e1 = cs->lit_end[len1];
			unsigned i2, e2 = cs->lensym_end[len2];

			if (s1 >= e1)
				continue;
			for (i2 = cs->lensym_start[len2]; i2 < e2; i2++) {
				u32 *dst = &decode_table[
					(unsigned)cs->short_lensym_cw[i2] << len1];
				u32 base = HUFFDEC_LIT_LENGTH |
					   (cs->short_lensym_res[i2] +
					    (total_cw_len << 8) + total_cw_len);
				unsigned i1;

				for (i1 = s1; i1 < e1; i1++) {
					u32 lit1 = short_lits[i1];

					dst[lit1 & SHORT_LIT_CODEWORD_MASK] =
						base |
						((lit1 & ~SHORT_LIT_CODEWORD_MASK) <<
						 (HUFFDEC_LITERAL2_SHIFT -
						  HUFFDEC_LITERAL1_SHIFT));
				}
			}
		}
	}
}

/*
 * Build a table for fast decoding of symbols from a Huffman code.  As input,
 * this function takes the codeword length of each symbol which may be used in
 * the code.  As output, it produces a decode table for the canonical Huffman
 * code described by the codeword lengths.  The decode table is built with the
 * assumption that it will be indexed with "bit-reversed" codewords, where the
 * low-order bit is the first bit of the codeword.  This format is used for all
 * Huffman codes in DEFLATE.
 *
 * @decode_table
 *	The array in which the decode table will be generated.  This array must
 *	have sufficient length; see the definition of the ENOUGH numbers.
 * @lens
 *	An array which provides, for each symbol, the length of the
 *	corresponding codeword in bits, or 0 if the symbol is unused.  This may
 *	alias @decode_table, since nothing is written to @decode_table until all
 *	@lens have been consumed.  All codeword lengths are assumed to be <=
 *	@max_codeword_len but are otherwise considered untrusted.  If they do
 *	not form a valid Huffman code, then the decode table is not built and
 *	%false is returned.
 * @num_syms
 *	The number of symbols in the code, including all unused symbols.
 * @decode_results
 *	An array which gives the incomplete decode result for each symbol.  The
 *	needed values in this array will be combined with codeword lengths to
 *	make the final decode table entries using make_decode_table_entry().
 * @table_bits
 *	The log base-2 of the number of main table entries to use, or the
 *	maximum value if @table_bits_ret != NULL.  Must be LITLEN_TABLEBITS if
 *	@allow_compound is true, or <= LITLEN_SMALL_TABLEBITS if
 *	@table_bits_ret != NULL and @allow_compound is false.
 * @max_codeword_len
 *	The maximum allowed codeword length for this Huffman code.
 *	Must be <= DEFLATE_MAX_CODEWORD_LEN.
 * @sorted_syms
 *	A temporary array of length @num_syms.
 * @table_bits_ret
 *	If non-NULL, indicates the litlen decode table: dynamic table_bits is
 *	enabled, and the actual table_bits value is returned here.  When
 *	compound entries are generated, this is
 *	min(LITLEN_TABLEBITS, max(max_codeword_len, longest_pair)), where
 *	longest_pair is the length of the longest (lit, lit) or fused
 *	(lit, len) codeword pair <= LITLEN_TABLEBITS; otherwise this is
 *	min(@table_bits, LITLEN_SMALL_TABLEBITS, max_codeword_len).
 * @allow_compound
 *	If true (only allowed when @table_bits_ret != NULL), also generate
 *	compound (2-literal and fused literal+length) entries using scratch
 *	space at &decode_table[1U << LITLEN_TABLEBITS].
 *
 * Returns %true if successful; %false if the codeword lengths do not form a
 * valid Huffman code.
 */
static ATTRIBUTES bool
ADD_SUFFIX(build_decode_table)(u32 decode_table[],
			       const u8 lens[],
			       const unsigned num_syms,
			       const u32 decode_results[],
			       unsigned table_bits,
			       unsigned max_codeword_len,
			       u16 *sorted_syms,
			       unsigned *table_bits_ret,
			       bool allow_compound)
{
	union {
		u16 c[4];
		u64 w;
	} part_counts[DEFLATE_MAX_CODEWORD_LEN + 1];
	/* Initialized only to avoid a gcc -Wmaybe-uninitialized warning */
	u16 len_counts[DEFLATE_MAX_CODEWORD_LEN + 1] = { 0 };
	const unsigned part_len = num_syms / 4;
	unsigned sym;		/* current symbol */
	unsigned codeword;	/* current codeword, bit-reversed */
	unsigned len;		/* current codeword length in bits */
	unsigned count;		/* num codewords remaining with this length */
	u32 codespace_used;	/* codespace used out of '2^max_codeword_len' */
	unsigned cur_table_end; /* end index of current table */
	unsigned subtable_prefix; /* codeword prefix of current subtable */
	unsigned subtable_start;  /* start index of current subtable */
	unsigned subtable_bits;   /* log2 of current subtable length */
	struct litlen_compound_syms cs;
	u32 *short_lits = NULL;
	unsigned num_short_lits = 0;
	unsigned num_short_lensyms = 0;
	unsigned min_compound_cw_len = LITLEN_TABLEBITS + 1;
	unsigned max_short_lit_cw_len = 0;
	unsigned max_short_lensym_cw_len = 0;

	ASSERT(!allow_compound || table_bits_ret != NULL);

	/*
	 * Count how many codewords have each length, including 0.
	 *
	 * Consecutive symbols often have the same length, which would make a
	 * simple counting loop a long chain of dependent increments of the same
	 * counter.  To avoid that, split the symbols into 4 contiguous parts
	 * (the last one also gets any remainder), and count the parts
	 * separately in an interleaved way.
	 */
	memset(part_counts, 0, sizeof(part_counts));
	for (sym = 0; sym < part_len; sym++) {
		part_counts[lens[0 * part_len + sym]].c[0]++;
		part_counts[lens[1 * part_len + sym]].c[1]++;
		part_counts[lens[2 * part_len + sym]].c[2]++;
		part_counts[lens[3 * part_len + sym]].c[3]++;
	}
	for (sym = 4 * part_len; sym < num_syms; sym++)
		part_counts[lens[sym]].c[3]++;

	/* Determine the actual maximum codeword length that was used. */
	while (max_codeword_len > 1 && part_counts[max_codeword_len].w == 0)
		max_codeword_len--;

	/*
	 * Make part_counts[] cumulative, in preparation for symbol sorting
	 * (again using 4-way interleaving for performance).
	 *
	 * While doing so, also compute len_counts[] and codespace_used.
	 *
	 * Statically assert that codespace_used cannot overflow.
	 */
	STATIC_ASSERT(sizeof(codespace_used) == 4);
	STATIC_ASSERT(UINT32_MAX / (1U << (DEFLATE_MAX_CODEWORD_LEN - 1)) >=
		      DEFLATE_MAX_NUM_SYMS);
	codespace_used = 0;
	{
		unsigned a = part_counts[0].c[0];
		unsigned b = part_counts[0].c[1];
		unsigned c = part_counts[0].c[2];
		unsigned d = part_counts[0].c[3];

		part_counts[0].c[0] = 0;
		part_counts[0].c[1] = a;
		part_counts[0].c[2] = a + b;
		part_counts[0].c[3] = a + b + c;
		len_counts[0] = a + b + c + d;
		count = len_counts[0];
	}
	for (len = 1; len <= max_codeword_len; len++) {
		unsigned a = part_counts[len].c[0];
		unsigned b = part_counts[len].c[1];
		unsigned c = part_counts[len].c[2];
		unsigned d = part_counts[len].c[3];

		part_counts[len].c[0] = count;
		part_counts[len].c[1] = count + a;
		part_counts[len].c[2] = count + a + b;
		part_counts[len].c[3] = count + a + b + c;
		len_counts[len] = a + b + c + d;
		count += len_counts[len];
		codespace_used = (codespace_used << 1) + len_counts[len];
	}

	/*
	 * Sort the symbols primarily by increasing codeword length and
	 * secondarily by increasing symbol value; or equivalently by their
	 * codewords in lexicographic order, since a canonical code is assumed.
	 */
	for (sym = 0; sym < part_len; sym++) {
		sorted_syms[part_counts[lens[0 * part_len + sym]].c[0]++] =
			0 * part_len + sym;
		sorted_syms[part_counts[lens[1 * part_len + sym]].c[1]++] =
			1 * part_len + sym;
		sorted_syms[part_counts[lens[2 * part_len + sym]].c[2]++] =
			2 * part_len + sym;
		sorted_syms[part_counts[lens[3 * part_len + sym]].c[3]++] =
			3 * part_len + sym;
	}
	for (sym = 4 * part_len; sym < num_syms; sym++)
		sorted_syms[part_counts[lens[sym]].c[3]++] = sym;
	sorted_syms += len_counts[0]; /* Skip unused symbols */

	/* lens[] is done being used, so we can write to decode_table[] now. */

	/*
	 * Check whether the lengths form a complete code (exactly fills the
	 * codespace), an incomplete code (doesn't fill the codespace), or an
	 * overfull code (overflows the codespace).  A codeword of length 'n'
	 * uses proportion '1/(2^n)' of the codespace.  An overfull code is
	 * nonsensical, so is considered invalid.  An incomplete code is
	 * considered valid only in two specific cases; see below.
	 */

	/* overfull code? */
	if (unlikely(codespace_used > (1U << max_codeword_len)))
		return false;

	/* incomplete code? */
	if (unlikely(codespace_used < (1U << max_codeword_len))) {
		u32 entry;
		unsigned i;

		/*
		 * The DEFLATE RFC explicitly allows the offset code to be
		 * incomplete in two cases: a code containing just 1 codeword,
		 * if that codeword has length 1; and a code containing no
		 * codewords.  Note: the list of offset codeword lengths is
		 * always nonempty, but lengths of 0 don't count as codewords.
		 *
		 * The RFC doesn't say whether the same cases are allowed for
		 * the litlen and pre codes.  It's actually impossible for no
		 * symbols to be used from these codes; however, it's
		 * technically possible for only one symbol to be used.  zlib
		 * allows 1 codeword for the litlen code, but not the precode.
		 * The RFC also doesn't say whether, when there is 1 codeword,
		 * that codeword is '0' or '1'.  zlib uses '0'.
		 *
		 * We accept what zlib accepts, plus a bit more.  First, we
		 * don't treat the precode more strictly than the litlen and
		 * offset codes.  There's no convincing reason to add a special
		 * case for the precode here.
		 *
		 * Second, we just map each allowed incomplete code to a complete
		 * code with only real symbols.  To do this, we choose a symbol,
		 * either the used symbol (for codes with 1 codeword) or an
		 * arbitrary symbol (for empty codes), and give it both
		 * codewords '0' and '1'.  zlib instead uses a special ERROR
		 * symbol in the part of the codespace the code doesn't use.
		 * However, having an ERROR symbol reduces the performance of
		 * the Huffman decoder, for no real benefit.  Our approach also
		 * avoids having to decide whether '0' or '1' is correct.
		 *
		 * Like zlib, we still reject all incomplete codes that contain
		 * more than 1 codeword or a codeword length greater than 1.
		 */
		if (codespace_used == 0) {
			sym = 0; /* arbitrary */
		} else {
			/*
			 * max_codeword_len is the longest length used, and an
			 * incomplete code whose codewords all have length 1 has
			 * exactly 1 codeword.
			 */
			if (max_codeword_len != 1)
				return false;
			sym = sorted_syms[0];
		}
		ASSERT(max_codeword_len == 1);
		if (table_bits_ret != NULL) {
			table_bits = 1;
			*table_bits_ret = 1;
		}
		entry = make_decode_table_entry(decode_results, sym, 1);
		for (i = 0; i < (1U << table_bits); i++)
			decode_table[i] = entry;
		return true;
	}

	/*
	 * The lengths form a complete code.  Now, enumerate the codewords in
	 * lexicographic order and fill the decode table entries for each one.
	 *
	 * First, process all codewords with len <= table_bits.  Each one gets
	 * '2^(table_bits-len)' direct entries in the table.
	 *
	 * Since DEFLATE uses bit-reversed codewords, these entries aren't
	 * consecutive but rather are spaced '2^len' entries apart.  This makes
	 * filling them naively somewhat awkward and inefficient, since strided
	 * stores are less cache-friendly and preclude the use of word or
	 * vector-at-a-time stores to fill multiple entries per instruction.
	 *
	 * To optimize this, we incrementally double the table size.  When
	 * processing codewords with length 'len', the table is treated as
	 * having only '2^len' entries, so each codeword uses just one entry.
	 * Then, each time 'len' is incremented, the table size is doubled and
	 * the first half is copied to the second half.  This significantly
	 * improves performance over naively doing strided stores.
	 *
	 * Note that some entries copied for each table doubling may not have
	 * been initialized yet, but it doesn't matter since they're guaranteed
	 * to be initialized later (because the Huffman code is complete).
	 *
	 * When 'allow_compound' is true (for the litlen table), compound
	 * 2-symbol entries -- (literal, literal) and fused (literal, length) --
	 * are piggybacked onto this same incremental doubling process:
	 *
	 *   - A literal 's1' with bit-reversed codeword 'cw1' of length 'len1'
	 *     has its single-literal entry written at decode_table[cw1] when
	 *     the active table has size '2^len1'.  Later doublings copy it to
	 *     all indices '(x << len1) | cw1'.
	 *
	 *   - When 's1' is followed by an eligible 2nd symbol 's2' (another
	 *     literal, or a length symbol in FIRST_SHORT_LENSYM ..
	 *     LAST_SHORT_LENSYM) with bit-reversed codeword 'cw2' of length
	 *     'len2' and 'L = len1 + len2 <= table_bits', the pair's L-bit
	 *     LSB-first prefix is '(cw2 << len1) | cw1'.
	 *
	 *   - As codewords are enumerated by increasing length, eligible
	 *     literals and length symbols are saved in 'short_lits[]' and 'cs',
	 *     grouped by length.  Once the active table has size '2^L' and all
	 *     single-symbol codewords of length <= L have been written,
	 *     fill_compound_litlen_entries(..., L) writes one compound entry at
	 *     'decode_table[(cw2 << len1) | cw1]' for each pair with
	 *     'len1 + len2 == L' (all already recorded since len1, len2 < L).
	 *
	 *   - This overwrites the replicated single-literal entry for 's1' at
	 *     '(cw2 << len1) | cw1' while leaving it intact at all other
	 *     '(x << len1) | cw1' indices (e.g. where the next symbol is EOB,
	 *     > LAST_SHORT_LENSYM, or too long to fit in 'table_bits').  Later
	 *     doublings up to '2^table_bits' then replicate the compound entry
	 *     for free, so each compound pair needs only a single store.
	 *     Because the Huffman code is prefix-free, no later write can
	 *     overwrite a compound entry.
	 */
	codeword = 0;
	len = 1;
	while ((count = len_counts[len]) == 0)
		len++;
	if (table_bits_ret != NULL) {
		ASSERT(table_bits <= LITLEN_TABLEBITS);

		if (allow_compound) {
			/*
			 * Pre-scan the symbol lengths to check whether any
			 * compound entries fit in LITLEN_TABLEBITS bits,
			 * choose the final 'table_bits', and find the maximum
			 * codeword lengths that need to be saved.
			 *
			 * Step 1: Build bitmasks of codeword lengths 'l <=
			 * table_bits - 1' that contain at least one literal
			 * (0..255) or fusable length symbol (257..280).  Within
			 * each length 'l', 'sorted_syms[]' is sorted by symbol
			 * value, so length 'l' has a literal iff its 1st symbol
			 * is < 256, and a fusable length symbol iff its last
			 * symbol <= 280 is >= 257.
			 */
			unsigned sym_idx = 0;
			unsigned max_l = MIN(max_codeword_len, table_bits - 1);
			u32 lit_cw_len_mask = 0;
			u32 lensym_cw_len_mask = 0;
			unsigned min_lit_cw_len = LITLEN_TABLEBITS + 1;
			unsigned min_lensym_cw_len = LITLEN_TABLEBITS + 1;
			unsigned min_second_cw_len;
			unsigned l;

			ASSERT(table_bits == LITLEN_TABLEBITS);
			for (l = len; l <= max_l; l++) {
				unsigned c = len_counts[l];

				if (c != 0) {
					unsigned e = sym_idx + c;

					if (sorted_syms[sym_idx] <
					    DEFLATE_NUM_LITERALS)
						lit_cw_len_mask |= 1U << l;
					while (e > sym_idx &&
					       sorted_syms[e - 1] >
					       LAST_SHORT_LENSYM)
						e--;
					if (e > sym_idx &&
					    sorted_syms[e - 1] >=
					    FIRST_SHORT_LENSYM)
						lensym_cw_len_mask |= 1U << l;
					sym_idx += c;
				}
			}
			if (lit_cw_len_mask != 0)
				min_lit_cw_len = bsf32(lit_cw_len_mask);
			if (lensym_cw_len_mask != 0)
				min_lensym_cw_len = bsf32(lensym_cw_len_mask);
			min_second_cw_len = MIN(min_lit_cw_len,
						min_lensym_cw_len);

			/*
			 * Step 2: Every compound entry starts with a literal,
			 * so at least one compound entry fits iff
			 * 'min_lit_cw_len + min_second_cw_len <= table_bits'.
			 */
			if (lit_cw_len_mask != 0 &&
			    min_lit_cw_len + min_second_cw_len <= table_bits) {
				if (max_codeword_len < table_bits) {
					/*
					 * Shrink 'table_bits' to
					 * max(max_codeword_len, longest_pair)
					 * to avoid unnecessary table doublings.
					 * For each literal length 'l1', mask
					 * 'second_mask' to bits 0 ..
					 * (table_bits - l1) and use bsr32() to
					 * find the longest valid 2nd length.
					 */
					u32 second_mask = lit_cw_len_mask |
							  lensym_cw_len_mask;
					unsigned min_table_bits =
						max_codeword_len;
					u32 m = lit_cw_len_mask;

					do {
						unsigned l1 = bsf32(m);
						u32 partner_mask;

						if (l1 + min_second_cw_len >
						    table_bits)
							break;
						partner_mask =
							second_mask &
							((2U << (table_bits - l1)) - 1);
						ASSERT(partner_mask != 0);
						min_table_bits =
							MAX(min_table_bits,
							    l1 + bsr32(partner_mask));
						m &= m - 1;
					} while (m != 0);
					table_bits = min_table_bits;
				}
				/*
				 * Step 3: Compute the length bounds for saving
				 * symbols during the main-table loop, and
				 * initialize 'cs' and 'short_lits'.
				 *
				 * Note: 'min_second_cw_len <= min_lit_cw_len'
				 * implies 'max_short_lensym_cw_len <=
				 * max_short_lit_cw_len', so the codeword loop
				 * only needs to check the latter first.
				 *
				 * 'short_lits' uses up to 256 entries at
				 * '&decode_table[1U << LITLEN_TABLEBITS]', in
				 * the subtable space reserved by LITLEN_ENOUGH
				 * (unused until main-table building finishes).
				 */
				min_compound_cw_len =
					min_lit_cw_len + min_second_cw_len;
				max_short_lit_cw_len =
					table_bits - min_second_cw_len;
				if (lensym_cw_len_mask != 0 &&
				    min_lit_cw_len + min_lensym_cw_len <=
				    table_bits)
					max_short_lensym_cw_len =
						table_bits - min_lit_cw_len;
				ASSERT(max_short_lensym_cw_len <=
				       max_short_lit_cw_len);
				ASSERT(max_short_lit_cw_len <=
				       SHORT_LIT_MAX_CW_LEN);
				ASSERT(max_short_lensym_cw_len <=
				       SHORT_LIT_MAX_CW_LEN);
				short_lits =
					&decode_table[1U << LITLEN_TABLEBITS];
				cs.min_lit_cw_len = min_lit_cw_len;
				cs.min_lensym_cw_len = min_lensym_cw_len;
				memset(cs.lit_start, 0, sizeof(cs.lit_start));
				memset(cs.lit_end, 0, sizeof(cs.lit_end));
				memset(cs.lensym_start, 0,
				       sizeof(cs.lensym_start));
				memset(cs.lensym_end, 0, sizeof(cs.lensym_end));
			} else {
				/*
				 * No compound entries fit; keep the main table
				 * capped at LITLEN_SMALL_TABLEBITS to save
				 * build time and L1d cache footprint.
				 */
				table_bits = MIN(LITLEN_SMALL_TABLEBITS,
						 max_codeword_len);
			}
		} else {
			ASSERT(table_bits <= LITLEN_SMALL_TABLEBITS);
			table_bits = MIN(table_bits, max_codeword_len);
		}
		*table_bits_ret = table_bits;
	}
	cur_table_end = 1U << len;
	while (len <= table_bits) {
		if (len <= max_short_lit_cw_len)
			cs.lit_start[len] = num_short_lits;
		if (len <= max_short_lensym_cw_len)
			cs.lensym_start[len] = num_short_lensyms;

		/* Process all 'count' codewords with length 'len' bits. */
		do {
			unsigned bit;
			u32 entry;

			/*
			 * Fill the first entry for the current codeword, and if
			 * compound entries are enabled and 'sym' is short
			 * enough to participate in one, record its symbol and
			 * codeword for later use by
			 * fill_compound_litlen_entries().
			 */
			sym = *sorted_syms++;
			entry = make_decode_table_entry(decode_results, sym, len);
			decode_table[codeword] = entry;
			if (len <= max_short_lit_cw_len) {
				if (sym < DEFLATE_NUM_LITERALS) {
					ASSERT(codeword <= SHORT_LIT_CODEWORD_MASK);
					ASSERT(num_short_lits < DEFLATE_NUM_LITERALS);
					short_lits[num_short_lits++] =
						MAKE_SHORT_LIT(sym, codeword);
				} else if (len <= max_short_lensym_cw_len &&
					   sym >= FIRST_SHORT_LENSYM &&
					   sym <= LAST_SHORT_LENSYM) {
					ASSERT(codeword <= SHORT_LIT_CODEWORD_MASK);
					ASSERT(num_short_lensyms <
					       MAX_NUM_SHORT_LENSYMS);
					ASSERT((decode_results[sym] >> 16) <=
					       SHORT_LENSYM_BASE_MASK);
					ASSERT((decode_results[sym] & 0xFFFF) <= 4);
					cs.short_lensym_res[num_short_lensyms] =
						decode_results[sym];
					cs.short_lensym_cw[num_short_lensyms] =
						(u16)codeword;
					num_short_lensyms++;
				}
			}

			if (codeword == cur_table_end - 1) {
				/*
				 * Last codeword (all 1's): no single-symbol
				 * codewords of greater length remain, so
				 * 'len == max_codeword_len <= table_bits'.
				 * Note that 'len < table_bits' still occurs
				 * when 'table_bits' is fixed (precode/offset)
				 * or was expanded above 'max_codeword_len' to
				 * fit compound entries.  Fill any compound
				 * entries of combined length 'len', then finish
				 * doubling the table up to '2^table_bits',
				 * filling compound entries of each combined
				 * length 'len + 1' along the way.
				 */
				if (len <= max_short_lit_cw_len)
					cs.lit_end[len] = num_short_lits;
				if (len <= max_short_lensym_cw_len)
					cs.lensym_end[len] = num_short_lensyms;
				if (len >= min_compound_cw_len)
					ADD_SUFFIX(fill_compound_litlen_entries)(
							decode_table,
							short_lits, &cs, len);
				for (; len < table_bits; len++) {
					memcpy(&decode_table[cur_table_end],
					       decode_table,
					       cur_table_end *
						sizeof(decode_table[0]));
					cur_table_end <<= 1;
					if (len + 1 >= min_compound_cw_len)
						ADD_SUFFIX(fill_compound_litlen_entries)(
							decode_table,
							short_lits, &cs,
							len + 1);
				}
				return true;
			}
			/*
			 * To advance to the lexicographically next codeword in
			 * the canonical code, the codeword must be incremented,
			 * then 0's must be appended to the codeword as needed
			 * to match the next codeword's length.
			 *
			 * Since the codeword is bit-reversed, appending 0's is
			 * a no-op.  However, incrementing it is nontrivial.  To
			 * do so efficiently, use the 'bsr' instruction to find
			 * the last (highest order) 0 bit in the codeword, set
			 * it, and clear any later (higher order) 1 bits.  But
			 * 'bsr' actually finds the highest order 1 bit, so to
			 * use it first flip all bits in the codeword by XOR'ing
			 * it with (1U << len) - 1 == cur_table_end - 1.
			 */
			bit = 1U << bsr32(codeword ^ (cur_table_end - 1));
			codeword &= bit - 1;
			codeword |= bit;
		} while (--count);

		/*
		 * All single-symbol codewords of length 'len' have now been
		 * placed into 'decode_table[0 .. (1 << len) - 1]', so fill any
		 * compound entries whose two codewords sum to 'len'.
		 */
		if (len <= max_short_lit_cw_len)
			cs.lit_end[len] = num_short_lits;
		if (len <= max_short_lensym_cw_len)
			cs.lensym_end[len] = num_short_lensyms;
		if (len >= min_compound_cw_len)
			ADD_SUFFIX(fill_compound_litlen_entries)(decode_table,
								 short_lits,
								 &cs, len);

		/*
		 * Advance to the next non-empty codeword length, doubling the
		 * table at each step up to 'table_bits'.  Even if a length
		 * 'len <= table_bits' has no single-symbol codewords
		 * (len_counts[len] == 0), shorter codewords can still sum to
		 * 'len', so fill_compound_litlen_entries() must still run at
		 * each skipped length.
		 */
		for (;;) {
			if (++len <= table_bits) {
				memcpy(&decode_table[cur_table_end],
				       decode_table,
				       cur_table_end * sizeof(decode_table[0]));
				cur_table_end <<= 1;
			}
			count = len_counts[len];
			if (count != 0)
				break;
			if (len <= table_bits && len >= min_compound_cw_len)
				ADD_SUFFIX(fill_compound_litlen_entries)(
						decode_table, short_lits, &cs,
						len);
		}
	}

	/* Process codewords with len > table_bits.  These require subtables. */
	cur_table_end = 1U << table_bits;
	subtable_prefix = -1;
	subtable_start = 0;
	for (;;) {
		u32 entry;
		unsigned i;
		unsigned stride;
		unsigned bit;

		/*
		 * Start a new subtable if the first 'table_bits' bits of the
		 * codeword don't match the prefix of the current subtable.
		 */
		if ((codeword & ((1U << table_bits) - 1)) != subtable_prefix) {
			subtable_prefix = (codeword & ((1U << table_bits) - 1));
			subtable_start = cur_table_end;
			/*
			 * Calculate the subtable length.  If the codeword has
			 * length 'table_bits + n', then the subtable needs
			 * '2^n' entries.  But it may need more; if fewer than
			 * '2^n' codewords of length 'table_bits + n' remain,
			 * then the length will need to be incremented to bring
			 * in longer codewords until the subtable can be
			 * completely filled.  Note that because the Huffman
			 * code is complete, it will always be possible to fill
			 * the subtable eventually.
			 */
			subtable_bits = len - table_bits;
			codespace_used = count;
			while (codespace_used < (1U << subtable_bits)) {
				subtable_bits++;
				codespace_used = (codespace_used << 1) +
					len_counts[table_bits + subtable_bits];
			}
			cur_table_end = subtable_start + (1U << subtable_bits);

			/*
			 * Create the entry that points from the main table to
			 * the subtable.
			 */
			decode_table[subtable_prefix] =
				((u32)subtable_start << 16) |
				HUFFDEC_EXCEPTIONAL |
				HUFFDEC_SUBTABLE_POINTER |
				(subtable_bits << 8) | table_bits;
		}

		/* Fill the subtable entries for the current codeword. */
		entry = make_decode_table_entry(decode_results, *sorted_syms++,
						len - table_bits);
		i = subtable_start + (codeword >> table_bits);
		stride = 1U << (len - table_bits);
		do {
			decode_table[i] = entry;
			i += stride;
		} while (i < cur_table_end);

		/* Advance to the next codeword. */
		if (codeword == (1U << len) - 1) /* last codeword (all 1's)? */
			return true;
		bit = 1U << bsr32(codeword ^ ((1U << len) - 1));
		codeword &= bit - 1;
		codeword |= bit;
		count--;
		while (count == 0)
			count = len_counts[++len];
	}
}

/* Build the decode table for the precode.  */
static ATTRIBUTES bool
ADD_SUFFIX(build_precode_decode_table)(struct libdeflate_decompressor *d)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(PRECODE_TABLEBITS == 7 && PRECODE_ENOUGH == 128);

	STATIC_ASSERT(ARRAY_LEN(precode_decode_results) ==
		      DEFLATE_NUM_PRECODE_SYMS);
	STATIC_ASSERT(DEFLATE_MAX_PRE_CODEWORD_LEN < BITBUF_NBITS);

	return ADD_SUFFIX(build_decode_table)(d->u.l.precode_decode_table,
					      d->u.precode_lens,
					      DEFLATE_NUM_PRECODE_SYMS,
					      precode_decode_results,
					      PRECODE_TABLEBITS,
					      DEFLATE_MAX_PRE_CODEWORD_LEN,
					      d->sorted_syms,
					      NULL,
					      false);
}

/* Build the decode table for the literal/length code.  */
static ATTRIBUTES bool
ADD_SUFFIX(build_litlen_decode_table)(struct libdeflate_decompressor *d,
				      unsigned num_litlen_syms,
				      bool allow_compound)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(LITLEN_TABLEBITS == 12 && LITLEN_ENOUGH == 4382);
	STATIC_ASSERT(LITLEN_SMALL_TABLEBITS == 11 && 2342 <= LITLEN_ENOUGH);
	/*
	 * build_decode_table() uses the space past the 2^LITLEN_TABLEBITS main
	 * table entries as scratch space for up to DEFLATE_NUM_LITERALS
	 * 'short_lits' entries when generating compound entries.
	 */
	STATIC_ASSERT(LITLEN_ENOUGH - (1U << LITLEN_TABLEBITS) >=
		      DEFLATE_NUM_LITERALS);

	STATIC_ASSERT(ARRAY_LEN(litlen_decode_results) ==
		      DEFLATE_NUM_LITLEN_SYMS);
	STATIC_ASSERT(LENGTH_MAXBITS < BITBUF_NBITS);

	/*
	 * For single-literal entries, the remaining codeword length in bits
	 * 11-8 must not reach HUFFDEC_DOUBLE_LITERAL, since
	 * WRITE_FASTLOOP_LITERALS() relies on that bit being clear.
	 */
	STATIC_ASSERT((DEFLATE_MAX_LITLEN_CODEWORD_LEN << 8) <
		      HUFFDEC_DOUBLE_LITERAL);

	return ADD_SUFFIX(build_decode_table)(d->u.litlen_decode_table,
					      d->u.l.lens,
					      num_litlen_syms,
					      litlen_decode_results,
					      allow_compound ?
						LITLEN_TABLEBITS :
						LITLEN_SMALL_TABLEBITS,
					      DEFLATE_MAX_LITLEN_CODEWORD_LEN,
					      d->sorted_syms,
					      &d->litlen_tablebits,
					      allow_compound);
}

/* Build the decode table for the offset code.  */
static ATTRIBUTES bool
ADD_SUFFIX(build_offset_decode_table)(struct libdeflate_decompressor *d,
				      unsigned num_litlen_syms,
				      unsigned num_offset_syms)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(OFFSET_TABLEBITS == 8 && OFFSET_ENOUGH == 402);

	STATIC_ASSERT(ARRAY_LEN(offset_decode_results) ==
		      DEFLATE_NUM_OFFSET_SYMS);
	STATIC_ASSERT(OFFSET_MAXBITS < BITBUF_NBITS);

	return ADD_SUFFIX(build_decode_table)(d->offset_decode_table,
					      d->u.l.lens + num_litlen_syms,
					      num_offset_syms,
					      offset_decode_results,
					      OFFSET_TABLEBITS,
					      DEFLATE_MAX_OFFSET_CODEWORD_LEN,
					      d->sorted_syms,
					      NULL,
					      false);
}

/*
 * Shared body of the fastloop for decoding literals and matches.  It does
 * bounds checks on in_next and out_next in the loop conditions so that
 * additional bounds checks aren't needed inside the loop body.
 *
 * Instantiated into two dedicated non-inline functions to keep register
 * allocation clean and avoid checking 'offset <= out_next - ctx->out' once at
 * least DEFLATE_MAX_MATCH_OFFSET (32768) bytes have already been decompressed:
 *
 * - decompress_fastloop_checked(): check_offset == true (first 32 KiB)
 * - decompress_fastloop_unchecked(): check_offset == false (past 32 KiB)
 *
 * The caller passes decompress_fastloop_checked() an 'out_fastloop_end' that
 * is capped at 'out + DEFLATE_MAX_MATCH_OFFSET', so that it returns as soon as
 * possible after the offset check becomes unnecessary.  The caller then
 * continues with decompress_fastloop_unchecked().
 *
 * Both 'in_fastloop_end' and 'out_fastloop_end' are checked only at the end of
 * each iteration, so the caller must ensure 'in_next < in_fastloop_end' and
 * 'out_next < out_fastloop_end' before calling.
 */
static forceinline ATTRIBUTES enum fastloop_result
ADD_SUFFIX(decompress_fastloop_body)(struct libdeflate_decompressor * restrict d,
				     struct fastloop_ctx * restrict ctx,
				     const u8 * const in_fastloop_end,
				     u8 * const out_fastloop_end,
				     const u32 litlen_tablemask,
				     const bool check_offset)
{
	const u8 * restrict in_next = ctx->in_next;
	u8 * restrict out_next = ctx->out_next;
	bitbuf_t bitbuf = ctx->bitbuf;
	bitbuf_t saved_bitbuf;
	u32 bitsleft = ctx->bitsleft;
	u32 entry;

	/*
	 * Refill the bitbuffer and preload the first litlen decode table entry.
	 * Each iteration preloads the first litlen entry for the *next*
	 * iteration before refilling or copying a match, hiding the L1 table
	 * load latency behind match copying, bitbuffer refill, and loop-bound
	 * checks.
	 */
	REFILL_BITS_IN_FASTLOOP();
	entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
	do {
		u32 length, offset;
		const u8 *src;
		u8 *dst;

		/*
		 * Consume the bits for the litlen decode table entry.  Save the
		 * original bitbuf for later, in case the extra match length
		 * bits need to be extracted from it.
		 */
		saved_bitbuf = bitbuf;
		bitbuf >>= entry & (BITBUF_NBITS - 1);
		bitsleft -= entry; /* optimization: subtract full entry */

		/*
		 * Begin by checking for a "fast" literal entry, i.e. 1 or 2
		 * literals that don't need a subtable.
		 */
		if (entry & HUFFDEC_LITERAL) {
			/*
			 * On 64-bit platforms, we do up to 2 extra fast literal
			 * table lookups in addition to the primary item.  Each
			 * lookup decodes 1 or 2 literals and consumes at most
			 * LITLEN_TABLEBITS bits, leaving enough bits remaining
			 * for whatever primary item follows: either a match
			 * length (up to LENGTH_MAXBITS, plus preloading
			 * OFFSET_TABLEBITS) or a literal via subtable (up to
			 * DEFLATE_MAX_LITLEN_CODEWORD_LEN, plus preloading the
			 * next iteration's LITLEN_TABLEBITS).
			 *
			 * Note: the definitions of FASTLOOP_MAX_BYTES_WRITTEN
			 * and FASTLOOP_MAX_BYTES_READ need to be updated if the
			 * number of extra lookups here is changed.
			 */
			if (/* enough bits for 2 fast lookups + length + offset preload? */
			    CAN_CONSUME_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS +
							 LENGTH_MAXBITS,
							 OFFSET_TABLEBITS) &&
			    /* enough bits for 2 fast lookups + slow literal + litlen preload? */
			    CAN_CONSUME_AND_THEN_PRELOAD(2 * LITLEN_TABLEBITS +
							 DEFLATE_MAX_LITLEN_CODEWORD_LEN,
							 LITLEN_TABLEBITS)) {
				/* 1st extra fast literal lookup (1-2 literals) */
				WRITE_FASTLOOP_LITERALS(entry);
				entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
				saved_bitbuf = bitbuf;
				bitbuf >>= entry & (BITBUF_NBITS - 1);
				bitsleft -= entry;
				if (entry & HUFFDEC_LITERAL) {
					/* 2nd extra fast literal lookup (1-2 literals) */
					WRITE_FASTLOOP_LITERALS(entry);
					entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
					saved_bitbuf = bitbuf;
					bitbuf >>= entry & (BITBUF_NBITS - 1);
					bitsleft -= entry;
					if (entry & HUFFDEC_LITERAL) {
						/*
						 * Another fast literal lookup
						 * (1-2 literals), in lieu of
						 * the primary item.
						 */
						WRITE_FASTLOOP_LITERALS(entry);
						entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
						REFILL_BITS_IN_FASTLOOP();
						continue;
					}
				}
			} else {
				/*
				 * Decode 1-2 literals, preload the next litlen
				 * decode table entry, and refill the bitbuffer.
				 */
				STATIC_ASSERT(CAN_CONSUME_AND_THEN_PRELOAD(
						LITLEN_TABLEBITS, LITLEN_TABLEBITS));
				WRITE_FASTLOOP_LITERALS(entry);
				entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
				REFILL_BITS_IN_FASTLOOP();
				continue;
			}
		}

		/*
		 * It's not a literal entry, so it can be a length entry (plain
		 * or fused with a preceding literal), a subtable pointer entry,
		 * or an end-of-block entry.  Detect the two unlikely cases by
		 * testing the HUFFDEC_EXCEPTIONAL flag.
		 */
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			/* Subtable pointer or end-of-block entry */

			if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
				goto block_done;

			/*
			 * A subtable is required.  Load and consume the
			 * subtable entry.  The subtable entry can be of any
			 * type: literal, length, or end-of-block.
			 */
			entry = d->u.litlen_decode_table[(entry >> 16) +
				EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x1F)];
			saved_bitbuf = bitbuf;
			bitbuf >>= entry & (BITBUF_NBITS - 1);
			bitsleft -= entry;

			/*
			 * 32-bit platforms that use the byte-at-a-time refill
			 * method have to do a refill here for there to always
			 * be enough bits to decode a literal that requires a
			 * subtable, then preload the next litlen decode table
			 * entry; or to decode a match length that requires a
			 * subtable, then preload the offset decode table entry.
			 */
			if (!CAN_CONSUME_AND_THEN_PRELOAD(DEFLATE_MAX_LITLEN_CODEWORD_LEN,
							  LITLEN_TABLEBITS) ||
			    !CAN_CONSUME_AND_THEN_PRELOAD(LENGTH_MAXBITS,
							  OFFSET_TABLEBITS))
				REFILL_BITS_IN_FASTLOOP();
			if (entry & HUFFDEC_LITERAL) {
				/*
				 * Decode a literal that required a subtable.
				 * Subtables never contain 2-literal entries,
				 * so this is always exactly 1 literal.
				 */
				u32 lit = entry >> HUFFDEC_LITERAL1_SHIFT;

				entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
				REFILL_BITS_IN_FASTLOOP();
				*out_next++ = lit;
				continue;
			}
			if (unlikely(entry & HUFFDEC_END_OF_BLOCK))
				goto block_done;
			/* Else, it's a length that required a subtable. */
			ASSERT(!(entry & HUFFDEC_LIT_LENGTH));
		}

		/*
		 * Decode the match length (and, if HUFFDEC_LIT_LENGTH is set,
		 * emit the fused preceding literal byte branchlessly).
		 *
		 * When HUFFDEC_LIT_LENGTH is clear (plain length entry), this
		 * writes a harmless byte to *out_next without advancing
		 * out_next; match copying immediately overwrites it.  The
		 * bitwise mask expression avoids letting the compiler emit a
		 * branch on the unpredictable HUFFDEC_LIT_LENGTH bit.
		 *
		 * The length base can't simply always be masked with
		 * SHORT_LENSYM_BASE_MASK (0x7F), since in plain length entries
		 * it can be up to 258 and use bits 24-23, which in fused
		 * entries hold the low 2 bits of the literal instead.
		 * '(has_lit - 1U) | SHORT_LENSYM_BASE_MASK' evaluates to 0x7F
		 * when has_lit == 1 and ~0U when has_lit == 0.
		 *
		 * 'out_next' MUST be updated before validating and applying the
		 * match offset, since the match starts after the literal (and
		 * may reference it when offset == 1).
		 */
		{
			u32 has_lit = (entry >> HUFFDEC_LIT_LENGTH_SHIFT) & 1;

			*out_next = (u8)(entry >> HUFFDEC_LITERAL2_SHIFT);
			out_next += has_lit;
			length = (entry >> 16) &
				 ((has_lit - 1U) | SHORT_LENSYM_BASE_MASK);
		}
		STATIC_ASSERT(LENGTH_MAXBITS < 32);
		STATIC_ASSERT(LITLEN_TABLEBITS + 4 <= LENGTH_MAXFASTBITS);
		length += EXTRACT_VARBITS8(saved_bitbuf, entry) >>
			  ((entry >> 8) & 0x1F);

		/*
		 * Decode the match offset.  There are enough "preloadable" bits
		 * remaining to preload the offset decode table entry, but a
		 * refill might be needed before consuming it.
		 */
		STATIC_ASSERT(CAN_CONSUME_AND_THEN_PRELOAD(LENGTH_MAXFASTBITS,
							   OFFSET_TABLEBITS));
		entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
		if (CAN_CONSUME_AND_THEN_PRELOAD(OFFSET_MAXBITS,
						 LITLEN_TABLEBITS)) {
			/*
			 * Decoding a match offset on a 64-bit platform.  We may
			 * need to refill once, but then we can decode the whole
			 * offset and preload the next litlen table entry.
			 */
			if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
				/* Offset codeword requires a subtable */
				if (unlikely((u8)bitsleft < OFFSET_MAXBITS +
					     LITLEN_TABLEBITS - PRELOAD_SLACK))
					REFILL_BITS_IN_FASTLOOP();
				bitbuf >>= OFFSET_TABLEBITS;
				bitsleft -= OFFSET_TABLEBITS;
				entry = d->offset_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x1F)];
			} else if (unlikely((u8)bitsleft < OFFSET_MAXFASTBITS +
					    LITLEN_TABLEBITS - PRELOAD_SLACK))
				REFILL_BITS_IN_FASTLOOP();
		} else {
			/* Decoding a match offset on a 32-bit platform */
			REFILL_BITS_IN_FASTLOOP();
			if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
				/* Offset codeword requires a subtable */
				bitbuf >>= OFFSET_TABLEBITS;
				bitsleft -= OFFSET_TABLEBITS;
				entry = d->offset_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x1F)];
				REFILL_BITS_IN_FASTLOOP();
				/* No further refill needed before extra bits */
				STATIC_ASSERT(CAN_CONSUME(
					OFFSET_MAXBITS - OFFSET_TABLEBITS));
			} else {
				/* No refill needed before extra bits */
				STATIC_ASSERT(CAN_CONSUME(OFFSET_MAXFASTBITS));
			}
		}
		saved_bitbuf = bitbuf;
		bitbuf >>= entry & (BITBUF_NBITS - 1);
		bitsleft -= entry; /* optimization: subtract full entry */
		offset = entry >> 16;
		STATIC_ASSERT(OFFSET_MAXBITS < 32);
		offset += EXTRACT_VARBITS8(saved_bitbuf, entry) >>
			  ((entry >> 8) & 0x1F);

		/*
		 * Validate the match offset.  Because every entry in
		 * offset_decode_results[] (including the unused symbols 30 and
		 * 31) produces an offset in [1, DEFLATE_MAX_MATCH_OFFSET], this
		 * check is guaranteed to pass once at least
		 * DEFLATE_MAX_MATCH_OFFSET bytes have been written to 'out'.
		 */
		STATIC_ASSERT(24577 + BITMASK(DEFLATE_MAX_EXTRA_OFFSET_BITS) ==
			      DEFLATE_MAX_MATCH_OFFSET);
		if (check_offset) {
			FASTLOOP_SAFETY_CHECK(offset <= out_next - ctx->out);
		} else {
			ASSERT(offset >= 1 &&
			       offset <= DEFLATE_MAX_MATCH_OFFSET);
			ASSERT(out_next - ctx->out >= DEFLATE_MAX_MATCH_OFFSET);
		}
		src = out_next - offset;
		dst = out_next;
		out_next += length;

		/*
		 * Before starting to issue the instructions to copy the match,
		 * refill the bitbuffer and preload the litlen decode table
		 * entry for the next loop iteration.  This can increase
		 * performance by allowing the latency of the match copy to
		 * overlap with these other operations.  To further reduce
		 * latency, we've arranged for there to be enough bits remaining
		 * to do the table preload independently of the refill, except
		 * on 32-bit platforms.
		 */
		if (!CAN_CONSUME_AND_THEN_PRELOAD(
			MAX(OFFSET_MAXBITS - OFFSET_TABLEBITS,
			    OFFSET_MAXFASTBITS),
			LITLEN_TABLEBITS) &&
		    unlikely((u8)bitsleft < LITLEN_TABLEBITS - PRELOAD_SLACK))
			REFILL_BITS_IN_FASTLOOP();
		entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
		REFILL_BITS_IN_FASTLOOP();

		/*
		 * Copy the match using SIMD instructions if available,
		 * otherwise using word loads/stores if
		 * UNALIGNED_ACCESS_IS_FAST, otherwise using byte loads/stores.
		 *
		 * Typically most matches are short, but the lengths can still
		 * vary quite a bit.  That makes branches on the length fairly
		 * unpredictable and costly.  So in general, it's worth
		 * unconditionally writing about 20-48 bytes to cover the vast
		 * majority of matches, then write 16-40 additional bytes per
		 * loop iteration (depending on the platform).
		 *
		 * To support this, it's guaranteed that there are at least
		 * FASTLOOP_MAX_MATCH_OVERRUN bytes available to be freely
		 * written to after &dst[length].  The conditions for entering
		 * and exiting the fastloop ensure that this always holds.
		 */
#ifdef COPY_MATCH
		COPY_MATCH(dst, src, offset, out_next);
#else
		STATIC_ASSERT(FASTLOOP_MAX_MATCH_OVERRUN >= 5 * WORDBYTES - 1);
		if (UNALIGNED_ACCESS_IS_FAST && offset >= WORDBYTES) {
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			store_word_unaligned(load_word_unaligned(src), dst);
			src += WORDBYTES;
			dst += WORDBYTES;
			while (dst < out_next) {
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
				store_word_unaligned(load_word_unaligned(src), dst);
				src += WORDBYTES;
				dst += WORDBYTES;
			}
		} else if (UNALIGNED_ACCESS_IS_FAST && offset == 1) {
			machine_word_t v;
			/*
			 * offset == 1 is run-length encoding, which is common
			 * and is worth handling specially rather than relying
			 * on the 'dst += offset' approach for it.
			 *
			 * Efficiently broadcast the single byte across a word
			 * by using a multiplication.
			 *
			 * This code tends to get auto-vectorized, so keep it
			 * copying a multiple of 16 bytes at a time.
			 */
			v = (machine_word_t)0x0101010101010101 * src[0];
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			store_word_unaligned(v, dst);
			dst += WORDBYTES;
			while (dst < out_next) {
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
				store_word_unaligned(v, dst);
				dst += WORDBYTES;
			}
		} else if (UNALIGNED_ACCESS_IS_FAST) {
			/*
			 * 2 <= offset < WORDBYTES: load one word 'v' whose
			 * first 'offset' bytes hold the repeating pattern, then
			 * store 'v' at stride 'offset'.  (Each store overwrites
			 * the previous store's trailing bytes.)  Reusing 'v'
			 * avoids per-step reloads and store-to-load forwarding.
			 */
			machine_word_t v = load_word_unaligned(src);

			store_word_unaligned(v, dst);
			dst += offset;
			store_word_unaligned(v, dst);
			dst += offset;
			do {
				store_word_unaligned(v, dst);
				dst += offset;
				store_word_unaligned(v, dst);
				dst += offset;
			} while (dst < out_next);
		} else {
			*dst++ = *src++;
			*dst++ = *src++;
			do {
				*dst++ = *src++;
			} while (dst < out_next);
		}
#endif /* !COPY_MATCH */
	} while (in_next < in_fastloop_end && out_next < out_fastloop_end);

	ctx->in_next = in_next;
	ctx->out_next = out_next;
	ctx->bitbuf = bitbuf;
	ctx->bitsleft = bitsleft;
	return FASTLOOP_BLOCK_NOT_DONE;

block_done:
	ctx->in_next = in_next;
	ctx->out_next = out_next;
	ctx->bitbuf = bitbuf;
	ctx->bitsleft = bitsleft;
	return FASTLOOP_BLOCK_DONE;
}

static NOINLINE ATTRIBUTES enum fastloop_result
ADD_SUFFIX(decompress_fastloop_checked)(struct libdeflate_decompressor * restrict d,
					struct fastloop_ctx * restrict ctx,
					const u8 * const in_fastloop_end,
					u8 * const out_fastloop_end,
					const u32 litlen_tablemask)
{
	return ADD_SUFFIX(decompress_fastloop_body)(d, ctx, in_fastloop_end,
						    out_fastloop_end,
						    litlen_tablemask, true);
}

static NOINLINE ATTRIBUTES enum fastloop_result
ADD_SUFFIX(decompress_fastloop_unchecked)(struct libdeflate_decompressor * restrict d,
					  struct fastloop_ctx * restrict ctx,
					  const u8 * const in_fastloop_end,
					  u8 * const out_fastloop_end,
					  const u32 litlen_tablemask)
{
	return ADD_SUFFIX(decompress_fastloop_body)(d, ctx, in_fastloop_end,
						    out_fastloop_end,
						    litlen_tablemask, false);
}

static ATTRIBUTES enum libdeflate_result
ADD_SUFFIX(deflate_decompress)(struct libdeflate_decompressor * restrict d,
			       const void * restrict in, size_t in_nbytes,
			       void * restrict out, size_t out_nbytes_avail,
			       size_t *actual_in_nbytes_ret,
			       size_t *actual_out_nbytes_ret)
{
	u8 *out_next = out;
	u8 * const out_end = out_next + out_nbytes_avail;
	u8 * const out_fastloop_end =
		out_end - MIN(out_nbytes_avail, FASTLOOP_MAX_BYTES_WRITTEN);

	/* Input bitstream state; see deflate_decompress.c for documentation */
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	const u8 * const in_fastloop_end =
		in_end - MIN(in_nbytes, FASTLOOP_MAX_BYTES_READ);
	bitbuf_t bitbuf = 0;
	bitbuf_t saved_bitbuf;
	u32 bitsleft = 0;
	size_t overread_count = 0;

	bool is_final_block;
	unsigned block_type;
	unsigned num_litlen_syms;
	unsigned num_offset_syms;
	u32 litlen_tablemask;
	u32 entry;

next_block:
	/* Starting to read the next block */
	;

	STATIC_ASSERT(CAN_CONSUME(1 + 2 + 5 + 5 + 4 + 3));
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
		unsigned i;

		/* Read the codeword length counts. */

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 257 + BITMASK(5));
		num_litlen_syms = 257 + ((bitbuf >> 3) & BITMASK(5));

		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 1 + BITMASK(5));
		num_offset_syms = 1 + ((bitbuf >> 8) & BITMASK(5));

		STATIC_ASSERT(DEFLATE_NUM_PRECODE_SYMS == 4 + BITMASK(4));
		num_explicit_precode_lens = 4 + ((bitbuf >> 13) & BITMASK(4));

		d->static_codes_loaded = false;

		/*
		 * Read the precode codeword lengths (up to 19 * 3 == 57 bits).
		 *
		 * On 64-bit platforms with branchless refill, CONSUMABLE_NBITS
		 * is 56, which is just 1 bit short of all 19 3-bit lengths.
		 * Reading the 1st length together with the 17-bit block header
		 * (20 <= 56 bits) leaves at most 18 lengths (54 <= 56 bits),
		 * which can then all be read after a single refill without
		 * per-iteration refill checks.
		 */
		STATIC_ASSERT(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);
		if (CAN_CONSUME(3 * (DEFLATE_NUM_PRECODE_SYMS - 1))) {
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
				if ((u8)bitsleft < 3)
					REFILL_BITS();
				d->u.precode_lens[deflate_precode_lens_permutation[i]] =
					bitbuf & BITMASK(3);
				bitbuf >>= 3;
				bitsleft -= 3;
			} while (++i < num_explicit_precode_lens);
		}
		for (; i < DEFLATE_NUM_PRECODE_SYMS; i++)
			d->u.precode_lens[deflate_precode_lens_permutation[i]] = 0;

		/* Build the decode table for the precode. */
		SAFETY_CHECK(ADD_SUFFIX(build_precode_decode_table)(d));

		/* Decode the litlen and offset codeword lengths. */
		i = 0;
		do {
			unsigned presym;
			u8 rep_val;
			unsigned rep_count;

			if ((u8)bitsleft < DEFLATE_MAX_PRE_CODEWORD_LEN + 7)
				REFILL_BITS();

			/*
			 * The code below assumes that the precode decode table
			 * doesn't have any subtables.
			 */
			STATIC_ASSERT(PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

			/* Decode the next precode symbol. */
			entry = d->u.l.precode_decode_table[
				bitbuf & BITMASK(DEFLATE_MAX_PRE_CODEWORD_LEN)];
			bitbuf >>= entry & (BITBUF_NBITS - 1);
			bitsleft -= entry; /* optimization: subtract full entry */
			presym = entry >> 16;

			if (presym < 16) {
				/* Explicit codeword length */
				d->u.l.lens[i++] = presym;
				continue;
			}

			/* Run-length encoded codeword lengths */

			/*
			 * Note: we don't need to immediately verify that the
			 * repeat count doesn't overflow the number of elements,
			 * since we've sized the lens array to have enough extra
			 * space to allow for the worst-case overrun (138 zeroes
			 * when only 1 length was remaining).
			 *
			 * It's best to always write the maximum number of
			 * entries, regardless of the actual repeat count.  With
			 * a constant size, the compiler emits inline
			 * branchless stores (such as SIMD stores or 'rep stos'
			 * for presym 18) instead of size-checking branches, a
			 * variable-length loop, or a call to an external
			 * memset() function that clobbers caller-saved
			 * registers and causes extra spills in the loop.
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
				STATIC_ASSERT(sizeof(d->u.l.lens[0]) == 1);
				memset(&d->u.l.lens[i], 0, 138);
				i += rep_count;
			}
		} while (i < num_litlen_syms + num_offset_syms);

		/* Unnecessary, but check this for consistency with zlib. */
		SAFETY_CHECK(i == num_litlen_syms + num_offset_syms);

	} else if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {
		u16 len, nlen;

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
		bitsleft = (u8)bitsleft;
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
		unsigned i;

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

	SAFETY_CHECK(ADD_SUFFIX(build_offset_decode_table)(d, num_litlen_syms,
							   num_offset_syms));
	/*
	 * Only generate compound (2-literal and literal+length) entries when
	 * both input and output have enough remaining space for the fastloop to
	 * run and amortize the extra table-building work.  (Note: static
	 * Huffman codes have min_lit_cw_len == 8 and min_lensym_cw_len == 7, so
	 * 8 + 7 == 15 > LITLEN_TABLEBITS; they never contain compound entries
	 * regardless.  Hence the static litlen table, including its
	 * table_bits of 9, is the same regardless of 'allow_compound', which is
	 * what allows it to be reused across blocks and calls via
	 * d->static_codes_loaded.)
	 */
	STATIC_ASSERT(COMPOUND_LITLEN_MIN_OUT_BYTES >
		      FASTLOOP_MAX_BYTES_WRITTEN);
	STATIC_ASSERT(LITLEN_TABLEBITS < 2 * 8);
	STATIC_ASSERT(LITLEN_TABLEBITS < 8 + 7);
	SAFETY_CHECK(ADD_SUFFIX(build_litlen_decode_table)(
			d, num_litlen_syms,
			in_next < in_fastloop_end &&
			out_end - out_next >= COMPOUND_LITLEN_MIN_OUT_BYTES));
have_decode_tables:
	litlen_tablemask = ((u32)1 << d->litlen_tablebits) - 1;

	if (in_next < in_fastloop_end && out_next < out_fastloop_end) {
		struct fastloop_ctx ctx = {
			.out = out,
			.in_next = in_next,
			.out_next = out_next,
			.bitbuf = bitbuf,
			.bitsleft = bitsleft,
		};
		enum fastloop_result res;

		if (out_next - (const u8 *)out < DEFLATE_MAX_MATCH_OFFSET) {
			u8 * const checked_end =
				(out_fastloop_end - (u8 *)out >
				 DEFLATE_MAX_MATCH_OFFSET) ?
				(u8 *)out + DEFLATE_MAX_MATCH_OFFSET :
				out_fastloop_end;

			res = ADD_SUFFIX(decompress_fastloop_checked)(
					d, &ctx, in_fastloop_end, checked_end,
					litlen_tablemask);
			SAFETY_CHECK(res != FASTLOOP_BAD_DATA);
			if (res == FASTLOOP_BLOCK_DONE ||
			    ctx.in_next >= in_fastloop_end ||
			    ctx.out_next >= out_fastloop_end)
				goto fastloop_done;
		}
		res = ADD_SUFFIX(decompress_fastloop_unchecked)(
				d, &ctx, in_fastloop_end, out_fastloop_end,
				litlen_tablemask);
		ASSERT(res != FASTLOOP_BAD_DATA);
fastloop_done:
		in_next = ctx.in_next;
		out_next = ctx.out_next;
		bitbuf = ctx.bitbuf;
		bitsleft = ctx.bitsleft;
		if (res == FASTLOOP_BLOCK_DONE)
			goto block_done;
	}

	/*
	 * This is the generic loop for decoding literals and matches.  This
	 * handles cases where in_next and out_next are close to the end of
	 * their respective buffers.  Usually this loop isn't performance-
	 * critical, as most time is spent in the fastloop above instead.  We
	 * therefore omit some optimizations here in favor of smaller code.
	 */
	for (;;) {
		u32 length, offset;
		const u8 *src;
		u8 *dst;

		REFILL_BITS();
		entry = d->u.litlen_decode_table[(u32)bitbuf & litlen_tablemask];
		saved_bitbuf = bitbuf;
		bitbuf >>= entry & (BITBUF_NBITS - 1);
		bitsleft -= entry;
		/*
		 * Unlike HUFFDEC_EXCEPTIONAL, HUFFDEC_SUBTABLE_POINTER
		 * is never set on literal entries, so it can be tested
		 * before HUFFDEC_LITERAL here to share the literal path.
		 */
		if (unlikely(entry & HUFFDEC_SUBTABLE_POINTER)) {
			entry = d->u.litlen_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x1F)];
			saved_bitbuf = bitbuf;
			bitbuf >>= entry & (BITBUF_NBITS - 1);
			bitsleft -= entry;
		}
		if (entry & HUFFDEC_LITERAL) {
			if (unlikely(out_next == out_end))
				return LIBDEFLATE_INSUFFICIENT_SPACE;
			*out_next++ = entry >> HUFFDEC_LITERAL1_SHIFT;
			if (entry & HUFFDEC_DOUBLE_LITERAL) {
				if (unlikely(out_next == out_end))
					return LIBDEFLATE_INSUFFICIENT_SPACE;
				*out_next++ = entry >> HUFFDEC_LITERAL2_SHIFT;
			}
			continue;
		}
		/*
		 * Only EOB still has HUFFDEC_EXCEPTIONAL set here (subtable
		 * pointers and literals have already been handled).  Do not
		 * test HUFFDEC_END_OF_BLOCK alone, as it shares bit 13 with
		 * HUFFDEC_LIT_LENGTH.
		 */
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			ASSERT(entry & HUFFDEC_END_OF_BLOCK);
			goto block_done;
		}
		if (entry & HUFFDEC_LIT_LENGTH) {
			if (unlikely(out_next == out_end))
				return LIBDEFLATE_INSUFFICIENT_SPACE;
			*out_next++ = entry >> HUFFDEC_LITERAL2_SHIFT;
			length = (entry >> 16) & SHORT_LENSYM_BASE_MASK;
		} else {
			length = entry >> 16;
		}
		STATIC_ASSERT(LENGTH_MAXBITS < 32);
		length += EXTRACT_VARBITS8(saved_bitbuf, entry) >>
			  ((entry >> 8) & 0x1F);
		if (unlikely(length > out_end - out_next))
			return LIBDEFLATE_INSUFFICIENT_SPACE;

		if (!CAN_CONSUME(LENGTH_MAXBITS + OFFSET_MAXBITS))
			REFILL_BITS();
		entry = d->offset_decode_table[bitbuf & BITMASK(OFFSET_TABLEBITS)];
		if (unlikely(entry & HUFFDEC_EXCEPTIONAL)) {
			bitbuf >>= OFFSET_TABLEBITS;
			bitsleft -= OFFSET_TABLEBITS;
			entry = d->offset_decode_table[(entry >> 16) +
					EXTRACT_VARBITS(bitbuf, (entry >> 8) & 0x1F)];
			if (!CAN_CONSUME(OFFSET_MAXBITS))
				REFILL_BITS();
		}
		offset = entry >> 16;
		STATIC_ASSERT(OFFSET_MAXBITS < 32);
		offset += EXTRACT_VARBITS8(bitbuf, entry) >>
			  ((entry >> 8) & 0x1F);
		bitbuf >>= entry & (BITBUF_NBITS - 1);
		bitsleft -= entry;

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

	bitsleft = (u8)bitsleft;

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

#undef SUFFIX
#undef ATTRIBUTES
#undef EXTRACT_VARBITS
#undef EXTRACT_VARBITS8
#undef COPY_MATCH
