/*
 * deflate_decompress.c - a decompressor for DEFLATE
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
 *
 * ---------------------------------------------------------------------------
 *
 * This is a highly optimized DEFLATE decompressor.  It is much faster than
 * vanilla zlib, typically over 3 times as fast, though results vary by CPU.
 *
 * Why this is faster than vanilla zlib:
 *
 * - Word accesses rather than byte accesses when reading input
 * - Word accesses rather than byte accesses when copying matches
 * - Faster Huffman decoding combined with various DEFLATE-specific tricks,
 *   including decoding 2 litlen symbols (2 literals, or a literal followed by
 *   a match length) with a single table lookup when they fit in the table
 * - Larger bitbuffer variable that doesn't need to be refilled as often
 * - Other optimizations to remove unnecessary branches
 * - Only full-buffer decompression is supported, so the code doesn't need to
 *   support stopping and resuming decompression.
 * - On x86, a version of the decompression routine is compiled with BMI2,
 *   BMI, LZCNT, and SSSE3 enabled and is used automatically at runtime when
 *   supported.  On arm64, a version with NEON match copying is used.
 */

#include "lib_common.h"
#include "deflate_constants.h"

/*
 * If the expression passed to SAFETY_CHECK() evaluates to false, then the
 * decompression routine immediately returns LIBDEFLATE_BAD_DATA, indicating the
 * compressed data is invalid.  FASTLOOP_SAFETY_CHECK() does the same in the
 * fastloop function(s), returning FASTLOOP_BAD_DATA.
 *
 * Theoretically, these checks could be disabled for specialized applications
 * where all input to the decompressor will be trusted.
 */
#if 0
#  pragma message("UNSAFE DECOMPRESSION IS ENABLED. THIS MUST ONLY BE USED IF THE DECOMPRESSOR INPUT WILL ALWAYS BE TRUSTED!")
#  define SAFETY_CHECK(expr)		(void)(expr)
#  define FASTLOOP_SAFETY_CHECK(expr)	(void)(expr)
#else
#  define SAFETY_CHECK(expr)		if (unlikely(!(expr))) return LIBDEFLATE_BAD_DATA
#  define FASTLOOP_SAFETY_CHECK(expr)	if (unlikely(!(expr))) return FASTLOOP_BAD_DATA
#endif

/*****************************************************************************
 *				Input bitstream                              *
 *****************************************************************************/

/*
 * The state of the "input bitstream" consists of the following variables:
 *
 *	- in_next: a pointer to the next unread byte in the input buffer
 *
 *	- in_end: a pointer to just past the end of the input buffer
 *
 *	- bitbuf: a word-sized variable containing bits that have been read from
 *		  the input buffer or from the implicit appended zero bytes
 *
 *	- bitsleft: the number of bits in 'bitbuf' available to be consumed.
 *		    After REFILL_BITS_BRANCHLESS(), 'bitbuf' can actually
 *		    contain more bits than this.  However, only the bits counted
 *		    by 'bitsleft' can actually be consumed; the rest can only be
 *		    used for preloading.
 *
 *		    As a micro-optimization, we allow bits 8 and higher of
 *		    'bitsleft' to contain garbage.  When consuming the bits
 *		    associated with a decode table entry, this allows us to do
 *		    'bitsleft -= entry' instead of 'bitsleft -= (u8)entry'.
 *		    On some CPUs, this helps reduce instruction dependencies.
 *		    This does have the disadvantage that 'bitsleft' sometimes
 *		    needs to be cast to 'u8', such as when it's used as a shift
 *		    amount in REFILL_BITS_BRANCHLESS().  But that one happens
 *		    for free since most CPUs ignore high bits in shift amounts.
 *
 *	- overread_count: the total number of implicit appended zero bytes that
 *			  have been loaded into the bitbuffer, including any
 *			  counted by 'bitsleft' and any already consumed
 */

/*
 * The type for the bitbuffer variable ('bitbuf' described above).  For best
 * performance, this should have size equal to a machine word.
 *
 * 64-bit platforms have a significant advantage: they get a bigger bitbuffer
 * which they don't have to refill as often.
 */
typedef machine_word_t bitbuf_t;
#define BITBUF_NBITS	(8 * (int)sizeof(bitbuf_t))

/* BITMASK(n) returns a bitmask of length 'n'. */
#define BITMASK(n)	(((bitbuf_t)1 << (n)) - 1)

/*
 * MAX_BITSLEFT is the maximum number of consumable bits, i.e. the maximum value
 * of '(u8)bitsleft'.  This is the size of the bitbuffer variable, minus 1 if
 * the branchless refill method is being used (see REFILL_BITS_BRANCHLESS()).
 */
#define MAX_BITSLEFT	\
	(UNALIGNED_ACCESS_IS_FAST ? BITBUF_NBITS - 1 : BITBUF_NBITS)

/*
 * CONSUMABLE_NBITS is the minimum number of bits that are guaranteed to be
 * consumable (counted in 'bitsleft') immediately after refilling the bitbuffer.
 * Since only whole bytes can be added to 'bitsleft', the worst case is
 * 'MAX_BITSLEFT - 7': the smallest amount where another byte doesn't fit.
 */
#define CONSUMABLE_NBITS	(MAX_BITSLEFT - 7)

/*
 * FASTLOOP_PRELOADABLE_NBITS is the minimum number of bits that are guaranteed
 * to be preloadable immediately after REFILL_BITS_IN_FASTLOOP().  (It is *not*
 * guaranteed after REFILL_BITS(), since REFILL_BITS() falls back to a
 * byte-at-a-time refill method near the end of input.)  This may exceed the
 * number of consumable bits (counted by 'bitsleft').  Any bits not counted in
 * 'bitsleft' can only be used for precomputation and cannot be consumed.
 */
#define FASTLOOP_PRELOADABLE_NBITS	\
	(UNALIGNED_ACCESS_IS_FAST ? BITBUF_NBITS : CONSUMABLE_NBITS)

/*
 * PRELOAD_SLACK is the minimum number of bits that are guaranteed to be
 * preloadable but not consumable, following REFILL_BITS_IN_FASTLOOP() and any
 * subsequent consumptions.  This is 1 bit if the branchless refill method is
 * being used, and 0 bits otherwise.
 */
#define PRELOAD_SLACK	MAX(0, FASTLOOP_PRELOADABLE_NBITS - MAX_BITSLEFT)

/*
 * CAN_CONSUME(n) is true if it's guaranteed that if the bitbuffer has just been
 * refilled, then it's always possible to consume 'n' bits from it.  'n' should
 * be a compile-time constant, to enable compile-time evaluation.
 */
#define CAN_CONSUME(n)	(CONSUMABLE_NBITS >= (n))

/*
 * CAN_CONSUME_AND_THEN_PRELOAD(consume_nbits, preload_nbits) is true if it's
 * guaranteed that after REFILL_BITS_IN_FASTLOOP(), it's always possible to
 * consume 'consume_nbits' bits, then preload 'preload_nbits' bits.  The
 * arguments should be compile-time constants to enable compile-time evaluation.
 */
#define CAN_CONSUME_AND_THEN_PRELOAD(consume_nbits, preload_nbits)	\
	(CONSUMABLE_NBITS >= (consume_nbits) &&				\
	 FASTLOOP_PRELOADABLE_NBITS >= (consume_nbits) + (preload_nbits))

/*
 * REFILL_BITS_BRANCHLESS() branchlessly refills the bitbuffer variable by
 * reading the next word from the input buffer and updating 'in_next' and
 * 'bitsleft' based on how many bits were refilled -- counting whole bytes only.
 * This is much faster than reading a byte at a time, at least if the CPU is
 * little endian and supports fast unaligned memory accesses.
 *
 * To avoid (u8) casts elsewhere, 'bitsleft' is allowed to contain garbage in
 * bits 8 and higher.  So that has to be taken into account here.
 *
 * Given that, the simplest way of branchlessly updating 'bitsleft' would be:
 *
 *	bitsleft += (MAX_BITSLEFT - (u8)bitsleft) & ~7;
 *
 * To make it faster, we define MAX_BITSLEFT to be 'BITBUF_NBITS - 1' rather
 * than BITBUF_NBITS, so that in binary it looks like 111111 or 11111.  Then,
 * we update 'bitsleft' by just setting the bits above the low 3 bits:
 *
 *	bitsleft |= MAX_BITSLEFT & ~7;
 *
 * That compiles down to a single instruction like 'orl $0x38, %edi'.  Using
 * 'MAX_BITSLEFT == BITBUF_NBITS - 1' also has the advantage that refills can be
 * done when '(u8)bitsleft == MAX_BITSLEFT' without invoking undefined behavior.
 *
 * 'in_next' can then be branchlessly updated using any of the following
 * methods (note that (u8)bitsleft <= MAX_BITSLEFT and MAX_BITSLEFT == 2^k - 1):
 *
 * (1)	in_next += (MAX_BITSLEFT - (u8)bitsleft) >> 3;
 *
 *	The compiler only knows (u8)bitsleft is in 0..255, not <= MAX_BITSLEFT,
 *	so it cannot easily turn the subtraction into an XOR/NOT and must
 *	instead emit a constant load and subtraction (e.g., 'mov $63; sub; shr'
 *	or 'sub; add').
 *
 * (2)	in_next += ((u8)bitsleft ^ MAX_BITSLEFT) >> 3;
 *
 *	On x86_64 with clang, this produces the best code ('movzbl + shrl $3 +
 *	xorl $7 + addq', where 'movzbl' can often be shared with '(u8)bitsleft'
 *	for the bitbuf shift, and only a single 'addq' updates 'in_next').  On
 *	arm64, both gcc and clang compile it to 3 instructions ('ubfx + eor +
 *	add' or 'lsr + eor + add') with no constant register.  On x86_64 with
 *	gcc, it emits an 8-bit partial-register shift ('shrb $3; xorl $7;
 *	movzbl'), which is slightly worse than (3) for gcc, but we prioritize
 *	x86_64 clang here.  So we use this method.
 *
 * (3)	in_next += (~bitsleft >> 3) & (MAX_BITSLEFT >> 3);
 *
 *	This avoids partial-register accesses on x86_64 gcc ('notl + shrl $3 +
 *	andl $7 + addq') and compiles to 'mvn + ubfx #3, #3 + add' on arm64,
 *	but on x86_64 clang it needs an extra instruction ('andl $7') compared
 *	to (2).
 *
 * (4)	in_next += sizeof(bitbuf_t) - 1;
 *	in_next -= (bitsleft >> 3) & 0x7;
 *
 *	In theory, this ignores the upper garbage bits of 'bitsleft' and could
 *	be combined with 'ubfx' on arm64 to shorten the dependency chain from
 *	'bitsleft' to 'in_next'.  In practice, however, compilers either update
 *	'in_next' twice in series ('sub + add $7/#7' with clang on both x86_64
 *	and arm64) or materialize '7' in a separate register ('mov x14, 7' on
 *	arm64 gcc, 'movl $7' on x86_64 gcc).
 */
#define REFILL_BITS_BRANCHLESS()					\
do {									\
	ASSERT((u8)bitsleft <= MAX_BITSLEFT);				\
	bitbuf |= get_unaligned_leword(in_next) << (u8)bitsleft;	\
	in_next += ((u8)bitsleft ^ MAX_BITSLEFT) >> 3;			\
	bitsleft |= MAX_BITSLEFT & ~7;					\
} while (0)

/*
 * REFILL_BITS() loads bits from the input buffer until the bitbuffer variable
 * contains at least CONSUMABLE_NBITS consumable bits.
 *
 * This checks for the end of input, and it doesn't guarantee
 * FASTLOOP_PRELOADABLE_NBITS, so it can't be used in the fastloop.
 *
 * If we would overread the input buffer, we just don't read anything, leaving
 * the bits zeroed but marking them filled.  This simplifies the decompressor
 * because it removes the need to always be able to distinguish between real
 * overreads and overreads caused only by the decompressor's own lookahead.
 *
 * We do still keep track of the number of bytes that have been overread, for
 * two reasons.  First, it allows us to determine the exact number of bytes that
 * were consumed once the stream ends or an uncompressed block is reached.
 * Second, it allows us to stop early if the overread amount gets so large (more
 * than sizeof bitbuf) that it can only be caused by a real overread.  (The
 * second part is arguably unneeded, since libdeflate is buffer-based; given
 * infinite zeroes, it will eventually either completely fill the output buffer
 * or return an error.  However, we do it to be slightly more friendly to the
 * not-recommended use case of decompressing with an unknown output size.)
 *
 * When UNALIGNED_ACCESS_IS_FAST, the byte-at-a-time loop is just a cold fallback
 * that is used near the end of the input.  In that case, disable unrolling of it
 * to avoid code bloat across the multiple expansions of REFILL_BITS().
 * Otherwise, the loop is the primary refill method, so leave the unrolling
 * decision to the compiler.  (In that case gcc can also restructure the loop
 * such that the hint would trigger an "ignoring loop annotation" warning.)
 */
#if UNALIGNED_ACCESS_IS_FAST
#  define REFILL_BITS_LOOP_HINT	NO_UNROLL_LOOP
#else
#  define REFILL_BITS_LOOP_HINT
#endif
#define REFILL_BITS()							\
do {									\
	if (UNALIGNED_ACCESS_IS_FAST &&					\
	    likely(in_end - in_next >= sizeof(bitbuf_t))) {		\
		REFILL_BITS_BRANCHLESS();				\
	} else {							\
		REFILL_BITS_LOOP_HINT					\
		while ((u8)bitsleft < CONSUMABLE_NBITS) {		\
			if (likely(in_next != in_end)) {		\
				bitbuf |= (bitbuf_t)*in_next++ <<	\
					  (u8)bitsleft;			\
			} else {					\
				overread_count++;			\
				SAFETY_CHECK(overread_count <=		\
					     sizeof(bitbuf_t));		\
			}						\
			bitsleft += 8;					\
		}							\
	}								\
} while (0)

/*
 * REFILL_BITS_IN_FASTLOOP() is like REFILL_BITS(), but it doesn't check for the
 * end of the input.  It can only be used in the fastloop.
 */
#define REFILL_BITS_IN_FASTLOOP()					\
do {									\
	STATIC_ASSERT(UNALIGNED_ACCESS_IS_FAST ||			\
		      FASTLOOP_PRELOADABLE_NBITS == CONSUMABLE_NBITS);	\
	if (UNALIGNED_ACCESS_IS_FAST) {					\
		REFILL_BITS_BRANCHLESS();				\
	} else {							\
		while ((u8)bitsleft < CONSUMABLE_NBITS) {		\
			bitbuf |= (bitbuf_t)*in_next++ << (u8)bitsleft;	\
			bitsleft += 8;					\
		}							\
	}								\
} while (0)

/*
 * FASTLOOP_MAX_MATCH_OVERRUN is the maximum number of bytes past the end of a
 * match that match copying in the fastloop may write to.  The generic copy
 * implementation copies 5 words before checking the length, then 5 words per
 * loop iteration, so it overruns by at most '5 * WORDBYTES - 1' bytes.  The
 * architecture optimized implementations copy at most 48 bytes initially, then
 * at most 32 bytes per iteration, so they overrun by up to MAX(48 -
 * DEFLATE_MIN_MATCH_LEN, 32 - 1) = 45 bytes.  Conservatively use 47 here, which
 * would still be enough even if a change to copy 48 bytes per iteration occurs.
 */
#define FASTLOOP_MAX_MATCH_OVERRUN	MAX(5 * WORDBYTES - 1, 47)

/*
 * FASTLOOP_MAX_BYTES_WRITTEN is the worst-case maximum number of output bytes
 * that are written to during each iteration of the fastloop.  The worst case is
 * 4 literals (consisting of 2 fast literal lookups with 2 literals each), then
 * a match of length DEFLATE_MAX_MATCH_LEN (note: a fused literal+length entry
 * emits at most 1 + SHORT_LENSYM_MAX_MATCH_LEN == 131 <=
 * DEFLATE_MAX_MATCH_LEN), then up to FASTLOOP_MAX_MATCH_OVERRUN bytes of
 * intentional overrun during match copying.
 *
 * This also covers the smaller "overruns" that happen before the match copy:
 * WRITE_FASTLOOP_LITERALS() always stores 2 bytes even for a single literal,
 * and decoding a length entry stores a dummy byte at 'out_next' when there is
 * no fused literal.  If the iteration decodes a match, the match writes >= 3
 * bytes starting at 'out_next', overwriting any such byte; otherwise (only
 * literals and/or end-of-block), the iteration writes at most 3 * 2 == 6 bytes
 * including any 1-byte overrun, well within FASTLOOP_MAX_BYTES_WRITTEN.
 */
#define FASTLOOP_MAX_BYTES_WRITTEN	\
	(4 + DEFLATE_MAX_MATCH_LEN + FASTLOOP_MAX_MATCH_OVERRUN)

/*
 * Worst-case maximum number of input bytes that may be read before 'in_next'
 * is checked against 'in_fastloop_end' at the end of a fastloop iteration.
 *
 * On the first iteration, REFILL_BITS_IN_FASTLOOP() runs once before the loop
 * and up to twice more during the iteration on 64-bit platforms (or up to 5
 * times on 32-bit platforms); on later iterations, 'bitsleft' already starts
 * at >= CONSUMABLE_NBITS from the previous iteration's final refill.  In
 * either case, 'bitsleft' starts >= 0 and ends <= MAX_BITSLEFT, and at most
 * '(2 * LITLEN_TABLEBITS) + LENGTH_MAXBITS + OFFSET_MAXBITS' bits can be
 * consumed in between (2 extra fast literal lookups on 64-bit, then a match;
 * 32-bit skips the extra lookups so this bound is slightly conservative
 * there).  Thus, across all refills in the iteration, at most 'MAX_BITSLEFT +
 * consumed_max' bits can be added to 'bitsleft', advancing 'in_next' by at
 * most 'DIV_ROUND_UP(MAX_BITSLEFT + consumed_max, 8)' bytes.  Finally, adding
 * sizeof(bitbuf_t) accounts for the last REFILL_BITS_BRANCHLESS() reading a
 * word at 'in_next' (which may advance 'in_next' by 0 bytes).
 */
#define FASTLOOP_MAX_BYTES_READ					\
	(DIV_ROUND_UP(MAX_BITSLEFT + (2 * LITLEN_TABLEBITS) +	\
		      LENGTH_MAXBITS + OFFSET_MAXBITS, 8) +	\
	 sizeof(bitbuf_t))

/*
 * Minimum remaining uncompressed output buffer space (in bytes) required (in
 * addition to 'in_next < in_fastloop_end') before generating compound
 * (2-literal and fused literal+length) entries and allowing the litlen main
 * table to grow from LITLEN_SMALL_TABLEBITS (11) up to LITLEN_TABLEBITS (12).
 * For smaller blocks/buffers, the extra table-building work outweighs the
 * fastloop speedup.
 *
 * This is purely a performance heuristic.  Correctness doesn't depend on it,
 * since the generic (non-fast) loop supports compound entries too.  Note that
 * it's based on the remaining output buffer space rather than the size of the
 * block, since the latter isn't known in advance.
 */
#define COMPOUND_LITLEN_MIN_OUT_BYTES	8192

/*
 * Multi-lane speculative decoding of Huffman blocks, see
 * decompress_multilane.h.  It's built for x86_64 and for Apple arm64, where it
 * has been measured, and used on the CPUs that mlsd_usable() accepts; define
 * LIBDEFLATE_NO_MLSD to leave it out.
 */
#if (defined(ARCH_X86_64) || \
     (defined(ARCH_ARM64) && defined(__APPLE__) && !defined(FREESTANDING))) && \
	(defined(__clang__) || GCC_PREREQ(8, 1)) && \
	!defined(LIBDEFLATE_NO_MLSD)
#  define MLSD_ENABLED		1
   /*
    * MLSD_LANES is the number of lanes decoded at once.  With
    * MLSD_INTERLEAVE, a round's records are executed during the next round's
    * steps instead of right away, which is slower on x86_64.  The lanes
    * decode a block if its length codewords take at least MLSD_GATE32 32nds
    * of the codespace: the fastloop does better on runs of literals.  With
    * MLSD_GATE_LEN, each counts for 1 - (its mean length) / MLSD_GATE_LEN:
    * the lanes gain less on long matches.
    */
#  ifndef MLSD_LANES		/* (can be overridden for testing) */
#    ifdef ARCH_X86_64
#      define MLSD_LANES	5
#      define MLSD_INTERLEAVE	0
#      define MLSD_GATE32	6
#      define MLSD_GATE_LEN	18
#    else
#      define MLSD_LANES	6
#      define MLSD_INTERLEAVE	1
#      define MLSD_GATE32	7
#      define MLSD_GATE_LEN	0
#    endif
#  endif
#  ifndef MLSD_ROUND_STEPS	/* (can be overridden for testing) */
#    define MLSD_ROUND_STEPS	256	/* max steps per lane per round */
#    define MLSD_CATCHUP	64	/* max extra steps until the lanes meet */
#    define MLSD_EXTEND		8	/* ... this many at a time */
#  endif
#  define MLSD_NREC		(MLSD_ROUND_STEPS + MLSD_CATCHUP)
#  define MLSD_TABLEBITS	12	/* litlen main table bits */
#else
#  define MLSD_ENABLED		0
#endif

/*****************************************************************************
 *                              Huffman decoding                             *
 *****************************************************************************/

/*
 * The fastest way to decode Huffman-encoded data is basically to use a decode
 * table that maps the next TABLEBITS bits of data to their symbol.  Each entry
 * decode_table[i] maps to the symbol whose codeword is a prefix of 'i'.  A
 * symbol with codeword length 'n' has '2**(TABLEBITS-n)' entries in the table.
 *
 * Ideally, TABLEBITS and the maximum codeword length would be the same; some
 * compression formats are designed with this goal in mind.  Unfortunately, in
 * DEFLATE, the maximum litlen and offset codeword lengths are 15 bits, which is
 * too large for a practical TABLEBITS.  It's not *that* much larger, though, so
 * the workaround is to use a single level of subtables.  In the main table,
 * entries for prefixes of codewords longer than TABLEBITS contain a "pointer"
 * to the appropriate subtable along with the number of bits it is indexed with.
 *
 * The most efficient way to allocate subtables is to allocate them dynamically
 * after the main table.  The worst-case number of table entries needed,
 * including subtables, is precomputable; see the ENOUGH constants below.
 *
 * A useful optimization is to store the codeword lengths in the decode table so
 * that they don't have to be looked up by indexing a separate table that maps
 * symbols to their codeword lengths.  We basically do this; however, for the
 * litlen and offset codes we also implement some DEFLATE-specific optimizations
 * that build in the consideration of the "extra bits" and the
 * literal/length/end-of-block division.
 *
 * For the litlen code, we go a step further and sometimes decode 2 symbols with
 * a single main table lookup.  The motivation is that each lookup depends on
 * the previous one: the number of bits consumed by one symbol determines where
 * the next codeword starts, so the next table index isn't known until the
 * previous entry has been loaded.  Decoding is therefore limited by a serial
 * chain of load-to-use latencies.  However, Huffman coding gives the most
 * frequent symbols the shortest codewords, and in many blocks the most frequent
 * literals have codewords of only a few bits.  So the bits used to index the
 * main table often contain two complete codewords, and the entry for such an
 * index can describe both symbols.  There are two kinds of these "compound"
 * entries:
 *
 *	- 2-literal entries.  The fastloop writes both literals with a single
 *	  16-bit store, and it handles 1-literal and 2-literal entries without
 *	  branching on which kind of entry it has.
 *
 *	- Literal+length entries, which contain a literal followed by a match
 *	  length symbol.  Runs of literals usually end with a match, so this
 *	  folds the last literal of a run into the lookup that decodes the
 *	  length.  Only length symbols FIRST_SHORT_LENSYM through
 *	  LAST_SHORT_LENSYM (257-280: length bases 3-115, at most 4 extra bits)
 *	  are supported.  These cover most matches in practice, and limiting
 *	  the length symbols this way lets the length base fit in the entry
 *	  next to the literal, and keeps the number of bits consumed and bytes
 *	  written within the bounds that already apply to plain length entries.
 *
 * Other combinations aren't supported.  A length symbol is never directly
 * followed by another litlen symbol, as any extra length bits and the match
 * offset come in between.  End-of-block occurs only once per block, so it isn't
 * worth combining with anything.  More than 2 symbols per entry isn't supported
 * either, as a 32-bit entry only has room for 2 literals alongside the flags
 * and bit counts.  Finally, compound entries exist only in the main table, as
 * symbols are combined only when both of their codewords fit in the main table
 * index.  Subtable entries always decode exactly 1 symbol.
 *
 * Compound entries aren't free, though.  To be useful they need a larger main
 * table, which takes longer to build and uses more L1 cache, and generating
 * them adds work to building the table.  So the litlen main table may use up to
 * LITLEN_TABLEBITS (12) bits when compound entries are generated, but only up
 * to LITLEN_SMALL_TABLEBITS (11) bits otherwise.  Compound entries are
 * generated only when enough input and output space remains for the fastloop to
 * amortize the extra work (see COMPOUND_LITLEN_MIN_OUT_BYTES), and only when at
 * least one pair of codewords fits in LITLEN_TABLEBITS bits.  The latter never
 * holds for static Huffman blocks, and it rarely holds for data with nearly
 * uniform literal frequencies, such as already-compressed data.  When compound
 * entries are generated, the main table is made just large enough to hold all
 * of them and the longest codeword, up to LITLEN_TABLEBITS bits.  Whether
 * compound entries are generated affects performance only, not correctness, as
 * the generic loop supports them too.
 *
 * Compound entries are filled in as part of build_decode_table()'s incremental
 * table doubling, at the point where the table has '2**n' entries, where 'n' is
 * the total length of the two codewords; later doublings then replicate them.
 * Compound entries overwrite only the replicated single-literal entries for
 * their 1st symbol where it is followed by an eligible 2nd symbol; because the
 * code is prefix-free, they never collide with each other, with other
 * single-symbol entries, or with subtable pointers.  For details, see
 * build_decode_table() and fill_compound_litlen_entries() in
 * decompress_template.h.
 *
 * For the exact decode table entry format we use, including for compound
 * entries, see the definitions of the *_decode_results[] arrays below.
 */


/*
 * These are the TABLEBITS values we use for each of the DEFLATE Huffman codes,
 * along with their corresponding ENOUGH values.
 *
 * For the precode, we use PRECODE_TABLEBITS == 7 since this is the maximum
 * precode codeword length.  This avoids ever needing subtables.
 *
 * For the litlen and offset codes, we cannot realistically avoid ever needing
 * subtables, since litlen and offset codewords can be up to 15 bits.  A higher
 * TABLEBITS reduces the number of lookups that need a subtable, which increases
 * performance; however, it increases memory usage and makes building the table
 * take longer, which decreases performance.  We choose values that work well in
 * practice, making subtables rarely needed without making the tables too large.
 *
 * Our choice of OFFSET_TABLEBITS == 8 is a bit low; without any special
 * considerations, 9 would fit the trade-off curve better.  However, there is a
 * performance benefit to using exactly 8 bits when it is a compile-time
 * constant, as many CPUs can take the low byte more easily than the low 9 bits.
 *
 * zlib treats its equivalents of TABLEBITS as maximum values; whenever it
 * builds a table, it caps the actual table_bits to the longest codeword.
 * However, having the table bits be a compile-time constant is beneficial to
 * the performance of the decode loop, so there is a trade-off.  libdeflate uses
 * dynamic table_bits for the litlen table only, due to its larger maximum size:
 * when compound entries are disabled (or none fit in LITLEN_TABLEBITS bits),
 * the main table is capped at min(LITLEN_SMALL_TABLEBITS, max_codeword_len);
 * when compound entries are generated, table_bits is expanded up to
 * LITLEN_TABLEBITS as needed to fit the longest codeword and the longest
 * compound pair <= LITLEN_TABLEBITS bits.  PRECODE_TABLEBITS and
 * OFFSET_TABLEBITS are smaller, so going dynamic there isn't as useful, and
 * OFFSET_TABLEBITS=8 is useful as mentioned above.
 *
 * Each TABLEBITS value has a corresponding ENOUGH value that gives the
 * worst-case maximum number of decode table entries, including the main table
 * and all subtables.  The ENOUGH value depends on three parameters:
 *
 *	(1) the maximum number of symbols in the code (DEFLATE_NUM_*_SYMS)
 *	(2) the maximum number of main table bits (*_TABLEBITS)
 *	(3) the maximum allowed codeword length (DEFLATE_MAX_*_CODEWORD_LEN)
 *
 * The ENOUGH values were computed using scripts/enough.py.
 */
#define PRECODE_TABLEBITS	7
#define PRECODE_ENOUGH		128	/* enough.py 19 7 7	*/
#define LITLEN_SMALL_TABLEBITS	11	/* enough.py 288 11 15 = 2342 <= LITLEN_ENOUGH */
#define LITLEN_TABLEBITS	12
#define LITLEN_ENOUGH		4382	/* enough.py 288 12 15	*/
#define OFFSET_TABLEBITS	8
#define OFFSET_ENOUGH		402	/* enough.py 32 8 15	*/

/*
 * make_decode_table_entry() creates a decode table entry for the given symbol
 * by combining the static part 'decode_results[sym]' with the dynamic part
 * 'len', which is the remaining codeword length (the codeword length for main
 * table entries, or the codeword length minus TABLEBITS for subtable entries).
 *
 * In all cases, we add 'len' to each of the two low-order bytes to create the
 * appropriately-formatted decode table entry.  See the definitions of the
 * *_decode_results[] arrays below, where the entry format is described.
 */
static forceinline u32
make_decode_table_entry(const u32 decode_results[], u32 sym, u32 len)
{
	return decode_results[sym] + (len << 8) + len;
}

/*
 * Here is the format of our precode decode table entries.  Bits not explicitly
 * described contain zeroes:
 *
 *	Bit 20-16:  presym
 *	Bit 10-8:   codeword length [not used]
 *	Bit 2-0:    codeword length
 *
 * The precode decode table never has subtables, since we use
 * PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN.
 *
 * precode_decode_results[] contains the static part of the entry for each
 * symbol.  make_decode_table_entry() produces the final entries.
 */
static const u32 precode_decode_results[] = {
#define ENTRY(presym)	((u32)presym << 16)
	ENTRY(0)   , ENTRY(1)   , ENTRY(2)   , ENTRY(3)   ,
	ENTRY(4)   , ENTRY(5)   , ENTRY(6)   , ENTRY(7)   ,
	ENTRY(8)   , ENTRY(9)   , ENTRY(10)  , ENTRY(11)  ,
	ENTRY(12)  , ENTRY(13)  , ENTRY(14)  , ENTRY(15)  ,
	ENTRY(16)  , ENTRY(17)  , ENTRY(18)  ,
#undef ENTRY
};

/* Litlen and offset decode table entry flags */

/* Indicates a literal entry in the litlen decode table */
#define HUFFDEC_LITERAL			0x80000000

/*
 * Indicates that HUFFDEC_SUBTABLE_POINTER or HUFFDEC_END_OF_BLOCK is set.
 *
 * In litlen decode table entries, this bit overlaps the low bit of the 1st
 * literal.  So it's meaningful only when HUFFDEC_LITERAL is not set.
 */
#define HUFFDEC_EXCEPTIONAL		0x00008000

/*
 * Indicates a subtable pointer entry in the litlen or offset decode table.
 * Unlike HUFFDEC_EXCEPTIONAL and HUFFDEC_END_OF_BLOCK, this bit is never set
 * in literal, length, or end-of-block entries, so it may be tested before
 * HUFFDEC_LITERAL when convenient (as done in the generic loop).
 */
#define HUFFDEC_SUBTABLE_POINTER	0x00004000

/*
 * Indicates an end-of-block entry in the litlen decode table when
 * HUFFDEC_EXCEPTIONAL is also set.
 *
 * When HUFFDEC_LITERAL and HUFFDEC_EXCEPTIONAL are both clear (i.e. a length
 * entry), this same bit serves as HUFFDEC_LIT_LENGTH, indicating a fused
 * (literal, length) entry.  Because (HUFFDEC_LIT_LENGTH >> 8) == 0x20, this bit
 * lies just outside the 5-bit field extracted by '(entry >> 8) & 0x1F' (bits
 * 12-8), so it doesn't disturb the codeword length stored there.  Since this
 * bit is overloaded, HUFFDEC_END_OF_BLOCK may be tested only after
 * HUFFDEC_EXCEPTIONAL has been found to be set.
 */
#define HUFFDEC_END_OF_BLOCK		0x00002000
#define HUFFDEC_LIT_LENGTH_SHIFT	13
#define HUFFDEC_LIT_LENGTH		(1U << HUFFDEC_LIT_LENGTH_SHIFT)

/* Indicates a 2-literal entry in the litlen decode table */
#define HUFFDEC_DOUBLE_LITERAL_SHIFT	12
#define HUFFDEC_DOUBLE_LITERAL		(1U << HUFFDEC_DOUBLE_LITERAL_SHIFT)

/* Location of the literal value(s) in litlen decode table literal entries */
#define HUFFDEC_LITERAL1_SHIFT		15
#define HUFFDEC_LITERAL1_MASK		(0xFFU << HUFFDEC_LITERAL1_SHIFT)
#define HUFFDEC_LITERAL2_SHIFT		23
#define HUFFDEC_LITERAL2_MASK		(0xFFU << HUFFDEC_LITERAL2_SHIFT)

/*
 * Range of litlen match-length symbols eligible for fusion into a single
 * (literal, length) decode table entry.  Symbols 257..280 have length_base in
 * 3..115 (<= 127, fitting in bits 22-16 below the 8-bit literal in bits 30-23)
 * and at most 4 extra bits, so total consumed bits <= LITLEN_TABLEBITS + 4
 * (16 <= LENGTH_MAXFASTBITS), and total output bytes written is at most
 * 1 + 115 + 15 == 131 <= DEFLATE_MAX_MATCH_LEN.
 */
#define FIRST_SHORT_LENSYM		257
#define LAST_SHORT_LENSYM		280
#define MAX_NUM_SHORT_LENSYMS		(LAST_SHORT_LENSYM - FIRST_SHORT_LENSYM + 1)
#define SHORT_LENSYM_BASE_MASK		0x7FU
#define SHORT_LENSYM_MAX_MATCH_LEN	(115 + ((1U << 4) - 1))

/*
 * Temporary state used by build_decode_table() when generating compound
 * (2-literal and fused literal+length) litlen decode table entries:
 *
 * - lit_start[len] and lit_end[len] delimit the range of short_lits[] (see
 *   below) that holds the literals whose codewords have length 'len'.
 *
 * - lensym_start[len] and lensym_end[len] likewise delimit the range of
 *   short_lensym_res[] and short_lensym_cw[] that holds the "short" length
 *   symbols (FIRST_SHORT_LENSYM..LAST_SHORT_LENSYM) whose codewords have
 *   length 'len'.  short_lensym_res[] holds their litlen_decode_results[]
 *   values, and short_lensym_cw[] holds their (bit-reversed) codewords.
 *
 * - min_lit_cw_len and min_lensym_cw_len are the shortest codeword lengths of
 *   any literal and any short length symbol, respectively, or LITLEN_TABLEBITS
 *   + 1 if there is no such symbol of length < LITLEN_TABLEBITS.
 *
 * Ranges for codeword lengths that have no such symbols are empty (start ==
 * end == 0).  Since each symbol in a pair needs at least 1 bit, only codeword
 * lengths up to SHORT_LIT_MAX_CW_LEN == LITLEN_TABLEBITS - 1 are needed.
 */
struct litlen_compound_syms {
	u16 lit_start[LITLEN_TABLEBITS];
	u16 lit_end[LITLEN_TABLEBITS];
	u8 lensym_start[LITLEN_TABLEBITS];
	u8 lensym_end[LITLEN_TABLEBITS];
	u32 short_lensym_res[MAX_NUM_SHORT_LENSYMS];
	u16 short_lensym_cw[MAX_NUM_SHORT_LENSYMS];
	unsigned min_lit_cw_len;
	unsigned min_lensym_cw_len;
};

/*
 * Format of the temporary short_lits[] entries that build_decode_table() uses
 * to generate compound (2-literal and fused literal+length) entries of the
 * litlen decode table:
 *
 *	Bit 22-15:  literal value (at HUFFDEC_LITERAL1_SHIFT)
 *	Bit 10-0:   codeword (bit-reversed)
 *
 * All other bits are 0, so the literal value can be isolated with
 * '~SHORT_LIT_CODEWORD_MASK' (and shifted left when it needs to go at
 * HUFFDEC_LITERAL2_SHIFT).  Since each symbol in a compound pair uses at least
 * 1 bit and the total codeword length is at most LITLEN_TABLEBITS, the codeword
 * length of each one is at most LITLEN_TABLEBITS - 1.
 */
#define SHORT_LIT_MAX_CW_LEN		(LITLEN_TABLEBITS - 1)
#define SHORT_LIT_CODEWORD_MASK		((1U << SHORT_LIT_MAX_CW_LEN) - 1)
#define MAKE_SHORT_LIT(sym, codeword)	\
	(((u32)(sym) << HUFFDEC_LITERAL1_SHIFT) | (codeword))

/*
 * Write the 1 or 2 literals given by the literal entry 'entry' to 'out_next',
 * and advance 'out_next' by the number of literals.  For efficiency, this
 * always writes 2 bytes, even if there is only 1 literal.  (In that case, the
 * extra byte is 0 and is written just past the new 'out_next'.)  Therefore,
 * this can be used only in the fastloop, where such overruns are allowed.
 */
#define WRITE_FASTLOOP_LITERALS(entry)					\
do {									\
	put_unaligned_le16((u16)((entry) >> HUFFDEC_LITERAL1_SHIFT),	\
			   out_next);					\
	out_next += 1 + (((entry) >> HUFFDEC_DOUBLE_LITERAL_SHIFT) & 1);\
} while (0)

/* Maximum number of bits that can be consumed by decoding a match length */
#define LENGTH_MAXBITS		(DEFLATE_MAX_LITLEN_CODEWORD_LEN + \
				 DEFLATE_MAX_EXTRA_LENGTH_BITS)
#define LENGTH_MAXFASTBITS	(LITLEN_TABLEBITS /* no subtable needed */ + \
				 DEFLATE_MAX_EXTRA_LENGTH_BITS)

/*
 * Here is the format of our litlen decode table entries.  Bits not explicitly
 * described contain zeroes:
 *
 *	Literals (1 or 2 literals):
 *		Bit 31:     1 (HUFFDEC_LITERAL)
 *		Bit 30-23:  2nd literal value (if HUFFDEC_DOUBLE_LITERAL; 0 otherwise)
 *		Bit 22-15:  1st literal value (overlaps HUFFDEC_EXCEPTIONAL)
 *		Bit 14:     0 (!HUFFDEC_SUBTABLE_POINTER)
 *		Bit 13:     0 (!HUFFDEC_END_OF_BLOCK)
 *		Bit 12:     1 if 2 literals (HUFFDEC_DOUBLE_LITERAL), else 0
 *		Bit 11-8:   remaining codeword length (0 if double literal) [not used]
 *		Bit 3-0:    remaining codeword length (1st + 2nd if double literal)
 *	Lengths (plain or fused literal+length):
 *		Bit 31:     0 (!HUFFDEC_LITERAL)
 *		Bit 30-23:  preceding literal (if HUFFDEC_LIT_LENGTH), else
 *		            bits 24-23 hold upper bits of length_base (30-25 zero)
 *		Bit 22-16:  length base value (3..115 if HUFFDEC_LIT_LENGTH;
 *		            low 7 bits of 3..258 otherwise)
 *		Bit 15:     0 (!HUFFDEC_EXCEPTIONAL)
 *		Bit 14:     0 (!HUFFDEC_SUBTABLE_POINTER)
 *		Bit 13:     1 if preceding literal present (HUFFDEC_LIT_LENGTH), else 0
 *		Bit 12:     0 (!HUFFDEC_DOUBLE_LITERAL)
 *		Bit 11-8:   remaining codeword length (lit+len sum if fused)
 *		Bit 4-0:    remaining codeword length + number of extra bits
 *	End of block:
 *		Bit 31:     0 (!HUFFDEC_LITERAL)
 *		Bit 15:     1 (HUFFDEC_EXCEPTIONAL)
 *		Bit 14:     0 (!HUFFDEC_SUBTABLE_POINTER)
 *		Bit 13:     1 (HUFFDEC_END_OF_BLOCK)
 *		Bit 12:     0 (!HUFFDEC_DOUBLE_LITERAL)
 *		Bit 11-8:   remaining codeword length [not used]
 *		Bit 3-0:    remaining codeword length
 *	Subtable pointer:
 *		Bit 31:     0 (!HUFFDEC_LITERAL)
 *		Bit 30-16:  index of start of subtable
 *		Bit 15:     1 (HUFFDEC_EXCEPTIONAL)
 *		Bit 14:     1 (HUFFDEC_SUBTABLE_POINTER)
 *		Bit 13:     0 (!HUFFDEC_END_OF_BLOCK)
 *		Bit 12:     0 (!HUFFDEC_DOUBLE_LITERAL)
 *		Bit 11-8:   number of subtable bits
 *		Bit 3-0:    number of main table bits
 *
 * This format has several desirable properties:
 *
 *	- The codeword length, length slot base, and number of extra length bits
 *	  are all built in.  This eliminates the need to separately look up this
 *	  information by indexing separate arrays by symbol or length slot.
 *
 *	- The HUFFDEC_* flags enable easily distinguishing between the different
 *	  types of entries.  The HUFFDEC_LITERAL flag enables a fast path for
 *	  literals; the high bit is used for this, as some CPUs can test the
 *	  high bit more easily than other bits.  Once literals have been
 *	  excluded, the HUFFDEC_EXCEPTIONAL flag makes it possible to detect
 *	  the two unlikely cases (subtable pointer and end of block) in a
 *	  single bit flag test.  (HUFFDEC_EXCEPTIONAL must not be tested
 *	  before HUFFDEC_LITERAL, as it overlaps the low bit of the 1st
 *	  literal.)
 *
 *	- The literal value(s) are located such that
 *	  'entry >> HUFFDEC_LITERAL1_SHIFT' has the 1st literal in bits 7-0 and
 *	  the 2nd literal (or 0 for single-literal entries) in bits 15-8, so
 *	  WRITE_FASTLOOP_LITERALS() can store both in order via a single
 *	  put_unaligned_le16() call (even on big-endian CPUs).
 *
 *	- The low byte is the number of bits that need to be removed from the
 *	  bitstream, including any extra bits so that they don't need to be
 *	  removed separately.  Ignoring bits 8+ of 'bitsleft' allows
 *	  'bitsleft -= entry' instead of 'bitsleft -= (u8)entry'.  Furthermore,
 *	  the low byte is always less than 32 (so bits 7-5 are always 0), which
 *	  allows 'bitbuf >>= entry & (BITBUF_NBITS - 1)' instead of
 *	  'bitbuf >>= (u8)entry' on both 32-bit and 64-bit platforms (letting
 *	  the mask fold into the shift instruction) and allows
 *	  EXTRACT_VARBITS8(word, entry) to use 'entry' without masking to u8.
 *
 *	- Bit 12 is 0 when either the "remaining codeword length" or "number of
 *	  subtable bits" in bits 11-8 is needed (i.e. on length and subtable
 *	  pointer entries), allowing bits 11-8 to be extracted with '& 0x1F'
 *	  rather than '& 0xF'.  When used to shift a 32-bit value, this masking
 *	  happens implicitly on most architectures via the shift amount being
 *	  truncated, saving an 'and' instruction.
 *
 * litlen_decode_results[] contains the static part of the entry for each
 * symbol.  make_decode_table_entry() produces the final entries.
 */
static const u32 litlen_decode_results[] = {

	/* Literals */
#define ENTRY(literal)	(HUFFDEC_LITERAL | \
			 ((u32)(literal) << HUFFDEC_LITERAL1_SHIFT))
	ENTRY(0)   , ENTRY(1)   , ENTRY(2)   , ENTRY(3)   ,
	ENTRY(4)   , ENTRY(5)   , ENTRY(6)   , ENTRY(7)   ,
	ENTRY(8)   , ENTRY(9)   , ENTRY(10)  , ENTRY(11)  ,
	ENTRY(12)  , ENTRY(13)  , ENTRY(14)  , ENTRY(15)  ,
	ENTRY(16)  , ENTRY(17)  , ENTRY(18)  , ENTRY(19)  ,
	ENTRY(20)  , ENTRY(21)  , ENTRY(22)  , ENTRY(23)  ,
	ENTRY(24)  , ENTRY(25)  , ENTRY(26)  , ENTRY(27)  ,
	ENTRY(28)  , ENTRY(29)  , ENTRY(30)  , ENTRY(31)  ,
	ENTRY(32)  , ENTRY(33)  , ENTRY(34)  , ENTRY(35)  ,
	ENTRY(36)  , ENTRY(37)  , ENTRY(38)  , ENTRY(39)  ,
	ENTRY(40)  , ENTRY(41)  , ENTRY(42)  , ENTRY(43)  ,
	ENTRY(44)  , ENTRY(45)  , ENTRY(46)  , ENTRY(47)  ,
	ENTRY(48)  , ENTRY(49)  , ENTRY(50)  , ENTRY(51)  ,
	ENTRY(52)  , ENTRY(53)  , ENTRY(54)  , ENTRY(55)  ,
	ENTRY(56)  , ENTRY(57)  , ENTRY(58)  , ENTRY(59)  ,
	ENTRY(60)  , ENTRY(61)  , ENTRY(62)  , ENTRY(63)  ,
	ENTRY(64)  , ENTRY(65)  , ENTRY(66)  , ENTRY(67)  ,
	ENTRY(68)  , ENTRY(69)  , ENTRY(70)  , ENTRY(71)  ,
	ENTRY(72)  , ENTRY(73)  , ENTRY(74)  , ENTRY(75)  ,
	ENTRY(76)  , ENTRY(77)  , ENTRY(78)  , ENTRY(79)  ,
	ENTRY(80)  , ENTRY(81)  , ENTRY(82)  , ENTRY(83)  ,
	ENTRY(84)  , ENTRY(85)  , ENTRY(86)  , ENTRY(87)  ,
	ENTRY(88)  , ENTRY(89)  , ENTRY(90)  , ENTRY(91)  ,
	ENTRY(92)  , ENTRY(93)  , ENTRY(94)  , ENTRY(95)  ,
	ENTRY(96)  , ENTRY(97)  , ENTRY(98)  , ENTRY(99)  ,
	ENTRY(100) , ENTRY(101) , ENTRY(102) , ENTRY(103) ,
	ENTRY(104) , ENTRY(105) , ENTRY(106) , ENTRY(107) ,
	ENTRY(108) , ENTRY(109) , ENTRY(110) , ENTRY(111) ,
	ENTRY(112) , ENTRY(113) , ENTRY(114) , ENTRY(115) ,
	ENTRY(116) , ENTRY(117) , ENTRY(118) , ENTRY(119) ,
	ENTRY(120) , ENTRY(121) , ENTRY(122) , ENTRY(123) ,
	ENTRY(124) , ENTRY(125) , ENTRY(126) , ENTRY(127) ,
	ENTRY(128) , ENTRY(129) , ENTRY(130) , ENTRY(131) ,
	ENTRY(132) , ENTRY(133) , ENTRY(134) , ENTRY(135) ,
	ENTRY(136) , ENTRY(137) , ENTRY(138) , ENTRY(139) ,
	ENTRY(140) , ENTRY(141) , ENTRY(142) , ENTRY(143) ,
	ENTRY(144) , ENTRY(145) , ENTRY(146) , ENTRY(147) ,
	ENTRY(148) , ENTRY(149) , ENTRY(150) , ENTRY(151) ,
	ENTRY(152) , ENTRY(153) , ENTRY(154) , ENTRY(155) ,
	ENTRY(156) , ENTRY(157) , ENTRY(158) , ENTRY(159) ,
	ENTRY(160) , ENTRY(161) , ENTRY(162) , ENTRY(163) ,
	ENTRY(164) , ENTRY(165) , ENTRY(166) , ENTRY(167) ,
	ENTRY(168) , ENTRY(169) , ENTRY(170) , ENTRY(171) ,
	ENTRY(172) , ENTRY(173) , ENTRY(174) , ENTRY(175) ,
	ENTRY(176) , ENTRY(177) , ENTRY(178) , ENTRY(179) ,
	ENTRY(180) , ENTRY(181) , ENTRY(182) , ENTRY(183) ,
	ENTRY(184) , ENTRY(185) , ENTRY(186) , ENTRY(187) ,
	ENTRY(188) , ENTRY(189) , ENTRY(190) , ENTRY(191) ,
	ENTRY(192) , ENTRY(193) , ENTRY(194) , ENTRY(195) ,
	ENTRY(196) , ENTRY(197) , ENTRY(198) , ENTRY(199) ,
	ENTRY(200) , ENTRY(201) , ENTRY(202) , ENTRY(203) ,
	ENTRY(204) , ENTRY(205) , ENTRY(206) , ENTRY(207) ,
	ENTRY(208) , ENTRY(209) , ENTRY(210) , ENTRY(211) ,
	ENTRY(212) , ENTRY(213) , ENTRY(214) , ENTRY(215) ,
	ENTRY(216) , ENTRY(217) , ENTRY(218) , ENTRY(219) ,
	ENTRY(220) , ENTRY(221) , ENTRY(222) , ENTRY(223) ,
	ENTRY(224) , ENTRY(225) , ENTRY(226) , ENTRY(227) ,
	ENTRY(228) , ENTRY(229) , ENTRY(230) , ENTRY(231) ,
	ENTRY(232) , ENTRY(233) , ENTRY(234) , ENTRY(235) ,
	ENTRY(236) , ENTRY(237) , ENTRY(238) , ENTRY(239) ,
	ENTRY(240) , ENTRY(241) , ENTRY(242) , ENTRY(243) ,
	ENTRY(244) , ENTRY(245) , ENTRY(246) , ENTRY(247) ,
	ENTRY(248) , ENTRY(249) , ENTRY(250) , ENTRY(251) ,
	ENTRY(252) , ENTRY(253) , ENTRY(254) , ENTRY(255) ,
#undef ENTRY

	/* End of block */
	HUFFDEC_EXCEPTIONAL | HUFFDEC_END_OF_BLOCK,

	/* Lengths */
#define ENTRY(length_base, num_extra_bits)	\
	(((u32)(length_base) << 16) | (num_extra_bits))
	ENTRY(3  , 0) , ENTRY(4  , 0) , ENTRY(5  , 0) , ENTRY(6  , 0),
	ENTRY(7  , 0) , ENTRY(8  , 0) , ENTRY(9  , 0) , ENTRY(10 , 0),
	ENTRY(11 , 1) , ENTRY(13 , 1) , ENTRY(15 , 1) , ENTRY(17 , 1),
	ENTRY(19 , 2) , ENTRY(23 , 2) , ENTRY(27 , 2) , ENTRY(31 , 2),
	ENTRY(35 , 3) , ENTRY(43 , 3) , ENTRY(51 , 3) , ENTRY(59 , 3),
	ENTRY(67 , 4) , ENTRY(83 , 4) , ENTRY(99 , 4) , ENTRY(115, 4),
	ENTRY(131, 5) , ENTRY(163, 5) , ENTRY(195, 5) , ENTRY(227, 5),
	ENTRY(258, 0) , ENTRY(258, 0) , ENTRY(258, 0) ,
#undef ENTRY
};

/* Maximum number of bits that can be consumed by decoding a match offset */
#define OFFSET_MAXBITS		(DEFLATE_MAX_OFFSET_CODEWORD_LEN + \
				 DEFLATE_MAX_EXTRA_OFFSET_BITS)
#define OFFSET_MAXFASTBITS	(OFFSET_TABLEBITS /* no subtable needed */ + \
				 DEFLATE_MAX_EXTRA_OFFSET_BITS)

/*
 * Here is the format of our offset decode table entries.  Bits not explicitly
 * described contain zeroes:
 *
 *	Offsets:
 *		Bit 31-16:  offset base value
 *		Bit 15:     0 (!HUFFDEC_EXCEPTIONAL)
 *		Bit 14:     0 (!HUFFDEC_SUBTABLE_POINTER)
 *		Bit 11-8:   remaining codeword length
 *		Bit 4-0:    remaining codeword length + number of extra bits
 *	Subtable pointer:
 *		Bit 31-16:  index of start of subtable
 *		Bit 15:     1 (HUFFDEC_EXCEPTIONAL)
 *		Bit 14:     1 (HUFFDEC_SUBTABLE_POINTER)
 *		Bit 11-8:   number of subtable bits
 *		Bit 3-0:    number of main table bits
 *
 * These work the same way as the length entries (without fused literals) and
 * the subtable pointer entries, respectively, in the litlen decode table.  See
 * litlen_decode_results[] above.
 */
static const u32 offset_decode_results[] = {
#define ENTRY(offset_base, num_extra_bits)	\
	(((u32)(offset_base) << 16) | (num_extra_bits))
	ENTRY(1     , 0)  , ENTRY(2     , 0)  , ENTRY(3     , 0)  , ENTRY(4     , 0)  ,
	ENTRY(5     , 1)  , ENTRY(7     , 1)  , ENTRY(9     , 2)  , ENTRY(13    , 2) ,
	ENTRY(17    , 3)  , ENTRY(25    , 3)  , ENTRY(33    , 4)  , ENTRY(49    , 4)  ,
	ENTRY(65    , 5)  , ENTRY(97    , 5)  , ENTRY(129   , 6)  , ENTRY(193   , 6)  ,
	ENTRY(257   , 7)  , ENTRY(385   , 7)  , ENTRY(513   , 8)  , ENTRY(769   , 8)  ,
	ENTRY(1025  , 9)  , ENTRY(1537  , 9)  , ENTRY(2049  , 10) , ENTRY(3073  , 10) ,
	ENTRY(4097  , 11) , ENTRY(6145  , 11) , ENTRY(8193  , 12) , ENTRY(12289 , 12) ,
	ENTRY(16385 , 13) , ENTRY(24577 , 13) , ENTRY(24577 , 13) , ENTRY(24577 , 13) ,
#undef ENTRY
};

/*
 * The main DEFLATE decompressor structure.  Since libdeflate only supports
 * full-buffer decompression, this structure doesn't store the entire
 * decompression state, most of which is in stack variables.  Instead, this
 * struct just contains the decode tables and some temporary arrays used for
 * building them, as these are too large to comfortably allocate on the stack.
 *
 * Storing the decode tables in the decompressor struct also allows the decode
 * tables for the static codes to be reused whenever two static Huffman blocks
 * are decoded without an intervening dynamic block, even across streams.
 */
struct libdeflate_decompressor {

	/*
	 * The arrays aren't all needed at the same time.  'precode_lens' and
	 * 'precode_decode_table' are unneeded after 'lens' has been filled.
	 * Furthermore, 'lens' need not be retained after building the litlen
	 * and offset decode tables.  In fact, 'lens' can be in union with
	 * 'litlen_decode_table' provided that 'offset_decode_table' is separate
	 * and is built first.
	 */

	union {
		u8 precode_lens[DEFLATE_NUM_PRECODE_SYMS];

		struct {
			u8 lens[DEFLATE_NUM_LITLEN_SYMS +
				DEFLATE_NUM_OFFSET_SYMS +
				DEFLATE_MAX_LENS_OVERRUN];

			u32 precode_decode_table[PRECODE_ENOUGH];
		} l;

		u32 litlen_decode_table[LITLEN_ENOUGH];
	} u;

	u32 offset_decode_table[OFFSET_ENOUGH];

	/* used only during build_decode_table() */
	u16 sorted_syms[DEFLATE_MAX_NUM_SYMS];

	bool static_codes_loaded;
	unsigned litlen_tablebits;

	/* The free() function for this struct, chosen at allocation time */
	free_func_t free_func;
};

#if MLSD_ENABLED
/*
 * The lanes' state during a libdeflate_deflate_decompress_ex() call, kept in
 * memory so that it takes no registers from the rest of the decompressor
 */
struct mlsd_state {
	/*
	 * The lanes' bits per step (times 16) and margin, carried across
	 * blocks as differences from their initial values
	 */
	size_t rate16;
	size_t margin;
	uintptr_t block_start;		/* where this block's output starts */
	size_t prev_block_out;		/* output bytes of the previous block */
	uintptr_t usable;		/* mlsd_usable(): 0 = unknown, 1, 2 = no */
	u8 *scratch;			/* the lanes' scratch, or 'out' if none */
	/* The numbers of codeword lengths in the scratch */
	unsigned num_litlen_syms;
	unsigned num_offset_syms;
};

/* What mlsd_build_table() keeps for each codeword, in the records' space */
struct mlsd_build_tmp {
	u16 end[DEFLATE_MAX_CODEWORD_LEN + 1];
	u16 lit_end[DEFLATE_MAX_CODEWORD_LEN + 1];
	u16 slow[DEFLATE_NUM_LITLEN_SYMS - DEFLATE_FIRST_LEN_SYM];
	u16 rc[1 + DEFLATE_MAX_NUM_SYMS];
	u64 ent[1 + DEFLATE_MAX_NUM_SYMS];
};

/*
 * The lanes' records for a round (two with MLSD_INTERLEAVE), their decode
 * tables and the block's codeword lengths (which the litlen decode table
 * overwrites in the decompressor), kept at the end of the output buffer
 */
struct mlsd_scratch {
	union {
		u64 recs[1 + MLSD_INTERLEAVE][MLSD_NREC + 1][MLSD_LANES];
		struct mlsd_build_tmp tmp;
	} u;
#ifdef ARCH_X86_64	/* one array: one register for both tables */
	u64 lt[LITLEN_ENOUGH + OFFSET_ENOUGH + 256];
#  define MLSD_OT(s, i)	((s)->lt[LITLEN_ENOUGH + (i)])
#else
	u64 lt[LITLEN_ENOUGH];
	u64 ot[OFFSET_ENOUGH + 256];
#  define MLSD_OT(s, i)	((s)->ot[i])
#endif
	u8 lens[DEFLATE_NUM_LITLEN_SYMS + DEFLATE_NUM_OFFSET_SYMS + 16];
};
#endif

/*****************************************************************************
 *                         Main decompression routine
 *****************************************************************************/

typedef enum libdeflate_result (*decompress_func_t)
	(struct libdeflate_decompressor * restrict d,
	 const void * restrict in, size_t in_nbytes,
	 void * restrict out, size_t out_nbytes_avail,
	 size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret);

struct fastloop_ctx {
	/*
	 * The 'out' pointer is an input only.  Put it at offset 0 because
	 * decompress_fastloop_checked() loads it on each match, as that was
	 * seen to reduce register spilling elsewhere in some cases.
	 */
	const u8 *out;
	/* The following are both inputs and outputs to the fastloop. */
	const u8 *in_next;
	u8 *out_next;
	bitbuf_t bitbuf;
	u32 bitsleft;
};

enum fastloop_result {
	FASTLOOP_BLOCK_NOT_DONE,
	FASTLOOP_BLOCK_DONE,
	FASTLOOP_BAD_DATA,
};

/* Include architecture-specific implementation(s) if available. */
#undef DEFAULT_IMPL
#undef arch_select_decompress_func
#if defined(ARCH_X86_32) || defined(ARCH_X86_64)
#  include "x86/decompress_impl.h"
#elif defined(ARCH_ARM64)
#  include "arm/decompress_impl.h"
#endif

#ifndef DEFAULT_IMPL
#  define SUFFIX _default
#  undef ATTRIBUTES
#  undef EXTRACT_VARBITS
#  undef EXTRACT_VARBITS8
#  undef COPY_MATCH
#  include "decompress_template.h"
#  define DEFAULT_IMPL deflate_decompress_default
#endif

#ifdef arch_select_decompress_func
static enum libdeflate_result
dispatch_decomp(struct libdeflate_decompressor * restrict d,
		const void * restrict in, size_t in_nbytes,
		void * restrict out, size_t out_nbytes_avail,
		size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret);

static LIBDEFLATE_ATOMIC(decompress_func_t) decompress_func = dispatch_decomp;

/* Choose the best implementation at runtime. */
static enum libdeflate_result
dispatch_decomp(struct libdeflate_decompressor * restrict d,
		const void * restrict in, size_t in_nbytes,
		void * restrict out, size_t out_nbytes_avail,
		size_t *actual_in_nbytes_ret, size_t *actual_out_nbytes_ret)
{
	decompress_func_t f = arch_select_decompress_func();

	if (f == NULL)
		f = DEFAULT_IMPL;

	LIBDEFLATE_ATOMIC_STORE_RELAXED(&decompress_func, f);
	return f(d, in, in_nbytes, out, out_nbytes_avail,
		 actual_in_nbytes_ret, actual_out_nbytes_ret);
}
#  define decompress_impl LIBDEFLATE_ATOMIC_LOAD_RELAXED(&decompress_func)
#else
/* The best implementation is statically known, so call it directly. */
#  define decompress_impl DEFAULT_IMPL
#endif

/*
 * This is the main DEFLATE decompression routine.  See libdeflate.h for the
 * documentation.
 *
 * Note that the real code is in decompress_template.h.  The part here just
 * handles calling the appropriate implementation depending on the CPU features
 * at runtime.
 */
LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress_ex(struct libdeflate_decompressor *d,
				 const void *in, size_t in_nbytes,
				 void *out, size_t out_nbytes_avail,
				 size_t *actual_in_nbytes_ret,
				 size_t *actual_out_nbytes_ret)
{
	static u8 dummy;

	/*
	 * Buffers larger than PTRDIFF_MAX cannot be accepted, since pointer
	 * arithmetic would not be fully defined on such buffers.
	 *
	 * Also sanitize zero-length buffers, so that decompress_impl() doesn't
	 * do 'NULL + 0' which is undefined behavior before C29.
	 * 'in_nbytes == 0' cannot be a valid DEFLATE stream, so just return
	 * LIBDEFLATE_BAD_DATA right away for it.  'out_nbytes_avail == 0' is
	 * potentially valid, since a DEFLATE stream can decompress to zero
	 * bytes, so in that case just give 'out' a non-NULL address.
	 */
	if (unlikely(in_nbytes == 0 || in_nbytes > PTRDIFF_MAX))
		return LIBDEFLATE_BAD_DATA;
	if (unlikely(out_nbytes_avail == 0))
		out = &dummy;
	else if (unlikely(out_nbytes_avail > PTRDIFF_MAX))
		return LIBDEFLATE_BAD_DATA;

	return decompress_impl(d, in, in_nbytes, out, out_nbytes_avail,
			       actual_in_nbytes_ret, actual_out_nbytes_ret);
}

LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor *d,
			      const void *in, size_t in_nbytes,
			      void *out, size_t out_nbytes_avail,
			      size_t *actual_out_nbytes_ret)
{
	return libdeflate_deflate_decompress_ex(d, in, in_nbytes,
						out, out_nbytes_avail,
						NULL, actual_out_nbytes_ret);
}

LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor_ex(const struct libdeflate_options *options)
{
	struct libdeflate_decompressor *d;

	/*
	 * Note: if more fields are added to libdeflate_options, this code will
	 * need to be updated to support both the old and new structs.
	 */
	if (options->sizeof_options != sizeof(*options))
		return NULL;

	d = (options->malloc_func ? options->malloc_func :
	     libdeflate_default_malloc_func)(sizeof(*d));
	if (d == NULL)
		return NULL;
	/*
	 * Note that only certain parts of the decompressor actually must be
	 * initialized here:
	 *
	 * - 'static_codes_loaded' must be initialized to false.
	 *
	 * - The first half of the main portion of each decode table must be
	 *   initialized to any value, to avoid reading from uninitialized
	 *   memory during table expansion in build_decode_table().  (Although,
	 *   this is really just to avoid warnings with dynamic tools like
	 *   valgrind, since build_decode_table() is guaranteed to initialize
	 *   all entries eventually anyway.)
	 *
	 * - 'free_func' must be set.
	 *
	 * But for simplicity, we currently just zero the whole decompressor.
	 */
	memset(d, 0, sizeof(*d));
	d->free_func = options->free_func ?
		       options->free_func : libdeflate_default_free_func;
	return d;
}

LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor(void)
{
	static const struct libdeflate_options defaults = {
		.sizeof_options = sizeof(defaults),
	};
	return libdeflate_alloc_decompressor_ex(&defaults);
}

LIBDEFLATEAPI void
libdeflate_free_decompressor(struct libdeflate_decompressor *d)
{
	if (d)
		d->free_func(d);
}
