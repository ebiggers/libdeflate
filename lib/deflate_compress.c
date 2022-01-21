/*
 * deflate_compress.c - a compressor for DEFLATE
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

#include "deflate_compress.h"
#include "deflate_constants.h"
#include "unaligned.h"

#include "libdeflate.h"

/******************************************************************************/

/*
 * The following parameters can be changed at build time to customize the
 * compression algorithms slightly:
 *
 * (Note, not all customizable parameters are here.  Some others can be found in
 * libdeflate_alloc_compressor() and in *_matchfinder.h.)
 */

/*
 * If this parameter is defined to 1, then the near-optimal parsing algorithm
 * will be included, and compression levels 10-12 will use it.  This algorithm
 * usually produces a compression ratio significantly better than the other
 * algorithms.  However, it is slow.  If this parameter is defined to 0, then
 * levels 10-12 will be the same as level 9 and will use the lazy2 algorithm.
 */
#define SUPPORT_NEAR_OPTIMAL_PARSING	1

/*
 * This is the minimum block length that the compressor will use, in
 * uncompressed bytes.  It is also approximately the amount by which the final
 * block is allowed to grow past the soft maximum length in order to avoid using
 * a very short block at the end.  This should be a value below which using
 * shorter blocks is unlikely to be worthwhile, due to the per-block overhead.
 *
 * Defining a fixed minimum block length is needed in order to guarantee a
 * reasonable upper bound on the compressed size.  It's also needed because our
 * block splitting algorithm doesn't work well on very short blocks.
 */
#define MIN_BLOCK_LENGTH	5000

/*
 * For the greedy, lazy, lazy2, and near-optimal compressors: This is the soft
 * maximum block length, in uncompressed bytes.  The compressor will try to end
 * blocks at this length, but it may go slightly past it if there is a match
 * that straddles this limit or if the input data ends soon after this limit.
 * This parameter doesn't apply to uncompressed blocks, which the DEFLATE format
 * limits to 65535 bytes.
 *
 * This should be a value above which it is very likely that splitting the block
 * would produce a better compression ratio.  For the near-optimal compressor,
 * increasing/decreasing this parameter will increase/decrease per-compressor
 * memory usage linearly.
 */
#define SOFT_MAX_BLOCK_LENGTH	300000

/*
 * For the greedy, lazy, and lazy2 compressors: this is the length of the
 * sequence store, which is an array where the compressor temporarily stores
 * matches that it's going to use in the current block.  This value is the
 * maximum number of matches that can be used in a block.  If the sequence store
 * fills up, then the compressor will be forced to end the block early.  This
 * value should be large enough so that this rarely happens, due to the block
 * being ended normally before then.  Increasing/decreasing this value will
 * increase/decrease per-compressor memory usage linearly.
 */
#define SEQ_STORE_LENGTH	50000

/*
 * For deflate_compress_fastest(): This is the soft maximum block length.
 * deflate_compress_fastest() doesn't use the regular block splitting algorithm;
 * it only ends blocks when they reach FAST_SOFT_MAX_BLOCK_LENGTH bytes or
 * FAST_SEQ_STORE_LENGTH matches.  Therefore, this value should be lower than
 * the regular SOFT_MAX_BLOCK_LENGTH.
 */
#define FAST_SOFT_MAX_BLOCK_LENGTH	65535

/*
 * For deflate_compress_fastest(): this is the length of the sequence store.
 * This is like SEQ_STORE_LENGTH, but this should be a lower value.
 */
#define FAST_SEQ_STORE_LENGTH	8192

/*
 * These are the maximum codeword lengths, in bits, the compressor will use for
 * each Huffman code.  The DEFLATE format defines limits for these.  However,
 * further limiting litlen codewords to 14 bits is beneficial, since it has
 * negligible effect on compression ratio but allows some optimizations when
 * outputting bits.  (It allows 4 literals to be written at once rather than 3.)
 */
#define MAX_LITLEN_CODEWORD_LEN		14
#define MAX_OFFSET_CODEWORD_LEN		DEFLATE_MAX_OFFSET_CODEWORD_LEN
#define MAX_PRE_CODEWORD_LEN		DEFLATE_MAX_PRE_CODEWORD_LEN

#if SUPPORT_NEAR_OPTIMAL_PARSING

/* Parameters specific to the near-optimal parsing algorithm */

/*
 * BIT_COST is a scaling factor that allows the near-optimal compressor to
 * consider fractional bit costs when deciding which literal/match sequence to
 * use.  This is useful when the true symbol costs are unknown.  For example, if
 * the compressor thinks that a symbol has 6.5 bits of entropy, it can set its
 * cost to 6.5 bits rather than have to use 6 or 7 bits.  Although in the end
 * each symbol will use a whole number of bits due to the Huffman coding,
 * considering fractional bits can be helpful due to the limited information.
 *
 * BIT_COST should be a power of 2.  A value of 8 or 16 works well.  A higher
 * value isn't very useful since the calculations are approximate anyway.
 *
 * BIT_COST doesn't apply to deflate_flush_block(), which considers whole bits.
 */
#define BIT_COST	16

/*
 * The NOSTAT_BITS value for a given alphabet is the number of bits assumed to
 * be needed to output a symbol that was unused in the previous optimization
 * pass.  Assigning a default cost allows the symbol to be used in the next
 * optimization pass.  However, the cost should be relatively high because the
 * symbol probably won't be used very many times (if at all).
 */
#define LITERAL_NOSTAT_BITS	13
#define LENGTH_NOSTAT_BITS	13
#define OFFSET_NOSTAT_BITS	10

/*
 * This is (slightly less than) the maximum number of matches that the
 * near-optimal compressor will cache per block.  This behaves similarly to
 * SEQ_STORE_LENGTH for the other compressors.
 */
#define MATCH_CACHE_LENGTH	(SOFT_MAX_BLOCK_LENGTH * 5)

#endif /* SUPPORT_NEAR_OPTIMAL_PARSING */

/******************************************************************************/

/* Include the needed matchfinders. */
#define MATCHFINDER_WINDOW_ORDER	DEFLATE_WINDOW_ORDER
#include "hc_matchfinder.h"
#include "ht_matchfinder.h"
#if SUPPORT_NEAR_OPTIMAL_PARSING
#  include "bt_matchfinder.h"
/*
 * This is the maximum number of matches the binary trees matchfinder can find
 * at a single position.  Since the matchfinder never finds more than one match
 * for the same length, presuming one of each possible length is sufficient for
 * an upper bound.  (This says nothing about whether it is worthwhile to
 * consider so many matches; this is just defining the worst case.)
 */
#define MAX_MATCHES_PER_POS	\
	(DEFLATE_MAX_MATCH_LEN - DEFLATE_MIN_MATCH_LEN + 1)
#endif

/*
 * The largest block length we will ever use is when the final block is of
 * length SOFT_MAX_BLOCK_LENGTH + MIN_BLOCK_LENGTH - 1, or when any block is of
 * length SOFT_MAX_BLOCK_LENGTH + 1 + DEFLATE_MAX_MATCH_LEN.  The latter case
 * occurs when the lazy2 compressor chooses two literals and a maximum-length
 * match, starting at SOFT_MAX_BLOCK_LENGTH - 1.
 */
#define MAX_BLOCK_LENGTH	\
	MAX(SOFT_MAX_BLOCK_LENGTH + MIN_BLOCK_LENGTH - 1,	\
	    SOFT_MAX_BLOCK_LENGTH + 1 + DEFLATE_MAX_MATCH_LEN)

static forceinline void
check_buildtime_parameters(void)
{
	/*
	 * Verify that MIN_BLOCK_LENGTH is being honored, as
	 * libdeflate_deflate_compress_bound() depends on it.
	 */
	STATIC_ASSERT(SOFT_MAX_BLOCK_LENGTH >= MIN_BLOCK_LENGTH);
	STATIC_ASSERT(FAST_SOFT_MAX_BLOCK_LENGTH >= MIN_BLOCK_LENGTH);
	STATIC_ASSERT(SEQ_STORE_LENGTH * DEFLATE_MIN_MATCH_LEN >=
		      MIN_BLOCK_LENGTH);
	STATIC_ASSERT(FAST_SEQ_STORE_LENGTH * HT_MATCHFINDER_MIN_MATCH_LEN >=
		      MIN_BLOCK_LENGTH);

	/* The definition of MAX_BLOCK_LENGTH assumes this. */
	STATIC_ASSERT(FAST_SOFT_MAX_BLOCK_LENGTH <= SOFT_MAX_BLOCK_LENGTH);

	/* Verify that the sequence stores aren't uselessly large. */
	STATIC_ASSERT(SEQ_STORE_LENGTH * DEFLATE_MIN_MATCH_LEN <=
		      SOFT_MAX_BLOCK_LENGTH + MIN_BLOCK_LENGTH);
	STATIC_ASSERT(FAST_SEQ_STORE_LENGTH * HT_MATCHFINDER_MIN_MATCH_LEN <=
		      FAST_SOFT_MAX_BLOCK_LENGTH + MIN_BLOCK_LENGTH);

	/* Verify that the maximum codeword lengths are valid. */
	STATIC_ASSERT(
		MAX_LITLEN_CODEWORD_LEN <= DEFLATE_MAX_LITLEN_CODEWORD_LEN);
	STATIC_ASSERT(
		MAX_OFFSET_CODEWORD_LEN <= DEFLATE_MAX_OFFSET_CODEWORD_LEN);
	STATIC_ASSERT(
		MAX_PRE_CODEWORD_LEN <= DEFLATE_MAX_PRE_CODEWORD_LEN);
	STATIC_ASSERT(
		(1U << MAX_LITLEN_CODEWORD_LEN) >= DEFLATE_NUM_LITLEN_SYMS);
	STATIC_ASSERT(
		(1U << MAX_OFFSET_CODEWORD_LEN) >= DEFLATE_NUM_OFFSET_SYMS);
	STATIC_ASSERT(
		(1U << MAX_PRE_CODEWORD_LEN) >= DEFLATE_NUM_PRECODE_SYMS);
}

/******************************************************************************/

/* Table: length slot => length slot base value */
static const unsigned deflate_length_slot_base[] = {
	3,    4,    5,    6,    7,    8,    9,    10,
	11,   13,   15,   17,   19,   23,   27,   31,
	35,   43,   51,   59,   67,   83,   99,   115,
	131,  163,  195,  227,  258,
};

/* Table: length slot => number of extra length bits */
static const u8 deflate_extra_length_bits[] = {
	0,    0,    0,    0,    0,    0,    0,    0,
	1,    1,    1,    1,    2,    2,    2,    2,
	3,    3,    3,    3,    4,    4,    4,    4,
	5,    5,    5,    5,    0,
};

/* Table: offset slot => offset slot base value */
static const unsigned deflate_offset_slot_base[] = {
	1,     2,     3,     4,     5,     7,     9,     13,
	17,    25,    33,    49,    65,    97,    129,   193,
	257,   385,   513,   769,   1025,  1537,  2049,  3073,
	4097,  6145,  8193,  12289, 16385, 24577,
};

/* Table: offset slot => number of extra offset bits */
static const u8 deflate_extra_offset_bits[] = {
	0,     0,     0,     0,     1,     1,     2,     2,
	3,     3,     4,     4,     5,     5,     6,     6,
	7,     7,     8,     8,     9,     9,     10,    10,
	11,    11,    12,    12,    13,    13,
};

/* Table: length => length slot */
static const u8 deflate_length_slot[DEFLATE_MAX_MATCH_LEN + 1] = {
	0, 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 12,
	12, 13, 13, 13, 13, 14, 14, 14, 14, 15, 15, 15, 15, 16, 16, 16, 16, 16,
	16, 16, 16, 17, 17, 17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18,
	18, 19, 19, 19, 19, 19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 20,
	20, 20, 20, 20, 20, 20, 20, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
	21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22, 22,
	22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23, 23,
	23, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
	24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 25, 25, 25,
	25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
	25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
	27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
	27, 27, 28,
};

/*
 * A condensed table which maps offset => offset slot as follows:
 *
 *	offset <= 256: deflate_offset_slot[offset]
 *	offset > 256: deflate_offset_slot[256 + ((offset - 1) >> 7)]
 *
 * This table was generated by scripts/gen_offset_slot_map.py.
 */
static const u8 deflate_offset_slot[512] = {
	0, 0, 1, 2, 3, 4, 4, 5, 5, 6, 6, 6, 6, 7, 7, 7,
	7, 8, 8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 9,
	9, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10, 10,
	10, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
	11, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
	12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12, 12,
	12, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
	13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13, 13,
	13, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
	14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
	14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
	14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14, 14,
	14, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
	15, 0, 16, 17, 18, 18, 19, 19, 20, 20, 20, 20, 21, 21, 21, 21,
	22, 22, 22, 22, 22, 22, 22, 22, 23, 23, 23, 23, 23, 23, 23, 23,
	24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24, 24,
	25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 25,
	26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
	26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26, 26,
	27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
	27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28, 28,
	29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
	29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29, 29,
};

/* The order in which precode codeword lengths are stored */
static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
	16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
};

/* Codewords for the DEFLATE Huffman codes */
struct deflate_codewords {
	u32 litlen[DEFLATE_NUM_LITLEN_SYMS];
	u32 offset[DEFLATE_NUM_OFFSET_SYMS];
};

/*
 * Codeword lengths (in bits) for the DEFLATE Huffman codes.
 * A zero length means the corresponding symbol had zero frequency.
 */
struct deflate_lens {
	u8 litlen[DEFLATE_NUM_LITLEN_SYMS];
	u8 offset[DEFLATE_NUM_OFFSET_SYMS];
};

/* Codewords and lengths for the DEFLATE Huffman codes */
struct deflate_codes {
	struct deflate_codewords codewords;
	struct deflate_lens lens;
};

/* Symbol frequency counters for the DEFLATE Huffman codes */
struct deflate_freqs {
	u32 litlen[DEFLATE_NUM_LITLEN_SYMS];
	u32 offset[DEFLATE_NUM_OFFSET_SYMS];
};

/*
 * Represents a run of literals followed by a match or end-of-block.  This
 * struct is needed to temporarily store items chosen by the parser, since items
 * cannot be written until all items for the block have been chosen and the
 * block's Huffman codes have been computed.
 */
struct deflate_sequence {

	/*
	 * Bits 0..22: the number of literals in this run.  This may be 0 and
	 * can be at most MAX_BLOCK_LENGTH.  The literals are not stored
	 * explicitly in this structure; instead, they are read directly from
	 * the uncompressed data.
	 *
	 * Bits 23..31: the length of the match which follows the literals, or 0
	 * if this literal run was the last in the block, so there is no match
	 * which follows it.
	 */
#define SEQ_LENGTH_SHIFT 23
#define SEQ_LITRUNLEN_MASK (((u32)1 << SEQ_LENGTH_SHIFT) - 1)
	u32 litrunlen_and_length;

	/*
	 * If 'length' doesn't indicate end-of-block, then this is the offset of
	 * the match which follows the literals.
	 */
	u16 offset;

	/*
	 * If 'length' doesn't indicate end-of-block, then this is the offset
	 * symbol of the match which follows the literals.
	 */
	u8 offset_symbol;

	/*
	 * If 'length' doesn't indicate end-of-block, then this is the length
	 * slot of the match which follows the literals.
	 */
	u8 length_slot;
};

#if SUPPORT_NEAR_OPTIMAL_PARSING

/* Costs for the near-optimal parsing algorithm */
struct deflate_costs {

	/* The cost to output each possible literal */
	u32 literal[DEFLATE_NUM_LITERALS];

	/* The cost to output each possible match length */
	u32 length[DEFLATE_MAX_MATCH_LEN + 1];

	/* The cost to output a match offset of each possible offset slot */
	u32 offset_slot[DEFLATE_NUM_OFFSET_SYMS];
};

/*
 * This structure represents a byte position in the input data and a node in the
 * graph of possible match/literal choices for the current block.
 *
 * Logically, each incoming edge to this node is labeled with a literal or a
 * match that can be taken to reach this position from an earlier position; and
 * each outgoing edge from this node is labeled with a literal or a match that
 * can be taken to advance from this position to a later position.
 *
 * But these "edges" are actually stored elsewhere (in 'match_cache').  Here we
 * associate with each node just two pieces of information:
 *
 *	'cost_to_end' is the minimum cost to reach the end of the block from
 *	this position.
 *
 *	'item' represents the literal or match that must be chosen from here to
 *	reach the end of the block with the minimum cost.  Equivalently, this
 *	can be interpreted as the label of the outgoing edge on the minimum-cost
 *	path to the "end of block" node from this node.
 */
struct deflate_optimum_node {

	u32 cost_to_end;

	/*
	 * Notes on the match/literal representation used here:
	 *
	 *	The low bits of 'item' are the length: 1 if this is a literal,
	 *	or the match length if this is a match.
	 *
	 *	The high bits of 'item' are the actual literal byte if this is a
	 *	literal, or the match offset if this is a match.
	 */
#define OPTIMUM_OFFSET_SHIFT 9
#define OPTIMUM_LEN_MASK (((u32)1 << OPTIMUM_OFFSET_SHIFT) - 1)
	u32 item;

};

#endif /* SUPPORT_NEAR_OPTIMAL_PARSING */

/* Block split statistics.  See "Block splitting algorithm" below. */
#define NUM_LITERAL_OBSERVATION_TYPES 8
#define NUM_MATCH_OBSERVATION_TYPES 2
#define NUM_OBSERVATION_TYPES (NUM_LITERAL_OBSERVATION_TYPES + \
			       NUM_MATCH_OBSERVATION_TYPES)
#define NUM_OBSERVATIONS_PER_BLOCK_CHECK 512
struct block_split_stats {
	u32 new_observations[NUM_OBSERVATION_TYPES];
	u32 observations[NUM_OBSERVATION_TYPES];
	u32 num_new_observations;
	u32 num_observations;
};

/* The main DEFLATE compressor structure */
struct libdeflate_compressor {

	/* Pointer to the compress() implementation chosen at allocation time */
	size_t (*impl)(struct libdeflate_compressor *c, const u8 *in,
		       size_t in_nbytes, u8 *out, size_t out_nbytes_avail);

	/* The compression level with which this compressor was created */
	unsigned compression_level;

	/* Anything smaller than this we won't bother trying to compress. */
	unsigned min_size_to_compress;

	/*
	 * The maximum search depth: consider at most this many potential
	 * matches at each position
	 */
	unsigned max_search_depth;

	/*
	 * The "nice" match length: if a match of this length is found, choose
	 * it immediately without further consideration
	 */
	unsigned nice_match_length;

	/* Frequency counters for the current block */
	struct deflate_freqs freqs;

	/* Block split statistics for the current block */
	struct block_split_stats split_stats;

	/* Dynamic Huffman codes for the current block */
	struct deflate_codes codes;

	/* The static Huffman codes defined by the DEFLATE format */
	struct deflate_codes static_codes;

	/* Temporary space for Huffman code output */
	u32 precode_freqs[DEFLATE_NUM_PRECODE_SYMS];
	u8 precode_lens[DEFLATE_NUM_PRECODE_SYMS];
	u32 precode_codewords[DEFLATE_NUM_PRECODE_SYMS];
	unsigned precode_items[DEFLATE_NUM_LITLEN_SYMS +
			       DEFLATE_NUM_OFFSET_SYMS];
	unsigned num_litlen_syms;
	unsigned num_offset_syms;
	unsigned num_explicit_lens;
	unsigned num_precode_items;

	union {
		/* Data for greedy or lazy parsing */
		struct {
			/* Hash chains matchfinder */
			struct hc_matchfinder hc_mf;

			/* Matches and literals chosen for the current block */
			struct deflate_sequence sequences[SEQ_STORE_LENGTH + 1];

		} g; /* (g)reedy */

		/* Data for fastest parsing */
		struct {
			/* Hash table matchfinder */
			struct ht_matchfinder ht_mf;

			/* Matches and literals chosen for the current block */
			struct deflate_sequence sequences[
						FAST_SEQ_STORE_LENGTH + 1];

		} f; /* (f)astest */

	#if SUPPORT_NEAR_OPTIMAL_PARSING
		/* Data for near-optimal parsing */
		struct {

			/* Binary tree matchfinder */
			struct bt_matchfinder bt_mf;

			/*
			 * Cached matches for the current block.  This array
			 * contains the matches that were found at each position
			 * in the block.  Specifically, for each position, there
			 * is a list of matches found at that position, if any,
			 * sorted by strictly increasing length.  In addition,
			 * following the matches for each position, there is a
			 * special 'struct lz_match' whose 'length' member
			 * contains the number of matches found at that
			 * position, and whose 'offset' member contains the
			 * literal at that position.
			 *
			 * Note: in rare cases, there will be a very high number
			 * of matches in the block and this array will overflow.
			 * If this happens, we force the end of the current
			 * block.  MATCH_CACHE_LENGTH is the length at which we
			 * actually check for overflow.  The extra slots beyond
			 * this are enough to absorb the worst case overflow,
			 * which occurs if starting at
			 * &match_cache[MATCH_CACHE_LENGTH - 1], we write
			 * MAX_MATCHES_PER_POS matches and a match count header,
			 * then skip searching for matches at
			 * 'DEFLATE_MAX_MATCH_LEN - 1' positions and write the
			 * match count header for each.
			 */
			struct lz_match match_cache[MATCH_CACHE_LENGTH +
						    MAX_MATCHES_PER_POS +
						    DEFLATE_MAX_MATCH_LEN - 1];

			/*
			 * Array of nodes, one per position, for running the
			 * minimum-cost path algorithm.
			 *
			 * This array must be large enough to accommodate the
			 * worst-case number of nodes, which is MAX_BLOCK_LENGTH
			 * plus 1 for the end-of-block node.
			 */
			struct deflate_optimum_node optimum_nodes[
				MAX_BLOCK_LENGTH + 1];

			/* The current cost model being used */
			struct deflate_costs costs;

			/*
			 * A table that maps match offset to offset slot.  This
			 * differs from deflate_offset_slot[] in that this is a
			 * full map, not a condensed one.  The full map is more
			 * appropriate for the near-optimal parser, since the
			 * near-optimal parser does more offset => offset_slot
			 * translations, it doesn't intersperse them with
			 * matchfinding (so cache evictions are less of a
			 * concern), and it uses more memory anyway.
			 */
			u8 offset_slot_full[DEFLATE_MAX_MATCH_OFFSET + 1];

			/* Literal/match statistics saved from previous block */
			u32 prev_observations[NUM_OBSERVATION_TYPES];
			u32 prev_num_observations;

			/*
			 * Approximate match length frequencies based on a
			 * greedy parse, gathered during matchfinding.  This is
			 * used for setting the initial symbol costs.
			 */
			u32 new_match_len_freqs[DEFLATE_MAX_MATCH_LEN + 1];
			u32 match_len_freqs[DEFLATE_MAX_MATCH_LEN + 1];

			unsigned num_optim_passes;
		} n; /* (n)ear-optimal */
	#endif /* SUPPORT_NEAR_OPTIMAL_PARSING */

	} p; /* (p)arser */
};

/*
 * The type for the bitbuffer variable, which temporarily holds bits that are
 * being packed into bytes and written to the output buffer.  For best
 * performance, this should have size equal to a machine word.
 */
typedef machine_word_t bitbuf_t;
#define BITBUF_NBITS	(8 * sizeof(bitbuf_t))

/*
 * Can the specified number of bits always be added to 'bitbuf' after any
 * pending bytes have been flushed?
 */
#define CAN_BUFFER(n)	((n) <= BITBUF_NBITS - 7)

/*
 * Structure to keep track of the current state of sending bits to the
 * compressed output buffer
 */
struct deflate_output_bitstream {

	/* Bits that haven't yet been written to the output buffer */
	bitbuf_t bitbuf;

	/* Number of bits currently held in @bitbuf */
	unsigned bitcount;

	/* Pointer to the beginning of the output buffer */
	u8 *begin;

	/*
	 * Pointer to the position in the output buffer at which the next byte
	 * should be written
	 */
	u8 *next;

	/* Pointer to just past the end of the output buffer */
	u8 *end;
};

/*
 * OUTPUT_END_PADDING is the size, in bytes, of the extra space that must be
 * present following os->end, in order to not overrun the buffer when generating
 * output.  When UNALIGNED_ACCESS_IS_FAST, we need at least sizeof(bitbuf_t)
 * bytes for put_unaligned_leword().  Otherwise we need only 1 byte.  However,
 * to make the compression algorithm produce the same result on all CPU
 * architectures (which is sometimes desirable), we have to unconditionally use
 * the maximum for any CPU, which is sizeof(bitbuf_t) == 8.
 */
#define OUTPUT_END_PADDING	8

/*
 * Initialize the output bitstream.  'size' must be at least OUTPUT_END_PADDING.
 */
static void
deflate_init_output(struct deflate_output_bitstream *os,
		    void *buffer, size_t size)
{
	os->bitbuf = 0;
	os->bitcount = 0;
	os->begin = buffer;
	os->next = os->begin;
	os->end = os->begin + size - OUTPUT_END_PADDING;
}

/*
 * Add some bits to the bitbuffer variable of the output bitstream.  The caller
 * must ensure that os->bitcount + num_bits <= BITBUF_NBITS, by calling
 * deflate_flush_bits() frequently enough.
 */
static forceinline void
deflate_add_bits(struct deflate_output_bitstream *os,
		 bitbuf_t bits, unsigned num_bits)
{
	os->bitbuf |= bits << os->bitcount;
	os->bitcount += num_bits;
}

/* Flush bits from the bitbuffer variable to the output buffer. */
static forceinline void
deflate_flush_bits(struct deflate_output_bitstream *os)
{
	if (UNALIGNED_ACCESS_IS_FAST) {
		/* Flush a whole word (branchlessly). */
		put_unaligned_leword(os->bitbuf, os->next);
		os->bitbuf >>= os->bitcount & ~7;
		os->next += MIN(os->end - os->next, os->bitcount >> 3);
		os->bitcount &= 7;
	} else {
		/* Flush a byte at a time. */
		while (os->bitcount >= 8) {
			*os->next = os->bitbuf;
			if (os->next != os->end)
				os->next++;
			os->bitcount -= 8;
			os->bitbuf >>= 8;
		}
	}
}

/*
 * Add bits, then flush right away.  Only use this where it is difficult to
 * batch up calls to deflate_add_bits().
 */
static forceinline void
deflate_write_bits(struct deflate_output_bitstream *os,
		   bitbuf_t bits, unsigned num_bits)
{
	deflate_add_bits(os, bits, num_bits);
	deflate_flush_bits(os);
}

/* Align the bitstream on a byte boundary. */
static forceinline void
deflate_align_bitstream(struct deflate_output_bitstream *os)
{
	os->bitcount += -os->bitcount & 7;
	deflate_flush_bits(os);
}

/*
 * Flush any remaining bits to the output buffer if needed.  Return the total
 * number of bytes that have been written to the output buffer since
 * deflate_init_output(), or 0 if an overflow occurred.
 */
static size_t
deflate_flush_output(struct deflate_output_bitstream *os)
{
	if (os->next == os->end) /* overflow? */
		return 0;

	while ((int)os->bitcount > 0) {
		*os->next++ = os->bitbuf;
		os->bitcount -= 8;
		os->bitbuf >>= 8;
	}

	return os->next - os->begin;
}

/*
 * Given the binary tree node A[subtree_idx] whose children already satisfy the
 * maxheap property, swap the node with its greater child until it is greater
 * than or equal to both of its children, so that the maxheap property is
 * satisfied in the subtree rooted at A[subtree_idx].  'A' uses 1-based indices.
 */
static void
heapify_subtree(u32 A[], unsigned length, unsigned subtree_idx)
{
	unsigned parent_idx;
	unsigned child_idx;
	u32 v;

	v = A[subtree_idx];
	parent_idx = subtree_idx;
	while ((child_idx = parent_idx * 2) <= length) {
		if (child_idx < length && A[child_idx + 1] > A[child_idx])
			child_idx++;
		if (v >= A[child_idx])
			break;
		A[parent_idx] = A[child_idx];
		parent_idx = child_idx;
	}
	A[parent_idx] = v;
}

/*
 * Rearrange the array 'A' so that it satisfies the maxheap property.
 * 'A' uses 1-based indices, so the children of A[i] are A[i*2] and A[i*2 + 1].
 */
static void
heapify_array(u32 A[], unsigned length)
{
	unsigned subtree_idx;

	for (subtree_idx = length / 2; subtree_idx >= 1; subtree_idx--)
		heapify_subtree(A, length, subtree_idx);
}

/*
 * Sort the array 'A', which contains 'length' unsigned 32-bit integers.
 *
 * Note: name this function heap_sort() instead of heapsort() to avoid colliding
 * with heapsort() from stdlib.h on BSD-derived systems --- though this isn't
 * necessary when compiling with -D_ANSI_SOURCE, which is the better solution.
 */
static void
heap_sort(u32 A[], unsigned length)
{
	A--; /* Use 1-based indices  */

	heapify_array(A, length);

	while (length >= 2) {
		u32 tmp = A[length];

		A[length] = A[1];
		A[1] = tmp;
		length--;
		heapify_subtree(A, length, 1);
	}
}

#define NUM_SYMBOL_BITS 10
#define SYMBOL_MASK ((1 << NUM_SYMBOL_BITS) - 1)

#define GET_NUM_COUNTERS(num_syms)	(num_syms)

/*
 * Sort the symbols primarily by frequency and secondarily by symbol value.
 * Discard symbols with zero frequency and fill in an array with the remaining
 * symbols, along with their frequencies.  The low NUM_SYMBOL_BITS bits of each
 * array entry will contain the symbol value, and the remaining bits will
 * contain the frequency.
 *
 * @num_syms
 *	Number of symbols in the alphabet.
 *	Can't be greater than (1 << NUM_SYMBOL_BITS).
 *
 * @freqs[num_syms]
 *	The frequency of each symbol.
 *
 * @lens[num_syms]
 *	An array that eventually will hold the length of each codeword.  This
 *	function only fills in the codeword lengths for symbols that have zero
 *	frequency, which are not well defined per se but will be set to 0.
 *
 * @symout[num_syms]
 *	The output array, described above.
 *
 * Returns the number of entries in 'symout' that were filled.  This is the
 * number of symbols that have nonzero frequency.
 */
static unsigned
sort_symbols(unsigned num_syms, const u32 freqs[restrict],
	     u8 lens[restrict], u32 symout[restrict])
{
	unsigned sym;
	unsigned i;
	unsigned num_used_syms;
	unsigned num_counters;
	unsigned counters[GET_NUM_COUNTERS(DEFLATE_MAX_NUM_SYMS)];

	/*
	 * We use heapsort, but with an added optimization.  Since often most
	 * symbol frequencies are low, we first do a count sort using a limited
	 * number of counters.  High frequencies are counted in the last
	 * counter, and only they will be sorted with heapsort.
	 *
	 * Note: with more symbols, it is generally beneficial to have more
	 * counters.  About 1 counter per symbol seems fastest.
	 */

	num_counters = GET_NUM_COUNTERS(num_syms);

	memset(counters, 0, num_counters * sizeof(counters[0]));

	/* Count the frequencies. */
	for (sym = 0; sym < num_syms; sym++)
		counters[MIN(freqs[sym], num_counters - 1)]++;

	/*
	 * Make the counters cumulative, ignoring the zero-th, which counted
	 * symbols with zero frequency.  As a side effect, this calculates the
	 * number of symbols with nonzero frequency.
	 */
	num_used_syms = 0;
	for (i = 1; i < num_counters; i++) {
		unsigned count = counters[i];

		counters[i] = num_used_syms;
		num_used_syms += count;
	}

	/*
	 * Sort nonzero-frequency symbols using the counters.  At the same time,
	 * set the codeword lengths of zero-frequency symbols to 0.
	 */
	for (sym = 0; sym < num_syms; sym++) {
		u32 freq = freqs[sym];

		if (freq != 0) {
			symout[counters[MIN(freq, num_counters - 1)]++] =
				sym | (freq << NUM_SYMBOL_BITS);
		} else {
			lens[sym] = 0;
		}
	}

	/* Sort the symbols counted in the last counter. */
	heap_sort(symout + counters[num_counters - 2],
		  counters[num_counters - 1] - counters[num_counters - 2]);

	return num_used_syms;
}

/*
 * Build a Huffman tree.
 *
 * This is an optimized implementation that
 *	(a) takes advantage of the frequencies being already sorted;
 *	(b) only generates non-leaf nodes, since the non-leaf nodes of a Huffman
 *	    tree are sufficient to generate a canonical code;
 *	(c) Only stores parent pointers, not child pointers;
 *	(d) Produces the nodes in the same memory used for input frequency
 *	    information.
 *
 * Array 'A', which contains 'sym_count' entries, is used for both input and
 * output.  For this function, 'sym_count' must be at least 2.
 *
 * For input, the array must contain the frequencies of the symbols, sorted in
 * increasing order.  Specifically, each entry must contain a frequency left
 * shifted by NUM_SYMBOL_BITS bits.  Any data in the low NUM_SYMBOL_BITS bits of
 * the entries will be ignored by this function.  Although these bits will, in
 * fact, contain the symbols that correspond to the frequencies, this function
 * is concerned with frequencies only and keeps the symbols as-is.
 *
 * For output, this function will produce the non-leaf nodes of the Huffman
 * tree.  These nodes will be stored in the first (sym_count - 1) entries of the
 * array.  Entry A[sym_count - 2] will represent the root node.  Each other node
 * will contain the zero-based index of its parent node in 'A', left shifted by
 * NUM_SYMBOL_BITS bits.  The low NUM_SYMBOL_BITS bits of each entry in A will
 * be kept as-is.  Again, note that although these low bits will, in fact,
 * contain a symbol value, this symbol will have *no relationship* with the
 * Huffman tree node that happens to occupy the same slot.  This is because this
 * implementation only generates the non-leaf nodes of the tree.
 */
static void
build_tree(u32 A[], unsigned sym_count)
{
	/*
	 * Index, in 'A', of next lowest frequency symbol that has not yet been
	 * processed.
	 */
	unsigned i = 0;

	/*
	 * Index, in 'A', of next lowest frequency parentless non-leaf node; or,
	 * if equal to 'e', then no such node exists yet.
	 */
	unsigned b = 0;

	/* Index, in 'A', of next node to allocate as a non-leaf. */
	unsigned e = 0;

	do {
		unsigned m, n;
		u32 freq_shifted;

		/* Choose the two next lowest frequency entries. */

		if (i != sym_count &&
		    (b == e ||
		     (A[i] >> NUM_SYMBOL_BITS) <= (A[b] >> NUM_SYMBOL_BITS)))
			m = i++;
		else
			m = b++;

		if (i != sym_count &&
		    (b == e ||
		     (A[i] >> NUM_SYMBOL_BITS) <= (A[b] >> NUM_SYMBOL_BITS)))
			n = i++;
		else
			n = b++;

		/*
		 * Allocate a non-leaf node and link the entries to it.
		 *
		 * If we link an entry that we're visiting for the first time
		 * (via index 'i'), then we're actually linking a leaf node and
		 * it will have no effect, since the leaf will be overwritten
		 * with a non-leaf when index 'e' catches up to it.  But it's
		 * not any slower to unconditionally set the parent index.
		 *
		 * We also compute the frequency of the non-leaf node as the sum
		 * of its two children's frequencies.
		 */

		freq_shifted = (A[m] & ~SYMBOL_MASK) + (A[n] & ~SYMBOL_MASK);

		A[m] = (A[m] & SYMBOL_MASK) | (e << NUM_SYMBOL_BITS);
		A[n] = (A[n] & SYMBOL_MASK) | (e << NUM_SYMBOL_BITS);
		A[e] = (A[e] & SYMBOL_MASK) | freq_shifted;
		e++;
	} while (sym_count - e > 1);
		/*
		 * When just one entry remains, it is a "leaf" that was linked
		 * to some other node.  We ignore it, since the rest of the
		 * array contains the non-leaves which we need.  (Note that
		 * we're assuming the cases with 0 or 1 symbols were handled
		 * separately.)
		 */
}

/*
 * Given the stripped-down Huffman tree constructed by build_tree(), determine
 * the number of codewords that should be assigned each possible length, taking
 * into account the length-limited constraint.
 *
 * @A
 *	The array produced by build_tree(), containing parent index information
 *	for the non-leaf nodes of the Huffman tree.  Each entry in this array is
 *	a node; a node's parent always has a greater index than that node
 *	itself.  This function will overwrite the parent index information in
 *	this array, so essentially it will destroy the tree.  However, the data
 *	in the low NUM_SYMBOL_BITS of each entry will be preserved.
 *
 * @root_idx
 *	The 0-based index of the root node in 'A', and consequently one less
 *	than the number of tree node entries in 'A'.  (Or, really 2 less than
 *	the actual length of 'A'.)
 *
 * @len_counts
 *	An array of length ('max_codeword_len' + 1) in which the number of
 *	codewords having each length <= max_codeword_len will be returned.
 *
 * @max_codeword_len
 *	The maximum permissible codeword length.
 */
static void
compute_length_counts(u32 A[restrict], unsigned root_idx,
		      unsigned len_counts[restrict], unsigned max_codeword_len)
{
	unsigned len;
	int node;

	/*
	 * The key observations are:
	 *
	 * (1) We can traverse the non-leaf nodes of the tree, always visiting a
	 *     parent before its children, by simply iterating through the array
	 *     in reverse order.  Consequently, we can compute the depth of each
	 *     node in one pass, overwriting the parent indices with depths.
	 *
	 * (2) We can initially assume that in the real Huffman tree, both
	 *     children of the root are leaves.  This corresponds to two
	 *     codewords of length 1.  Then, whenever we visit a (non-leaf) node
	 *     during the traversal, we modify this assumption to account for
	 *     the current node *not* being a leaf, but rather its two children
	 *     being leaves.  This causes the loss of one codeword for the
	 *     current depth and the addition of two codewords for the current
	 *     depth plus one.
	 *
	 * (3) We can handle the length-limited constraint fairly easily by
	 *     simply using the largest length available when a depth exceeds
	 *     max_codeword_len.
	 */

	for (len = 0; len <= max_codeword_len; len++)
		len_counts[len] = 0;
	len_counts[1] = 2;

	/* Set the root node's depth to 0. */
	A[root_idx] &= SYMBOL_MASK;

	for (node = root_idx - 1; node >= 0; node--) {

		/* Calculate the depth of this node. */

		unsigned parent = A[node] >> NUM_SYMBOL_BITS;
		unsigned parent_depth = A[parent] >> NUM_SYMBOL_BITS;
		unsigned depth = parent_depth + 1;
		unsigned len = depth;

		/*
		 * Set the depth of this node so that it is available when its
		 * children (if any) are processed.
		 */
		A[node] = (A[node] & SYMBOL_MASK) | (depth << NUM_SYMBOL_BITS);

		/*
		 * If needed, decrease the length to meet the length-limited
		 * constraint.  This is not the optimal method for generating
		 * length-limited Huffman codes!  But it should be good enough.
		 */
		if (len >= max_codeword_len) {
			len = max_codeword_len;
			do {
				len--;
			} while (len_counts[len] == 0);
		}

		/*
		 * Account for the fact that we have a non-leaf node at the
		 * current depth.
		 */
		len_counts[len]--;
		len_counts[len + 1] += 2;
	}
}

/* Reverse the Huffman codeword 'codeword', which is 'len' bits in length. */
static u32
reverse_codeword(u32 codeword, u8 len)
{
	/*
	 * The following branchless algorithm is faster than going bit by bit.
	 * Note: since no codewords are longer than 16 bits, we only need to
	 * reverse the low 16 bits of the 'u32'.
	 */
	STATIC_ASSERT(DEFLATE_MAX_CODEWORD_LEN <= 16);

	/* Flip adjacent 1-bit fields. */
	codeword = ((codeword & 0x5555) << 1) | ((codeword & 0xAAAA) >> 1);

	/* Flip adjacent 2-bit fields. */
	codeword = ((codeword & 0x3333) << 2) | ((codeword & 0xCCCC) >> 2);

	/* Flip adjacent 4-bit fields. */
	codeword = ((codeword & 0x0F0F) << 4) | ((codeword & 0xF0F0) >> 4);

	/* Flip adjacent 8-bit fields. */
	codeword = ((codeword & 0x00FF) << 8) | ((codeword & 0xFF00) >> 8);

	/* Return the high 'len' bits of the bit-reversed 16 bit value. */
	return codeword >> (16 - len);
}

/*
 * Generate the codewords for a canonical Huffman code.
 *
 * @A
 *	The output array for codewords.  In addition, initially this
 *	array must contain the symbols, sorted primarily by frequency and
 *	secondarily by symbol value, in the low NUM_SYMBOL_BITS bits of
 *	each entry.
 *
 * @len
 *	Output array for codeword lengths.
 *
 * @len_counts
 *	An array that provides the number of codewords that will have
 *	each possible length <= max_codeword_len.
 *
 * @max_codeword_len
 *	Maximum length, in bits, of each codeword.
 *
 * @num_syms
 *	Number of symbols in the alphabet, including symbols with zero
 *	frequency.  This is the length of the 'A' and 'len' arrays.
 */
static void
gen_codewords(u32 A[restrict], u8 lens[restrict],
	      const unsigned len_counts[restrict],
	      unsigned max_codeword_len, unsigned num_syms)
{
	u32 next_codewords[DEFLATE_MAX_CODEWORD_LEN + 1];
	unsigned i;
	unsigned len;
	unsigned sym;

	/*
	 * Given the number of codewords that will have each length, assign
	 * codeword lengths to symbols.  We do this by assigning the lengths in
	 * decreasing order to the symbols sorted primarily by increasing
	 * frequency and secondarily by increasing symbol value.
	 */
	for (i = 0, len = max_codeword_len; len >= 1; len--) {
		unsigned count = len_counts[len];

		while (count--)
			lens[A[i++] & SYMBOL_MASK] = len;
	}

	/*
	 * Generate the codewords themselves.  We initialize the
	 * 'next_codewords' array to provide the lexicographically first
	 * codeword of each length, then assign codewords in symbol order.  This
	 * produces a canonical code.
	 */
	next_codewords[0] = 0;
	next_codewords[1] = 0;
	for (len = 2; len <= max_codeword_len; len++)
		next_codewords[len] =
			(next_codewords[len - 1] + len_counts[len - 1]) << 1;

	for (sym = 0; sym < num_syms; sym++) {
		u8 len = lens[sym];
		u32 codeword = next_codewords[len]++;

		/* DEFLATE requires bit-reversed codewords. */
		A[sym] = reverse_codeword(codeword, len);
	}
}

/*
 * ---------------------------------------------------------------------
 *			deflate_make_huffman_code()
 * ---------------------------------------------------------------------
 *
 * Given an alphabet and the frequency of each symbol in it, construct a
 * length-limited canonical Huffman code.
 *
 * @num_syms
 *	The number of symbols in the alphabet.  The symbols are the integers in
 *	the range [0, num_syms - 1].  This parameter must be at least 2 and
 *	can't be greater than (1 << NUM_SYMBOL_BITS).
 *
 * @max_codeword_len
 *	The maximum permissible codeword length.
 *
 * @freqs
 *	An array of @num_syms entries, each of which specifies the frequency of
 *	the corresponding symbol.  It is valid for some, none, or all of the
 *	frequencies to be 0.
 *
 * @lens
 *	An array of @num_syms entries in which this function will return the
 *	length, in bits, of the codeword assigned to each symbol.  Symbols with
 *	0 frequency will not have codewords per se, but their entries in this
 *	array will be set to 0.  No lengths greater than @max_codeword_len will
 *	be assigned.
 *
 * @codewords
 *	An array of @num_syms entries in which this function will return the
 *	codeword for each symbol, right-justified and padded on the left with
 *	zeroes.  Codewords for symbols with 0 frequency will be undefined.
 *
 * ---------------------------------------------------------------------
 *
 * This function builds a length-limited canonical Huffman code.
 *
 * A length-limited Huffman code contains no codewords longer than some
 * specified length, and has exactly (with some algorithms) or approximately
 * (with the algorithm used here) the minimum weighted path length from the
 * root, given this constraint.
 *
 * A canonical Huffman code satisfies the properties that a longer codeword
 * never lexicographically precedes a shorter codeword, and the lexicographic
 * ordering of codewords of the same length is the same as the lexicographic
 * ordering of the corresponding symbols.  A canonical Huffman code, or more
 * generally a canonical prefix code, can be reconstructed from only a list
 * containing the codeword length of each symbol.
 *
 * The classic algorithm to generate a Huffman code creates a node for each
 * symbol, then inserts these nodes into a min-heap keyed by symbol frequency.
 * Then, repeatedly, the two lowest-frequency nodes are removed from the
 * min-heap and added as the children of a new node having frequency equal to
 * the sum of its two children, which is then inserted into the min-heap.  When
 * only a single node remains in the min-heap, it is the root of the Huffman
 * tree.  The codeword for each symbol is determined by the path needed to reach
 * the corresponding node from the root.  Descending to the left child appends a
 * 0 bit, whereas descending to the right child appends a 1 bit.
 *
 * The classic algorithm is relatively easy to understand, but it is subject to
 * a number of inefficiencies.  In practice, it is fastest to first sort the
 * symbols by frequency.  (This itself can be subject to an optimization based
 * on the fact that most frequencies tend to be low.)  At the same time, we sort
 * secondarily by symbol value, which aids the process of generating a canonical
 * code.  Then, during tree construction, no heap is necessary because both the
 * leaf nodes and the unparented non-leaf nodes can be easily maintained in
 * sorted order.  Consequently, there can never be more than two possibilities
 * for the next-lowest-frequency node.
 *
 * In addition, because we're generating a canonical code, we actually don't
 * need the leaf nodes of the tree at all, only the non-leaf nodes.  This is
 * because for canonical code generation we don't need to know where the symbols
 * are in the tree.  Rather, we only need to know how many leaf nodes have each
 * depth (codeword length).  And this information can, in fact, be quickly
 * generated from the tree of non-leaves only.
 *
 * Furthermore, we can build this stripped-down Huffman tree directly in the
 * array in which the codewords are to be generated, provided that these array
 * slots are large enough to hold a symbol and frequency value.
 *
 * Still furthermore, we don't even need to maintain explicit child pointers.
 * We only need the parent pointers, and even those can be overwritten in-place
 * with depth information as part of the process of extracting codeword lengths
 * from the tree.  So in summary, we do NOT need a big structure like:
 *
 *	struct huffman_tree_node {
 *		unsigned int symbol;
 *		unsigned int frequency;
 *		unsigned int depth;
 *		struct huffman_tree_node *left_child;
 *		struct huffman_tree_node *right_child;
 *	};
 *
 *
 * ... which often gets used in "naive" implementations of Huffman code
 * generation.
 *
 * Many of these optimizations are based on the implementation in 7-Zip (source
 * file: C/HuffEnc.c), which was placed in the public domain by Igor Pavlov.
 */
static void
deflate_make_huffman_code(unsigned num_syms, unsigned max_codeword_len,
			  const u32 freqs[restrict],
			  u8 lens[restrict], u32 codewords[restrict])
{
	u32 *A = codewords;
	unsigned num_used_syms;

	STATIC_ASSERT(DEFLATE_MAX_NUM_SYMS <= 1 << NUM_SYMBOL_BITS);

	/*
	 * We begin by sorting the symbols primarily by frequency and
	 * secondarily by symbol value.  As an optimization, the array used for
	 * this purpose ('A') shares storage with the space in which we will
	 * eventually return the codewords.
	 */
	num_used_syms = sort_symbols(num_syms, freqs, lens, A);

	/*
	 * 'num_used_syms' is the number of symbols with nonzero frequency.
	 * This may be less than @num_syms.  'num_used_syms' is also the number
	 * of entries in 'A' that are valid.  Each entry consists of a distinct
	 * symbol and a nonzero frequency packed into a 32-bit integer.
	 */

	/*
	 * Handle special cases where only 0 or 1 symbols were used (had nonzero
	 * frequency).
	 */

	if (unlikely(num_used_syms == 0)) {
		/*
		 * Code is empty.  sort_symbols() already set all lengths to 0,
		 * so there is nothing more to do.
		 */
		return;
	}

	if (unlikely(num_used_syms == 1)) {
		/*
		 * Only one symbol was used, so we only need one codeword.  But
		 * two codewords are needed to form the smallest complete
		 * Huffman code, which uses codewords 0 and 1.  Therefore, we
		 * choose another symbol to which to assign a codeword.  We use
		 * 0 (if the used symbol is not 0) or 1 (if the used symbol is
		 * 0).  In either case, the lesser-valued symbol must be
		 * assigned codeword 0 so that the resulting code is canonical.
		 */

		unsigned sym = A[0] & SYMBOL_MASK;
		unsigned nonzero_idx = sym ? sym : 1;

		codewords[0] = 0;
		lens[0] = 1;
		codewords[nonzero_idx] = 1;
		lens[nonzero_idx] = 1;
		return;
	}

	/*
	 * Build a stripped-down version of the Huffman tree, sharing the array
	 * 'A' with the symbol values.  Then extract length counts from the tree
	 * and use them to generate the final codewords.
	 */

	build_tree(A, num_used_syms);

	{
		unsigned len_counts[DEFLATE_MAX_CODEWORD_LEN + 1];

		compute_length_counts(A, num_used_syms - 2,
				      len_counts, max_codeword_len);

		gen_codewords(A, lens, len_counts, max_codeword_len, num_syms);
	}
}

/*
 * Clear the Huffman symbol frequency counters.  This must be called when
 * starting a new DEFLATE block.
 */
static void
deflate_reset_symbol_frequencies(struct libdeflate_compressor *c)
{
	memset(&c->freqs, 0, sizeof(c->freqs));
}

/*
 * Build the literal/length and offset Huffman codes for a DEFLATE block.
 *
 * This takes as input the frequency tables for each alphabet and produces as
 * output a set of tables that map symbols to codewords and codeword lengths.
 */
static void
deflate_make_huffman_codes(const struct deflate_freqs *freqs,
			   struct deflate_codes *codes)
{
	deflate_make_huffman_code(DEFLATE_NUM_LITLEN_SYMS,
				  MAX_LITLEN_CODEWORD_LEN,
				  freqs->litlen,
				  codes->lens.litlen,
				  codes->codewords.litlen);

	deflate_make_huffman_code(DEFLATE_NUM_OFFSET_SYMS,
				  MAX_OFFSET_CODEWORD_LEN,
				  freqs->offset,
				  codes->lens.offset,
				  codes->codewords.offset);
}

/* Initialize c->static_codes. */
static void
deflate_init_static_codes(struct libdeflate_compressor *c)
{
	unsigned i;

	for (i = 0; i < 144; i++)
		c->freqs.litlen[i] = 1 << (9 - 8);
	for (; i < 256; i++)
		c->freqs.litlen[i] = 1 << (9 - 9);
	for (; i < 280; i++)
		c->freqs.litlen[i] = 1 << (9 - 7);
	for (; i < 288; i++)
		c->freqs.litlen[i] = 1 << (9 - 8);

	for (i = 0; i < 32; i++)
		c->freqs.offset[i] = 1 << (5 - 5);

	deflate_make_huffman_codes(&c->freqs, &c->static_codes);
}

/* Return the offset slot for the given match offset, using the small map. */
static forceinline unsigned
deflate_get_offset_slot(unsigned offset)
{
#if 1
	if (offset <= 256)
		return deflate_offset_slot[offset];
	else
		return deflate_offset_slot[256 + ((offset - 1) >> 7)];
#else /* Branchless version */
	u32 i1 = offset;
	u32 i2 = 256 + ((offset - 1) >> 7);
	u32 is_small = (s32)(offset - 257) >> 31;

	return deflate_offset_slot[(i1 & is_small) ^ (i2 & ~is_small)];
#endif
}

/* Write the header fields common to all DEFLATE block types. */
static void
deflate_write_block_header(struct deflate_output_bitstream *os,
			   bool is_final_block, unsigned block_type)
{
	deflate_add_bits(os, is_final_block, 1);
	deflate_add_bits(os, block_type, 2);
	deflate_flush_bits(os);
}

static unsigned
deflate_compute_precode_items(const u8 lens[restrict],
			      const unsigned num_lens,
			      u32 precode_freqs[restrict],
			      unsigned precode_items[restrict])
{
	unsigned *itemptr;
	unsigned run_start;
	unsigned run_end;
	unsigned extra_bits;
	u8 len;

	memset(precode_freqs, 0,
	       DEFLATE_NUM_PRECODE_SYMS * sizeof(precode_freqs[0]));

	itemptr = precode_items;
	run_start = 0;
	do {
		/* Find the next run of codeword lengths. */

		/* len = the length being repeated */
		len = lens[run_start];

		/* Extend the run. */
		run_end = run_start;
		do {
			run_end++;
		} while (run_end != num_lens && len == lens[run_end]);

		if (len == 0) {
			/* Run of zeroes. */

			/* Symbol 18: RLE 11 to 138 zeroes at a time. */
			while ((run_end - run_start) >= 11) {
				extra_bits = MIN((run_end - run_start) - 11,
						 0x7F);
				precode_freqs[18]++;
				*itemptr++ = 18 | (extra_bits << 5);
				run_start += 11 + extra_bits;
			}

			/* Symbol 17: RLE 3 to 10 zeroes at a time. */
			if ((run_end - run_start) >= 3) {
				extra_bits = MIN((run_end - run_start) - 3,
						 0x7);
				precode_freqs[17]++;
				*itemptr++ = 17 | (extra_bits << 5);
				run_start += 3 + extra_bits;
			}
		} else {

			/* A run of nonzero lengths. */

			/* Symbol 16: RLE 3 to 6 of the previous length. */
			if ((run_end - run_start) >= 4) {
				precode_freqs[len]++;
				*itemptr++ = len;
				run_start++;
				do {
					extra_bits = MIN((run_end - run_start) -
							 3, 0x3);
					precode_freqs[16]++;
					*itemptr++ = 16 | (extra_bits << 5);
					run_start += 3 + extra_bits;
				} while ((run_end - run_start) >= 3);
			}
		}

		/* Output any remaining lengths without RLE. */
		while (run_start != run_end) {
			precode_freqs[len]++;
			*itemptr++ = len;
			run_start++;
		}
	} while (run_start != num_lens);

	return itemptr - precode_items;
}

/*
 * Huffman codeword lengths for dynamic Huffman blocks are compressed using a
 * separate Huffman code, the "precode", which contains a symbol for each
 * possible codeword length in the larger code as well as several special
 * symbols to represent repeated codeword lengths (a form of run-length
 * encoding).  The precode is itself constructed in canonical form, and its
 * codeword lengths are represented literally in 19 3-bit fields that
 * immediately precede the compressed codeword lengths of the larger code.
 */

/* Precompute the information needed to output Huffman codes. */
static void
deflate_precompute_huffman_header(struct libdeflate_compressor *c)
{
	/* Compute how many litlen and offset symbols are needed. */

	for (c->num_litlen_syms = DEFLATE_NUM_LITLEN_SYMS;
	     c->num_litlen_syms > 257;
	     c->num_litlen_syms--)
		if (c->codes.lens.litlen[c->num_litlen_syms - 1] != 0)
			break;

	for (c->num_offset_syms = DEFLATE_NUM_OFFSET_SYMS;
	     c->num_offset_syms > 1;
	     c->num_offset_syms--)
		if (c->codes.lens.offset[c->num_offset_syms - 1] != 0)
			break;

	/*
	 * If we're not using the full set of literal/length codeword lengths,
	 * then temporarily move the offset codeword lengths over so that the
	 * literal/length and offset codeword lengths are contiguous.
	 */
	STATIC_ASSERT(offsetof(struct deflate_lens, offset) ==
		      DEFLATE_NUM_LITLEN_SYMS);
	if (c->num_litlen_syms != DEFLATE_NUM_LITLEN_SYMS) {
		memmove((u8 *)&c->codes.lens + c->num_litlen_syms,
			(u8 *)&c->codes.lens + DEFLATE_NUM_LITLEN_SYMS,
			c->num_offset_syms);
	}

	/*
	 * Compute the "items" (RLE / literal tokens and extra bits) with which
	 * the codeword lengths in the larger code will be output.
	 */
	c->num_precode_items =
		deflate_compute_precode_items((u8 *)&c->codes.lens,
					      c->num_litlen_syms +
							c->num_offset_syms,
					      c->precode_freqs,
					      c->precode_items);

	/* Build the precode. */
	deflate_make_huffman_code(DEFLATE_NUM_PRECODE_SYMS,
				  MAX_PRE_CODEWORD_LEN,
				  c->precode_freqs, c->precode_lens,
				  c->precode_codewords);

	/* Count how many precode lengths we actually need to output. */
	for (c->num_explicit_lens = DEFLATE_NUM_PRECODE_SYMS;
	     c->num_explicit_lens > 4;
	     c->num_explicit_lens--)
		if (c->precode_lens[deflate_precode_lens_permutation[
						c->num_explicit_lens - 1]] != 0)
			break;

	/* Restore the offset codeword lengths if needed. */
	if (c->num_litlen_syms != DEFLATE_NUM_LITLEN_SYMS) {
		memmove((u8 *)&c->codes.lens + DEFLATE_NUM_LITLEN_SYMS,
			(u8 *)&c->codes.lens + c->num_litlen_syms,
			c->num_offset_syms);
	}
}

/* Output the Huffman codes. */
static void
deflate_write_huffman_header(struct libdeflate_compressor *c,
			     struct deflate_output_bitstream *os)
{
	unsigned i;

	deflate_add_bits(os, c->num_litlen_syms - 257, 5);
	deflate_add_bits(os, c->num_offset_syms - 1, 5);
	deflate_add_bits(os, c->num_explicit_lens - 4, 4);
	deflate_flush_bits(os);

	/* Output the lengths of the codewords in the precode. */
	for (i = 0; i < c->num_explicit_lens; i++) {
		deflate_write_bits(os, c->precode_lens[
				       deflate_precode_lens_permutation[i]], 3);
	}

	/* Output the encoded lengths of the codewords in the larger code. */
	for (i = 0; i < c->num_precode_items; i++) {
		unsigned precode_item = c->precode_items[i];
		unsigned precode_sym = precode_item & 0x1F;

		deflate_add_bits(os, c->precode_codewords[precode_sym],
				 c->precode_lens[precode_sym]);
		if (precode_sym >= 16) {
			if (precode_sym == 16)
				deflate_add_bits(os, precode_item >> 5, 2);
			else if (precode_sym == 17)
				deflate_add_bits(os, precode_item >> 5, 3);
			else
				deflate_add_bits(os, precode_item >> 5, 7);
		}
		STATIC_ASSERT(CAN_BUFFER(MAX_PRE_CODEWORD_LEN + 7));
		deflate_flush_bits(os);
	}
}

static forceinline void
deflate_write_literal_run(struct deflate_output_bitstream *os,
			  const u8 *in_next, u32 litrunlen,
			  const struct deflate_codes *codes)
{
#if 1
	while (litrunlen >= 4) {
		unsigned lit0 = in_next[0];
		unsigned lit1 = in_next[1];
		unsigned lit2 = in_next[2];
		unsigned lit3 = in_next[3];

		deflate_add_bits(os, codes->codewords.litlen[lit0],
				 codes->lens.litlen[lit0]);
		if (!CAN_BUFFER(2 * MAX_LITLEN_CODEWORD_LEN))
			deflate_flush_bits(os);

		deflate_add_bits(os, codes->codewords.litlen[lit1],
				 codes->lens.litlen[lit1]);
		if (!CAN_BUFFER(4 * MAX_LITLEN_CODEWORD_LEN))
			deflate_flush_bits(os);

		deflate_add_bits(os, codes->codewords.litlen[lit2],
				 codes->lens.litlen[lit2]);
		if (!CAN_BUFFER(2 * MAX_LITLEN_CODEWORD_LEN))
			deflate_flush_bits(os);

		deflate_add_bits(os, codes->codewords.litlen[lit3],
				 codes->lens.litlen[lit3]);
		deflate_flush_bits(os);
		in_next += 4;
		litrunlen -= 4;
	}
	if (litrunlen-- != 0) {
		deflate_add_bits(os, codes->codewords.litlen[*in_next],
				 codes->lens.litlen[*in_next]);
		if (!CAN_BUFFER(3 * MAX_LITLEN_CODEWORD_LEN))
			deflate_flush_bits(os);
		in_next++;
		if (litrunlen-- != 0) {
			deflate_add_bits(os, codes->codewords.litlen[*in_next],
					 codes->lens.litlen[*in_next]);
			if (!CAN_BUFFER(3 * MAX_LITLEN_CODEWORD_LEN))
				deflate_flush_bits(os);
			in_next++;
			if (litrunlen-- != 0) {
				deflate_add_bits(os,
					codes->codewords.litlen[*in_next],
					codes->lens.litlen[*in_next]);
				if (!CAN_BUFFER(3 * MAX_LITLEN_CODEWORD_LEN))
					deflate_flush_bits(os);
				in_next++;
			}
		}
		if (CAN_BUFFER(3 * MAX_LITLEN_CODEWORD_LEN))
			deflate_flush_bits(os);
	}
#else
	do {
		unsigned lit = *in_next++;

		deflate_write_bits(os, codes->codewords.litlen[lit],
				   codes->lens.litlen[lit]);
	} while (--litrunlen);
#endif
}

static forceinline void
deflate_write_match(struct deflate_output_bitstream * restrict os,
		    unsigned length, unsigned length_slot,
		    unsigned offset, unsigned offset_symbol,
		    const struct deflate_codes * restrict codes)
{
	unsigned litlen_symbol = DEFLATE_FIRST_LEN_SYM + length_slot;

	/* Litlen symbol */
	deflate_add_bits(os, codes->codewords.litlen[litlen_symbol],
			 codes->lens.litlen[litlen_symbol]);

	/* Extra length bits */
	STATIC_ASSERT(CAN_BUFFER(MAX_LITLEN_CODEWORD_LEN +
				 DEFLATE_MAX_EXTRA_LENGTH_BITS));
	deflate_add_bits(os, length - deflate_length_slot_base[length_slot],
			 deflate_extra_length_bits[length_slot]);

	if (!CAN_BUFFER(MAX_LITLEN_CODEWORD_LEN +
			DEFLATE_MAX_EXTRA_LENGTH_BITS +
			MAX_OFFSET_CODEWORD_LEN +
			DEFLATE_MAX_EXTRA_OFFSET_BITS))
		deflate_flush_bits(os);

	/* Offset symbol */
	deflate_add_bits(os, codes->codewords.offset[offset_symbol],
			 codes->lens.offset[offset_symbol]);

	if (!CAN_BUFFER(MAX_OFFSET_CODEWORD_LEN +
			DEFLATE_MAX_EXTRA_OFFSET_BITS))
		deflate_flush_bits(os);

	/* Extra offset bits */
	deflate_add_bits(os, offset - deflate_offset_slot_base[offset_symbol],
			 deflate_extra_offset_bits[offset_symbol]);

	deflate_flush_bits(os);
}

static void
deflate_write_sequences(struct deflate_output_bitstream * restrict os,
			const struct deflate_codes * restrict codes,
			const struct deflate_sequence sequences[restrict],
			const u8 * restrict in_next)
{
	const struct deflate_sequence *seq = sequences;

	for (;;) {
		u32 litrunlen = seq->litrunlen_and_length & SEQ_LITRUNLEN_MASK;
		unsigned length = seq->litrunlen_and_length >> SEQ_LENGTH_SHIFT;

		if (litrunlen) {
			deflate_write_literal_run(os, in_next, litrunlen,
						  codes);
			in_next += litrunlen;
		}

		if (length == 0)
			return;

		deflate_write_match(os, length, seq->length_slot,
				    seq->offset, seq->offset_symbol, codes);

		in_next += length;
		seq++;
	}
}

#if SUPPORT_NEAR_OPTIMAL_PARSING
/*
 * Follow the minimum-cost path in the graph of possible match/literal choices
 * for the current block and write out the matches/literals using the specified
 * Huffman codes.
 */
static void
deflate_write_item_list(struct deflate_output_bitstream *os,
			const struct deflate_codes *codes,
			struct libdeflate_compressor *c,
			u32 block_length)
{
	struct deflate_optimum_node *cur_node = &c->p.n.optimum_nodes[0];
	struct deflate_optimum_node * const end_node =
		&c->p.n.optimum_nodes[block_length];
	do {
		unsigned length = cur_node->item & OPTIMUM_LEN_MASK;
		unsigned offset = cur_node->item >> OPTIMUM_OFFSET_SHIFT;

		if (length == 1) {
			/* Literal */
			deflate_write_bits(os, codes->codewords.litlen[offset],
					   codes->lens.litlen[offset]);
		} else {
			/* Match */
			deflate_write_match(os, length,
					    deflate_length_slot[length],
					    offset,
					    c->p.n.offset_slot_full[offset],
					    codes);
		}
		cur_node += length;
	} while (cur_node != end_node);
}
#endif /* SUPPORT_NEAR_OPTIMAL_PARSING */

/* Output the end-of-block symbol. */
static void
deflate_write_end_of_block(struct deflate_output_bitstream *os,
			   const struct deflate_codes *codes)
{
	deflate_write_bits(os, codes->codewords.litlen[DEFLATE_END_OF_BLOCK],
			   codes->lens.litlen[DEFLATE_END_OF_BLOCK]);
}

static void
deflate_write_uncompressed_block(struct deflate_output_bitstream *os,
				 const u8 *data, u16 len,
				 bool is_final_block)
{
	deflate_write_block_header(os, is_final_block,
				   DEFLATE_BLOCKTYPE_UNCOMPRESSED);
	deflate_align_bitstream(os);

	if (4 + (u32)len >= os->end - os->next) {
		os->next = os->end;
		return;
	}

	put_unaligned_le16(len, os->next);
	os->next += 2;
	put_unaligned_le16(~len, os->next);
	os->next += 2;
	memcpy(os->next, data, len);
	os->next += len;
}

static void
deflate_write_uncompressed_blocks(struct deflate_output_bitstream *os,
				  const u8 *data, size_t data_length,
				  bool is_final_block)
{
	do {
		u16 len = MIN(data_length, UINT16_MAX);

		deflate_write_uncompressed_block(os, data, len,
					is_final_block && len == data_length);
		data += len;
		data_length -= len;
	} while (data_length != 0);
}

/*
 * Choose the best type of block to use (dynamic Huffman, static Huffman, or
 * uncompressed), then output it.
 */
static void
deflate_flush_block(struct libdeflate_compressor * restrict c,
		    struct deflate_output_bitstream * restrict os,
		    const u8 * restrict block_begin, u32 block_length,
		    const struct deflate_sequence *sequences,
		    bool is_final_block)
{
	static const u8 deflate_extra_precode_bits[DEFLATE_NUM_PRECODE_SYMS] = {
		0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 3, 7,
	};

	/* Costs are measured in bits */
	u32 dynamic_cost = 0;
	u32 static_cost = 0;
	u32 uncompressed_cost = 0;
	struct deflate_codes *codes;
	int block_type;
	unsigned sym;

	if (sequences != NULL /* !near_optimal */ ||
	    !SUPPORT_NEAR_OPTIMAL_PARSING) {
		/* Tally the end-of-block symbol. */
		c->freqs.litlen[DEFLATE_END_OF_BLOCK]++;

		/* Build dynamic Huffman codes. */
		deflate_make_huffman_codes(&c->freqs, &c->codes);
	} /* Else, this was already done. */

	/* Account for the cost of sending dynamic Huffman codes. */
	deflate_precompute_huffman_header(c);
	dynamic_cost += 5 + 5 + 4 + (3 * c->num_explicit_lens);
	for (sym = 0; sym < DEFLATE_NUM_PRECODE_SYMS; sym++) {
		u32 extra = deflate_extra_precode_bits[sym];

		dynamic_cost += c->precode_freqs[sym] *
				(extra + c->precode_lens[sym]);
	}

	/* Account for the cost of encoding literals. */
	for (sym = 0; sym < 256; sym++) {
		dynamic_cost += c->freqs.litlen[sym] *
				c->codes.lens.litlen[sym];
	}
	for (sym = 0; sym < 144; sym++)
		static_cost += c->freqs.litlen[sym] * 8;
	for (; sym < 256; sym++)
		static_cost += c->freqs.litlen[sym] * 9;

	/* Account for the cost of encoding the end-of-block symbol. */
	dynamic_cost += c->codes.lens.litlen[DEFLATE_END_OF_BLOCK];
	static_cost += 7;

	/* Account for the cost of encoding lengths. */
	for (sym = DEFLATE_FIRST_LEN_SYM;
	     sym < DEFLATE_FIRST_LEN_SYM + ARRAY_LEN(deflate_extra_length_bits);
	     sym++) {
		u32 extra = deflate_extra_length_bits[
					sym - DEFLATE_FIRST_LEN_SYM];

		dynamic_cost += c->freqs.litlen[sym] *
				(extra + c->codes.lens.litlen[sym]);
		static_cost += c->freqs.litlen[sym] *
				(extra + c->static_codes.lens.litlen[sym]);
	}

	/* Account for the cost of encoding offsets. */
	for (sym = 0; sym < ARRAY_LEN(deflate_extra_offset_bits); sym++) {
		u32 extra = deflate_extra_offset_bits[sym];

		dynamic_cost += c->freqs.offset[sym] *
				(extra + c->codes.lens.offset[sym]);
		static_cost += c->freqs.offset[sym] * (extra + 5);
	}

	/* Compute the cost of using uncompressed blocks. */
	uncompressed_cost += (-(os->bitcount + 3) & 7) + 32 +
			     (40 * (DIV_ROUND_UP(block_length,
						 UINT16_MAX) - 1)) +
			     (8 * block_length);

	/* Choose the cheapest block type. */
	if (dynamic_cost < MIN(static_cost, uncompressed_cost)) {
		block_type = DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN;
		codes = &c->codes;
	} else if (static_cost < uncompressed_cost) {
		block_type = DEFLATE_BLOCKTYPE_STATIC_HUFFMAN;
		codes = &c->static_codes;
	} else {
		block_type = DEFLATE_BLOCKTYPE_UNCOMPRESSED;
	}

	/* Now actually output the block. */

	if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {
		/*
		 * Note: the length being flushed may exceed the maximum length
		 * of an uncompressed block (65535 bytes).  Therefore, more than
		 * one uncompressed block might be needed.
		 */
		deflate_write_uncompressed_blocks(os, block_begin, block_length,
						  is_final_block);
	} else {
		/* Output the block header. */
		deflate_write_block_header(os, is_final_block, block_type);

		/* Output the Huffman codes (dynamic Huffman blocks only). */
		if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN)
			deflate_write_huffman_header(c, os);

		/* Output the literals, matches, and end-of-block symbol. */
	#if SUPPORT_NEAR_OPTIMAL_PARSING
		if (sequences == NULL)
			deflate_write_item_list(os, codes, c, block_length);
		else
	#endif
			deflate_write_sequences(os, codes, sequences,
						block_begin);
		deflate_write_end_of_block(os, codes);
	}
}

/******************************************************************************/

/*
 * Block splitting algorithm.  The problem is to decide when it is worthwhile to
 * start a new block with new Huffman codes.  There is a theoretically optimal
 * solution: recursively consider every possible block split, considering the
 * exact cost of each block, and choose the minimum cost approach.  But this is
 * far too slow.  Instead, as an approximation, we can count symbols and after
 * every N symbols, compare the expected distribution of symbols based on the
 * previous data with the actual distribution.  If they differ "by enough", then
 * start a new block.
 *
 * As an optimization and heuristic, we don't distinguish between every symbol
 * but rather we combine many symbols into a single "observation type".  For
 * literals we only look at the high bits and low bits, and for matches we only
 * look at whether the match is long or not.  The assumption is that for typical
 * "real" data, places that are good block boundaries will tend to be noticeable
 * based only on changes in these aggregate probabilities, without looking for
 * subtle differences in individual symbols.  For example, a change from ASCII
 * bytes to non-ASCII bytes, or from few matches (generally less compressible)
 * to many matches (generally more compressible), would be easily noticed based
 * on the aggregates.
 *
 * For determining whether the probability distributions are "different enough"
 * to start a new block, the simple heuristic of splitting when the sum of
 * absolute differences exceeds a constant seems to be good enough.  We also add
 * a number proportional to the block length so that the algorithm is more
 * likely to end long blocks than short blocks.  This reflects the general
 * expectation that it will become increasingly beneficial to start a new block
 * as the current block grows longer.
 *
 * Finally, for an approximation, it is not strictly necessary that the exact
 * symbols being used are considered.  With "near-optimal parsing", for example,
 * the actual symbols that will be used are unknown until after the block
 * boundary is chosen and the block has been optimized.  Since the final choices
 * cannot be used, we can use preliminary "greedy" choices instead.
 */

/* Initialize the block split statistics when starting a new block. */
static void
init_block_split_stats(struct block_split_stats *stats)
{
	int i;

	for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
		stats->new_observations[i] = 0;
		stats->observations[i] = 0;
	}
	stats->num_new_observations = 0;
	stats->num_observations = 0;
}

/*
 * Literal observation.  Heuristic: use the top 2 bits and low 1 bits of the
 * literal, for 8 possible literal observation types.
 */
static forceinline void
observe_literal(struct block_split_stats *stats, u8 lit)
{
	stats->new_observations[((lit >> 5) & 0x6) | (lit & 1)]++;
	stats->num_new_observations++;
}

/*
 * Match observation.  Heuristic: use one observation type for "short match" and
 * one observation type for "long match".
 */
static forceinline void
observe_match(struct block_split_stats *stats, unsigned length)
{
	stats->new_observations[NUM_LITERAL_OBSERVATION_TYPES +
				(length >= 9)]++;
	stats->num_new_observations++;
}

static void
merge_new_observations(struct block_split_stats *stats)
{
	int i;

	for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
		stats->observations[i] += stats->new_observations[i];
		stats->new_observations[i] = 0;
	}
	stats->num_observations += stats->num_new_observations;
	stats->num_new_observations = 0;
}

static bool
do_end_block_check(struct block_split_stats *stats, u32 block_length)
{
	if (stats->num_observations > 0) {
		/*
		 * Compute the sum of absolute differences of probabilities.  To
		 * avoid needing to use floating point arithmetic or do slow
		 * divisions, we do all arithmetic with the probabilities
		 * multiplied by num_observations * num_new_observations.  E.g.,
		 * for the "old" observations the probabilities would be
		 * (double)observations[i] / num_observations, but since we
		 * multiply by both num_observations and num_new_observations we
		 * really do observations[i] * num_new_observations.
		 */
		u32 total_delta = 0;
		u32 num_items;
		u32 cutoff;
		int i;

		for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
			u32 expected = stats->observations[i] *
				       stats->num_new_observations;
			u32 actual = stats->new_observations[i] *
				     stats->num_observations;
			u32 delta = (actual > expected) ? actual - expected :
							  expected - actual;

			total_delta += delta;
		}

		num_items = stats->num_observations +
			    stats->num_new_observations;
		/*
		 * Heuristic: the cutoff is when the sum of absolute differences
		 * of probabilities becomes at least 200/512.  As above, the
		 * probability is multiplied by both num_new_observations and
		 * num_observations.  Be careful to avoid integer overflow.
		 */
		cutoff = stats->num_new_observations * 200 / 512 *
			 stats->num_observations;
		/*
		 * Very short blocks have a lot of overhead for the Huffman
		 * codes, so only use them if it clearly seems worthwhile.
		 * (This is an additional penalty, which adds to the smaller
		 * penalty below which scales more slowly.)
		 */
		if (block_length < 10000 && num_items < 8192)
			cutoff += (u64)cutoff * (8192 - num_items) / 8192;

		/* Ready to end the block? */
		if (total_delta +
		    (block_length / 4096) * stats->num_observations >= cutoff)
			return true;
	}
	merge_new_observations(stats);
	return false;
}

static forceinline bool
ready_to_check_block(const struct block_split_stats *stats,
		     const u8 *in_block_begin, const u8 *in_next,
		     const u8 *in_end)
{
	return stats->num_new_observations >= NUM_OBSERVATIONS_PER_BLOCK_CHECK
		&& in_next - in_block_begin >= MIN_BLOCK_LENGTH
		&& in_end - in_next >= MIN_BLOCK_LENGTH;
}

static forceinline bool
should_end_block(struct block_split_stats *stats,
		 const u8 *in_block_begin, const u8 *in_next, const u8 *in_end)
{
	/* Ready to try to end the block (again)? */
	if (!ready_to_check_block(stats, in_block_begin, in_next, in_end))
		return false;

	return do_end_block_check(stats, in_next - in_block_begin);
}

/******************************************************************************/

static void
deflate_begin_sequences(struct libdeflate_compressor *c,
			struct deflate_sequence *first_seq)
{
	deflate_reset_symbol_frequencies(c);
	first_seq->litrunlen_and_length = 0;
}

static forceinline void
deflate_choose_literal(struct libdeflate_compressor *c, unsigned literal,
		       bool gather_split_stats, struct deflate_sequence *seq)
{
	c->freqs.litlen[literal]++;

	if (gather_split_stats)
		observe_literal(&c->split_stats, literal);

	STATIC_ASSERT(MAX_BLOCK_LENGTH <= SEQ_LITRUNLEN_MASK);
	seq->litrunlen_and_length++;
}

static forceinline void
deflate_choose_match(struct libdeflate_compressor *c,
		     unsigned length, unsigned offset, bool gather_split_stats,
		     struct deflate_sequence **seq_p)
{
	struct deflate_sequence *seq = *seq_p;
	unsigned length_slot = deflate_length_slot[length];
	unsigned offset_slot = deflate_get_offset_slot(offset);

	c->freqs.litlen[DEFLATE_FIRST_LEN_SYM + length_slot]++;
	c->freqs.offset[offset_slot]++;
	if (gather_split_stats)
		observe_match(&c->split_stats, length);

	seq->litrunlen_and_length |= (u32)length << SEQ_LENGTH_SHIFT;
	seq->offset = offset;
	seq->length_slot = length_slot;
	seq->offset_symbol = offset_slot;

	seq++;
	seq->litrunlen_and_length = 0;
	*seq_p = seq;
}

/*
 * Decrease the maximum and nice match lengths if we're approaching the end of
 * the input buffer.
 */
static forceinline void
adjust_max_and_nice_len(unsigned *max_len, unsigned *nice_len, size_t remaining)
{
	if (unlikely(remaining < DEFLATE_MAX_MATCH_LEN)) {
		*max_len = remaining;
		*nice_len = MIN(*nice_len, *max_len);
	}
}

/*
 * Choose the minimum match length for the greedy and lazy parsers.
 *
 * By default the minimum match length is 3, which is the smallest length the
 * DEFLATE format allows.  However, with greedy and lazy parsing, some data
 * (e.g. DNA sequencing data) benefits greatly from a longer minimum length.
 * Typically, this is because literals are very cheap.  In general, the
 * near-optimal parser handles this case naturally, but the greedy and lazy
 * parsers need a heuristic to decide when to use short matches.
 *
 * The heuristic we use is to make the minimum match length depend on the number
 * of different literals that exist in the data.  If there are many different
 * literals, then literals will probably be expensive, so short matches will
 * probably be worthwhile.  Conversely, if not many literals are used, then
 * probably literals will be cheap and short matches won't be worthwhile.
 */
static unsigned
choose_min_match_len(unsigned num_used_literals, unsigned max_search_depth)
{
	/* map from num_used_literals to min_len */
	static const u8 min_lens[] = {
		9, 9, 9, 9, 9, 9, 8, 8, 7, 7, 6, 6, 6, 6, 6, 6,
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5,
		5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 5, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
		4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4,
		/* The rest is implicitly 3. */
	};
	unsigned min_len;

	STATIC_ASSERT(DEFLATE_MIN_MATCH_LEN <= 3);
	STATIC_ASSERT(ARRAY_LEN(min_lens) <= DEFLATE_NUM_LITERALS + 1);

	if (num_used_literals >= ARRAY_LEN(min_lens))
		return 3;
	min_len = min_lens[num_used_literals];
	/*
	 * With a low max_search_depth, it may be too hard to find long matches.
	 */
	if (max_search_depth < 16) {
		if (max_search_depth < 5)
			min_len = MIN(min_len, 4);
		else if (max_search_depth < 10)
			min_len = MIN(min_len, 5);
		else
			min_len = MIN(min_len, 7);
	}
	return min_len;
}

static unsigned
calculate_min_match_len(const u8 *data, size_t data_len,
			unsigned max_search_depth)
{
	u8 used[256] = { 0 };
	unsigned num_used_literals = 0;
	size_t i;

	/*
	 * For an initial approximation, scan the first 4 KiB of data.  The
	 * caller may use recalculate_min_match_len() to update min_len later.
	 */
	data_len = MIN(data_len, 4096);
	for (i = 0; i < data_len; i++)
		used[data[i]] = 1;
	for (i = 0; i < 256; i++)
		num_used_literals += used[i];
	return choose_min_match_len(num_used_literals, max_search_depth);
}

/*
 * Recalculate the minimum match length for a block, now that we know the
 * distribution of literals that are actually being used (freqs->litlen).
 */
static unsigned
recalculate_min_match_len(const struct deflate_freqs *freqs,
			  unsigned max_search_depth)
{
	u32 literal_freq = 0;
	u32 cutoff;
	unsigned num_used_literals = 0;
	int i;

	for (i = 0; i < DEFLATE_NUM_LITERALS; i++)
		literal_freq += freqs->litlen[i];

	cutoff = literal_freq >> 10; /* Ignore literals used very rarely. */

	for (i = 0; i < DEFLATE_NUM_LITERALS; i++) {
		if (freqs->litlen[i] > cutoff)
			num_used_literals++;
	}
	return choose_min_match_len(num_used_literals, max_search_depth);
}

static forceinline const u8 *
choose_max_block_end(const u8 *in_block_begin, const u8 *in_end,
		     size_t soft_max_len)
{
	if (in_end - in_block_begin < soft_max_len + MIN_BLOCK_LENGTH)
		return in_end;
	return in_block_begin + soft_max_len;
}

/*
 * This is the level 0 "compressor".  It always outputs uncompressed blocks.
 */
static size_t
deflate_compress_none(struct libdeflate_compressor * restrict c,
		      const u8 * restrict in, size_t in_nbytes,
		      u8 * restrict out, size_t out_nbytes_avail)
{
	struct deflate_output_bitstream os;

	deflate_init_output(&os, out, out_nbytes_avail);

	deflate_write_uncompressed_blocks(&os, in, in_nbytes, true);

	return deflate_flush_output(&os);
}

/*
 * This is a faster variant of deflate_compress_greedy().  It uses the
 * ht_matchfinder rather than the hc_matchfinder.  It also skips the block
 * splitting algorithm and just uses fixed length blocks.  c->max_search_depth
 * has no effect with this algorithm, as it is hardcoded in ht_matchfinder.h.
 */
static size_t
deflate_compress_fastest(struct libdeflate_compressor * restrict c,
			 const u8 * restrict in, size_t in_nbytes,
			 u8 * restrict out, size_t out_nbytes_avail)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
	struct deflate_output_bitstream os;
	const u8 *in_cur_base = in_next;
	unsigned max_len = DEFLATE_MAX_MATCH_LEN;
	unsigned nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hash = 0;

	deflate_init_output(&os, out, out_nbytes_avail);
	ht_matchfinder_init(&c->p.f.ht_mf);

	do {
		/* Starting a new DEFLATE block */

		const u8 * const in_block_begin = in_next;
		const u8 * const in_max_block_end = choose_max_block_end(
				in_next, in_end, FAST_SOFT_MAX_BLOCK_LENGTH);
		struct deflate_sequence *seq = c->p.f.sequences;

		deflate_begin_sequences(c, seq);

		do {
			u32 length;
			u32 offset;
			size_t remaining = in_end - in_next;

			if (unlikely(remaining < DEFLATE_MAX_MATCH_LEN)) {
				max_len = remaining;
				if (max_len < HT_MATCHFINDER_REQUIRED_NBYTES) {
					do {
						deflate_choose_literal(c,
							*in_next++, false, seq);
					} while (--max_len);
					break;
				}
				nice_len = MIN(nice_len, max_len);
			}
			length = ht_matchfinder_longest_match(&c->p.f.ht_mf,
							      &in_cur_base,
							      in_next,
							      max_len,
							      nice_len,
							      &next_hash,
							      &offset);
			if (length) {
				/* Match found */
				deflate_choose_match(c, length, offset, false,
						     &seq);
				ht_matchfinder_skip_bytes(&c->p.f.ht_mf,
							  &in_cur_base,
							  in_next + 1,
							  in_end,
							  length - 1,
							  &next_hash);
				in_next += length;
			} else {
				/* No match found */
				deflate_choose_literal(c, *in_next++, false,
						       seq);
			}

			/* Check if it's time to output another block. */
		} while (in_next < in_max_block_end &&
			 seq < &c->p.f.sequences[FAST_SEQ_STORE_LENGTH]);

		deflate_flush_block(c, &os, in_block_begin,
				    in_next - in_block_begin,
				    c->p.f.sequences, in_next == in_end);
	} while (in_next != in_end);

	return deflate_flush_output(&os);
}

/*
 * This is the "greedy" DEFLATE compressor. It always chooses the longest match.
 */
static size_t
deflate_compress_greedy(struct libdeflate_compressor * restrict c,
			const u8 * restrict in, size_t in_nbytes,
			u8 * restrict out, size_t out_nbytes_avail)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
	struct deflate_output_bitstream os;
	const u8 *in_cur_base = in_next;
	unsigned max_len = DEFLATE_MAX_MATCH_LEN;
	unsigned nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hashes[2] = {0, 0};

	deflate_init_output(&os, out, out_nbytes_avail);
	hc_matchfinder_init(&c->p.g.hc_mf);

	do {
		/* Starting a new DEFLATE block */

		const u8 * const in_block_begin = in_next;
		const u8 * const in_max_block_end = choose_max_block_end(
				in_next, in_end, SOFT_MAX_BLOCK_LENGTH);
		struct deflate_sequence *seq = c->p.g.sequences;
		unsigned min_len;

		init_block_split_stats(&c->split_stats);
		deflate_begin_sequences(c, seq);
		min_len = calculate_min_match_len(in_next,
						  in_max_block_end - in_next,
						  c->max_search_depth);
		do {
			u32 length;
			u32 offset;

			adjust_max_and_nice_len(&max_len, &nice_len,
						in_end - in_next);
			length = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next,
						min_len - 1,
						max_len,
						nice_len,
						c->max_search_depth,
						next_hashes,
						&offset);

			if (length >= min_len &&
			    (length > DEFLATE_MIN_MATCH_LEN ||
			     offset <= 4096)) {
				/* Match found */
				deflate_choose_match(c, length, offset, true,
						     &seq);
				hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
							  &in_cur_base,
							  in_next + 1,
							  in_end,
							  length - 1,
							  next_hashes);
				in_next += length;
			} else {
				/* No match found */
				deflate_choose_literal(c, *in_next++, true,
						       seq);
			}

			/* Check if it's time to output another block. */
		} while (in_next < in_max_block_end &&
			 seq < &c->p.g.sequences[SEQ_STORE_LENGTH] &&
			 !should_end_block(&c->split_stats,
					   in_block_begin, in_next, in_end));

		deflate_flush_block(c, &os, in_block_begin,
				    in_next - in_block_begin,
				    c->p.g.sequences, in_next == in_end);
	} while (in_next != in_end);

	return deflate_flush_output(&os);
}

static forceinline size_t
deflate_compress_lazy_generic(struct libdeflate_compressor * restrict c,
			      const u8 * restrict in, size_t in_nbytes,
			      u8 * restrict out, size_t out_nbytes_avail,
			      bool lazy2)
{
	const u8 *in_next = in;
	const u8 *in_end = in_next + in_nbytes;
	struct deflate_output_bitstream os;
	const u8 *in_cur_base = in_next;
	unsigned max_len = DEFLATE_MAX_MATCH_LEN;
	unsigned nice_len = MIN(c->nice_match_length, max_len);
	u32 next_hashes[2] = {0, 0};

	deflate_init_output(&os, out, out_nbytes_avail);
	hc_matchfinder_init(&c->p.g.hc_mf);

	do {
		/* Starting a new DEFLATE block */

		const u8 * const in_block_begin = in_next;
		const u8 * const in_max_block_end = choose_max_block_end(
				in_next, in_end, SOFT_MAX_BLOCK_LENGTH);
		const u8 *next_recalc_min_len =
			in_next + MIN(in_end - in_next, 10000);
		struct deflate_sequence *seq = c->p.g.sequences;
		unsigned min_len;

		init_block_split_stats(&c->split_stats);
		deflate_begin_sequences(c, seq);
		min_len = calculate_min_match_len(in_next,
						  in_max_block_end - in_next,
						  c->max_search_depth);
		do {
			unsigned cur_len;
			unsigned cur_offset;
			unsigned next_len;
			unsigned next_offset;

			/*
			 * Recalculate the minimum match length if it hasn't
			 * been done recently.
			 */
			if (in_next >= next_recalc_min_len) {
				min_len = recalculate_min_match_len(
						&c->freqs,
						c->max_search_depth);
				next_recalc_min_len +=
					MIN(in_end - next_recalc_min_len,
					    in_next - in_block_begin);
			}

			/* Find the longest match at the current position. */
			adjust_max_and_nice_len(&max_len, &nice_len,
						in_end - in_next);
			cur_len = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next,
						min_len - 1,
						max_len,
						nice_len,
						c->max_search_depth,
						next_hashes,
						&cur_offset);
			if (cur_len < min_len ||
			    (cur_len == DEFLATE_MIN_MATCH_LEN &&
			     cur_offset > 8192)) {
				/* No match found.  Choose a literal. */
				deflate_choose_literal(c, *in_next++, true,
						       seq);
				continue;
			}
			in_next++;

have_cur_match:
			/*
			 * We have a match at the current position.
			 * If it's very long, choose it immediately.
			 */
			if (cur_len >= nice_len) {
				deflate_choose_match(c, cur_len, cur_offset,
						     true, &seq);
				hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
							  &in_cur_base,
							  in_next,
							  in_end,
							  cur_len - 1,
							  next_hashes);
				in_next += cur_len - 1;
				continue;
			}

			/*
			 * Try to find a better match at the next position.
			 *
			 * Note: since we already have a match at the *current*
			 * position, we use only half the 'max_search_depth'
			 * when checking the *next* position.  This is a useful
			 * trade-off because it's more worthwhile to use a
			 * greater search depth on the initial match.
			 *
			 * Note: it's possible to structure the code such that
			 * there's only one call to longest_match(), which
			 * handles both the "find the initial match" and "try to
			 * find a better match" cases.  However, it is faster to
			 * have two call sites, with longest_match() inlined at
			 * each.
			 */
			adjust_max_and_nice_len(&max_len, &nice_len,
						in_end - in_next);
			next_len = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next++,
						cur_len - 1,
						max_len,
						nice_len,
						c->max_search_depth >> 1,
						next_hashes,
						&next_offset);
			if (next_len >= cur_len &&
			    4 * (int)(next_len - cur_len) +
			    ((int)bsr32(cur_offset) -
			     (int)bsr32(next_offset)) > 2) {
				/*
				 * Found a better match at the next position.
				 * Output a literal.  Then the next match
				 * becomes the current match.
				 */
				deflate_choose_literal(c, *(in_next - 2), true,
						       seq);
				cur_len = next_len;
				cur_offset = next_offset;
				goto have_cur_match;
			}

			if (lazy2) {
				/* In lazy2 mode, look ahead another position */
				adjust_max_and_nice_len(&max_len, &nice_len,
							in_end - in_next);
				next_len = hc_matchfinder_longest_match(
						&c->p.g.hc_mf,
						&in_cur_base,
						in_next++,
						cur_len - 1,
						max_len,
						nice_len,
						c->max_search_depth >> 2,
						next_hashes,
						&next_offset);
				if (next_len >= cur_len &&
				    4 * (int)(next_len - cur_len) +
				    ((int)bsr32(cur_offset) -
				     (int)bsr32(next_offset)) > 6) {
					/*
					 * There's a much better match two
					 * positions ahead, so use two literals.
					 */
					deflate_choose_literal(
						c, *(in_next - 3), true, seq);
					deflate_choose_literal(
						c, *(in_next - 2), true, seq);
					cur_len = next_len;
					cur_offset = next_offset;
					goto have_cur_match;
				}
				/*
				 * No better match at either of the next 2
				 * positions.  Output the current match.
				 */
				deflate_choose_match(c, cur_len, cur_offset,
						     true, &seq);
				if (cur_len > 3) {
					hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
								  &in_cur_base,
								  in_next,
								  in_end,
								  cur_len - 3,
								  next_hashes);
					in_next += cur_len - 3;
				}
			} else { /* !lazy2 */
				/*
				 * No better match at the next position.  Output
				 * the current match.
				 */
				deflate_choose_match(c, cur_len, cur_offset,
						     true, &seq);
				hc_matchfinder_skip_bytes(&c->p.g.hc_mf,
							  &in_cur_base,
							  in_next,
							  in_end,
							  cur_len - 2,
							  next_hashes);
				in_next += cur_len - 2;
			}
			/* Check if it's time to output another block. */
		} while (in_next < in_max_block_end &&
			 seq < &c->p.g.sequences[SEQ_STORE_LENGTH] &&
			 !should_end_block(&c->split_stats,
					   in_block_begin, in_next, in_end));

		deflate_flush_block(c, &os, in_block_begin,
				    in_next - in_block_begin,
				    c->p.g.sequences, in_next == in_end);
	} while (in_next != in_end);

	return deflate_flush_output(&os);
}

/*
 * This is the "lazy" DEFLATE compressor.  Before choosing a match, it checks to
 * see if there's a better match at the next position.  If yes, it outputs a
 * literal and continues to the next position.  If no, it outputs the match.
 */
static size_t
deflate_compress_lazy(struct libdeflate_compressor * restrict c,
		      const u8 * restrict in, size_t in_nbytes,
		      u8 * restrict out, size_t out_nbytes_avail)
{
	return deflate_compress_lazy_generic(c, in, in_nbytes, out,
					     out_nbytes_avail, false);
}

/*
 * The lazy2 compressor.  This is similar to the regular lazy one, but it looks
 * for a better match at the next 2 positions rather than the next 1.  This
 * makes it take slightly more time, but compress some inputs slightly more.
 */
static size_t
deflate_compress_lazy2(struct libdeflate_compressor * restrict c,
		       const u8 * restrict in, size_t in_nbytes,
		       u8 * restrict out, size_t out_nbytes_avail)
{
	return deflate_compress_lazy_generic(c, in, in_nbytes, out,
					     out_nbytes_avail, true);
}

#if SUPPORT_NEAR_OPTIMAL_PARSING

/*
 * Follow the minimum-cost path in the graph of possible match/literal choices
 * for the current block and compute the frequencies of the Huffman symbols that
 * would be needed to output those matches and literals.
 */
static void
deflate_tally_item_list(struct libdeflate_compressor *c, u32 block_length)
{
	struct deflate_optimum_node *cur_node = &c->p.n.optimum_nodes[0];
	struct deflate_optimum_node *end_node =
		&c->p.n.optimum_nodes[block_length];

	do {
		unsigned length = cur_node->item & OPTIMUM_LEN_MASK;
		unsigned offset = cur_node->item >> OPTIMUM_OFFSET_SHIFT;

		if (length == 1) {
			/* Literal */
			c->freqs.litlen[offset]++;
		} else {
			/* Match */
			c->freqs.litlen[DEFLATE_FIRST_LEN_SYM +
					deflate_length_slot[length]]++;
			c->freqs.offset[c->p.n.offset_slot_full[offset]]++;
		}
		cur_node += length;
	} while (cur_node != end_node);

	/* Tally the end-of-block symbol. */
	c->freqs.litlen[DEFLATE_END_OF_BLOCK]++;
}

/* Set the current cost model from the codeword lengths specified in @lens. */
static void
deflate_set_costs_from_codes(struct libdeflate_compressor *c,
			     const struct deflate_lens *lens)
{
	unsigned i;

	/* Literals */
	for (i = 0; i < DEFLATE_NUM_LITERALS; i++) {
		u32 bits = (lens->litlen[i] ?
			    lens->litlen[i] : LITERAL_NOSTAT_BITS);

		c->p.n.costs.literal[i] = bits * BIT_COST;
	}

	/* Lengths */
	for (i = DEFLATE_MIN_MATCH_LEN; i <= DEFLATE_MAX_MATCH_LEN; i++) {
		unsigned length_slot = deflate_length_slot[i];
		unsigned litlen_sym = DEFLATE_FIRST_LEN_SYM + length_slot;
		u32 bits = (lens->litlen[litlen_sym] ?
			    lens->litlen[litlen_sym] : LENGTH_NOSTAT_BITS);

		bits += deflate_extra_length_bits[length_slot];
		c->p.n.costs.length[i] = bits * BIT_COST;
	}

	/* Offset slots */
	for (i = 0; i < ARRAY_LEN(deflate_offset_slot_base); i++) {
		u32 bits = (lens->offset[i] ?
			    lens->offset[i] : OFFSET_NOSTAT_BITS);

		bits += deflate_extra_offset_bits[i];
		c->p.n.costs.offset_slot[i] = bits * BIT_COST;
	}
}

/*
 * This lookup table gives the default cost of a literal symbol and of a length
 * symbol, depending on the characteristics of the input data.  It was generated
 * by scripts/gen_default_litlen_costs.py.
 *
 * This table is indexed first by the estimated match probability:
 *
 *	i=0: data doesn't contain many matches	[match_prob=0.25]
 *	i=1: neutral				[match_prob=0.50]
 *	i=2: data contains lots of matches	[match_prob=0.75]
 *
 * This lookup produces a subtable which maps the number of distinct used
 * literals to the default cost of a literal symbol, i.e.:
 *
 *	int(-log2((1 - match_prob) / num_used_literals) * BIT_COST)
 *
 * ... for num_used_literals in [1, 256] (and 0, which is copied from 1).  This
 * accounts for literals usually getting cheaper as the number of distinct
 * literals decreases, and as the proportion of literals to matches increases.
 *
 * The lookup also produces the cost of a length symbol, which is:
 *
 *	int(-log2(match_prob/NUM_LEN_SLOTS) * BIT_COST)
 *
 * Note: we don't currently assign different costs to different literal symbols,
 * or to different length symbols, as this is hard to do in a useful way.
 */
static const struct {
	u8 used_lits_to_lit_cost[257];
	u8 len_sym_cost;
} default_litlen_costs[] = {
	{ /* match_prob = 0.25 */
		.used_lits_to_lit_cost = {
			6, 6, 22, 32, 38, 43, 48, 51,
			54, 57, 59, 61, 64, 65, 67, 69,
			70, 72, 73, 74, 75, 76, 77, 79,
			80, 80, 81, 82, 83, 84, 85, 85,
			86, 87, 88, 88, 89, 89, 90, 91,
			91, 92, 92, 93, 93, 94, 95, 95,
			96, 96, 96, 97, 97, 98, 98, 99,
			99, 99, 100, 100, 101, 101, 101, 102,
			102, 102, 103, 103, 104, 104, 104, 105,
			105, 105, 105, 106, 106, 106, 107, 107,
			107, 108, 108, 108, 108, 109, 109, 109,
			109, 110, 110, 110, 111, 111, 111, 111,
			112, 112, 112, 112, 112, 113, 113, 113,
			113, 114, 114, 114, 114, 114, 115, 115,
			115, 115, 115, 116, 116, 116, 116, 116,
			117, 117, 117, 117, 117, 118, 118, 118,
			118, 118, 118, 119, 119, 119, 119, 119,
			120, 120, 120, 120, 120, 120, 121, 121,
			121, 121, 121, 121, 121, 122, 122, 122,
			122, 122, 122, 123, 123, 123, 123, 123,
			123, 123, 124, 124, 124, 124, 124, 124,
			124, 125, 125, 125, 125, 125, 125, 125,
			125, 126, 126, 126, 126, 126, 126, 126,
			127, 127, 127, 127, 127, 127, 127, 127,
			128, 128, 128, 128, 128, 128, 128, 128,
			128, 129, 129, 129, 129, 129, 129, 129,
			129, 129, 130, 130, 130, 130, 130, 130,
			130, 130, 130, 131, 131, 131, 131, 131,
			131, 131, 131, 131, 131, 132, 132, 132,
			132, 132, 132, 132, 132, 132, 132, 133,
			133, 133, 133, 133, 133, 133, 133, 133,
			133, 134, 134, 134, 134, 134, 134, 134,
			134,
		},
		.len_sym_cost = 109,
	}, { /* match_prob = 0.5 */
		.used_lits_to_lit_cost = {
			16, 16, 32, 41, 48, 53, 57, 60,
			64, 66, 69, 71, 73, 75, 76, 78,
			80, 81, 82, 83, 85, 86, 87, 88,
			89, 90, 91, 92, 92, 93, 94, 95,
			96, 96, 97, 98, 98, 99, 99, 100,
			101, 101, 102, 102, 103, 103, 104, 104,
			105, 105, 106, 106, 107, 107, 108, 108,
			108, 109, 109, 110, 110, 110, 111, 111,
			112, 112, 112, 113, 113, 113, 114, 114,
			114, 115, 115, 115, 115, 116, 116, 116,
			117, 117, 117, 118, 118, 118, 118, 119,
			119, 119, 119, 120, 120, 120, 120, 121,
			121, 121, 121, 122, 122, 122, 122, 122,
			123, 123, 123, 123, 124, 124, 124, 124,
			124, 125, 125, 125, 125, 125, 126, 126,
			126, 126, 126, 127, 127, 127, 127, 127,
			128, 128, 128, 128, 128, 128, 129, 129,
			129, 129, 129, 129, 130, 130, 130, 130,
			130, 130, 131, 131, 131, 131, 131, 131,
			131, 132, 132, 132, 132, 132, 132, 133,
			133, 133, 133, 133, 133, 133, 134, 134,
			134, 134, 134, 134, 134, 134, 135, 135,
			135, 135, 135, 135, 135, 135, 136, 136,
			136, 136, 136, 136, 136, 136, 137, 137,
			137, 137, 137, 137, 137, 137, 138, 138,
			138, 138, 138, 138, 138, 138, 138, 139,
			139, 139, 139, 139, 139, 139, 139, 139,
			140, 140, 140, 140, 140, 140, 140, 140,
			140, 141, 141, 141, 141, 141, 141, 141,
			141, 141, 141, 142, 142, 142, 142, 142,
			142, 142, 142, 142, 142, 142, 143, 143,
			143, 143, 143, 143, 143, 143, 143, 143,
			144,
		},
		.len_sym_cost = 93,
	}, { /* match_prob = 0.75 */
		.used_lits_to_lit_cost = {
			32, 32, 48, 57, 64, 69, 73, 76,
			80, 82, 85, 87, 89, 91, 92, 94,
			96, 97, 98, 99, 101, 102, 103, 104,
			105, 106, 107, 108, 108, 109, 110, 111,
			112, 112, 113, 114, 114, 115, 115, 116,
			117, 117, 118, 118, 119, 119, 120, 120,
			121, 121, 122, 122, 123, 123, 124, 124,
			124, 125, 125, 126, 126, 126, 127, 127,
			128, 128, 128, 129, 129, 129, 130, 130,
			130, 131, 131, 131, 131, 132, 132, 132,
			133, 133, 133, 134, 134, 134, 134, 135,
			135, 135, 135, 136, 136, 136, 136, 137,
			137, 137, 137, 138, 138, 138, 138, 138,
			139, 139, 139, 139, 140, 140, 140, 140,
			140, 141, 141, 141, 141, 141, 142, 142,
			142, 142, 142, 143, 143, 143, 143, 143,
			144, 144, 144, 144, 144, 144, 145, 145,
			145, 145, 145, 145, 146, 146, 146, 146,
			146, 146, 147, 147, 147, 147, 147, 147,
			147, 148, 148, 148, 148, 148, 148, 149,
			149, 149, 149, 149, 149, 149, 150, 150,
			150, 150, 150, 150, 150, 150, 151, 151,
			151, 151, 151, 151, 151, 151, 152, 152,
			152, 152, 152, 152, 152, 152, 153, 153,
			153, 153, 153, 153, 153, 153, 154, 154,
			154, 154, 154, 154, 154, 154, 154, 155,
			155, 155, 155, 155, 155, 155, 155, 155,
			156, 156, 156, 156, 156, 156, 156, 156,
			156, 157, 157, 157, 157, 157, 157, 157,
			157, 157, 157, 158, 158, 158, 158, 158,
			158, 158, 158, 158, 158, 158, 159, 159,
			159, 159, 159, 159, 159, 159, 159, 159,
			160,
		},
		.len_sym_cost = 84,
	},
};

/*
 * Choose the default costs for literal and length symbols.  These symbols are
 * both part of the litlen alphabet.
 */
static void
deflate_choose_default_litlen_costs(struct libdeflate_compressor *c,
				    const u8 *block_begin, u32 block_length,
				    u32 *lit_cost, u32 *len_sym_cost)
{
	unsigned num_used_literals = 0;
	u32 literal_freq = block_length;
	u32 match_freq = 0;
	u32 cutoff;
	u32 i;

	/* Calculate the number of distinct literals that exist in the data. */
	memset(c->freqs.litlen, 0,
	       DEFLATE_NUM_LITERALS * sizeof(c->freqs.litlen[0]));
	cutoff = literal_freq >> 11; /* Ignore literals used very rarely. */
	for (i = 0; i < block_length; i++)
		c->freqs.litlen[block_begin[i]]++;
	for (i = 0; i < DEFLATE_NUM_LITERALS; i++) {
		if (c->freqs.litlen[i] > cutoff)
			num_used_literals++;
	}
	if (num_used_literals == 0)
		num_used_literals = 1;

	/*
	 * Estimate the relative frequency of literals and matches in the
	 * optimal parsing solution.  We don't know the optimal solution, so
	 * this can only be a very rough estimate.  Therefore, we basically use
	 * the match frequency from a greedy parse.  We also apply the min_len
	 * heuristic used by the greedy and lazy parsers, to avoid counting too
	 * many matches when literals are cheaper than short matches.
	 */
	match_freq = 0;
	i = choose_min_match_len(num_used_literals, c->max_search_depth);
	for (; i < ARRAY_LEN(c->p.n.match_len_freqs); i++) {
		match_freq += c->p.n.match_len_freqs[i];
		literal_freq -= i * c->p.n.match_len_freqs[i];
	}
	if ((s32)literal_freq < 0) /* shouldn't happen */
		literal_freq = 0;

	if (match_freq > literal_freq)
		i = 2; /* many matches */
	else if (match_freq * 4 > literal_freq)
		i = 1; /* neutral */
	else
		i = 0; /* few matches */

	STATIC_ASSERT(BIT_COST == 16);
	*lit_cost = default_litlen_costs[i].used_lits_to_lit_cost[
							num_used_literals];
	*len_sym_cost = default_litlen_costs[i].len_sym_cost;
}

static forceinline u32
deflate_default_length_cost(unsigned len, u32 len_sym_cost)
{
	unsigned slot = deflate_length_slot[len];
	u32 num_extra_bits = deflate_extra_length_bits[slot];

	return len_sym_cost + (num_extra_bits * BIT_COST);
}

static forceinline u32
deflate_default_offset_slot_cost(unsigned slot)
{
	u32 num_extra_bits = deflate_extra_offset_bits[slot];
	/*
	 * Assume that all offset symbols are equally probable.
	 * The resulting cost is 'int(-log2(1/30) * BIT_COST)',
	 * where 30 is the number of potentially-used offset symbols.
	 */
	u32 offset_sym_cost = 4*BIT_COST + (907*BIT_COST)/1000;

	return offset_sym_cost + (num_extra_bits * BIT_COST);
}

/* Set default symbol costs for the first block's first optimization pass. */
static void
deflate_set_default_costs(struct libdeflate_compressor *c,
			  u32 lit_cost, u32 len_sym_cost)
{
	unsigned i;

	/* Literals */
	for (i = 0; i < DEFLATE_NUM_LITERALS; i++)
		c->p.n.costs.literal[i] = lit_cost;

	/* Lengths */
	for (i = DEFLATE_MIN_MATCH_LEN; i <= DEFLATE_MAX_MATCH_LEN; i++)
		c->p.n.costs.length[i] =
			deflate_default_length_cost(i, len_sym_cost);

	/* Offset slots */
	for (i = 0; i < ARRAY_LEN(deflate_offset_slot_base); i++)
		c->p.n.costs.offset_slot[i] =
			deflate_default_offset_slot_cost(i);
}

static forceinline void
deflate_adjust_cost(u32 *cost_p, u32 default_cost, int change_amount)
{
	if (change_amount == 0)
		/* Block is very similar to previous; prefer previous costs. */
		*cost_p = (default_cost + 3 * *cost_p) / 4;
	else if (change_amount == 1)
		*cost_p = (default_cost + *cost_p) / 2;
	else if (change_amount == 2)
		*cost_p = (5 * default_cost + 3 * *cost_p) / 8;
	else
		/* Block differs greatly from previous; prefer default costs. */
		*cost_p = (3 * default_cost + *cost_p) / 4;
}

static forceinline void
deflate_adjust_costs_impl(struct libdeflate_compressor *c,
			  u32 lit_cost, u32 len_sym_cost, int change_amount)
{
	unsigned i;

	/* Literals */
	for (i = 0; i < DEFLATE_NUM_LITERALS; i++)
		deflate_adjust_cost(&c->p.n.costs.literal[i], lit_cost,
				    change_amount);

	/* Lengths */
	for (i = DEFLATE_MIN_MATCH_LEN; i <= DEFLATE_MAX_MATCH_LEN; i++)
		deflate_adjust_cost(&c->p.n.costs.length[i],
				    deflate_default_length_cost(i,
								len_sym_cost),
				    change_amount);

	/* Offset slots */
	for (i = 0; i < ARRAY_LEN(deflate_offset_slot_base); i++)
		deflate_adjust_cost(&c->p.n.costs.offset_slot[i],
				    deflate_default_offset_slot_cost(i),
				    change_amount);
}

/*
 * Adjust the costs when beginning a new block.
 *
 * Since the current costs have been optimized for the data, it's undesirable to
 * throw them away and start over with the default costs.  At the same time, we
 * don't want to bias the parse by assuming that the next block will be similar
 * to the current block.  As a compromise, make the costs closer to the
 * defaults, but don't simply set them to the defaults.
 */
static void
deflate_adjust_costs(struct libdeflate_compressor *c,
		     u32 lit_cost, u32 len_sym_cost)
{
	u64 total_delta = 0;
	u64 cutoff;
	int i;

	/*
	 * Decide how different the current block is from the previous block,
	 * using the block splitting statistics from the current and previous
	 * blocks.  The more different the current block is, the more we prefer
	 * the default costs rather than the previous block's costs.
	 *
	 * The algorithm here is similar to the end-of-block check one, but here
	 * we compare two entire blocks rather than a partial block with a small
	 * extra part, and therefore we need 64-bit numbers in some places.
	 */
	for (i = 0; i < NUM_OBSERVATION_TYPES; i++) {
		u64 prev = (u64)c->p.n.prev_observations[i] *
			    c->split_stats.num_observations;
		u64 cur = (u64)c->split_stats.observations[i] *
			  c->p.n.prev_num_observations;

		total_delta += prev > cur ? prev - cur : cur - prev;
	}
	cutoff = ((u64)c->p.n.prev_num_observations *
		  c->split_stats.num_observations * 200) / 512;

	if (4 * total_delta > 9 * cutoff)
		deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 3);
	else if (2 * total_delta > 3 * cutoff)
		deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 2);
	else if (2 * total_delta > cutoff)
		deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 1);
	else
		deflate_adjust_costs_impl(c, lit_cost, len_sym_cost, 0);
}

/*
 * Find the minimum-cost path through the graph of possible match/literal
 * choices for this block.
 *
 * We find the minimum cost path from 'c->p.n.optimum_nodes[0]', which
 * represents the node at the beginning of the block, to
 * 'c->p.n.optimum_nodes[block_length]', which represents the node at the end of
 * the block.  Edge costs are evaluated using the cost model 'c->p.n.costs'.
 *
 * The algorithm works backwards, starting at the end node and proceeding
 * backwards one node at a time.  At each node, the minimum cost to reach the
 * end node is computed and the match/literal choice that begins that path is
 * saved.
 */
static void
deflate_find_min_cost_path(struct libdeflate_compressor *c,
			   const u32 block_length,
			   const struct lz_match *cache_ptr)
{
	struct deflate_optimum_node *end_node =
		&c->p.n.optimum_nodes[block_length];
	struct deflate_optimum_node *cur_node = end_node;

	cur_node->cost_to_end = 0;
	do {
		unsigned num_matches;
		unsigned literal;
		u32 best_cost_to_end;

		cur_node--;
		cache_ptr--;

		num_matches = cache_ptr->length;
		literal = cache_ptr->offset;

		/* It's always possible to choose a literal. */
		best_cost_to_end = c->p.n.costs.literal[literal] +
				   (cur_node + 1)->cost_to_end;
		cur_node->item = ((u32)literal << OPTIMUM_OFFSET_SHIFT) | 1;

		/* Also consider matches if there are any. */
		if (num_matches) {
			const struct lz_match *match;
			unsigned len;
			unsigned offset;
			unsigned offset_slot;
			u32 offset_cost;
			u32 cost_to_end;

			/*
			 * Consider each length from the minimum
			 * (DEFLATE_MIN_MATCH_LEN) to the length of the longest
			 * match found at this position.  For each length, we
			 * consider only the smallest offset for which that
			 * length is available.  Although this is not guaranteed
			 * to be optimal due to the possibility of a larger
			 * offset costing less than a smaller offset to code,
			 * this is a very useful heuristic.
			 */
			match = cache_ptr - num_matches;
			len = DEFLATE_MIN_MATCH_LEN;
			do {
				offset = match->offset;
				offset_slot = c->p.n.offset_slot_full[offset];
				offset_cost =
					c->p.n.costs.offset_slot[offset_slot];
				do {
					cost_to_end = offset_cost +
						c->p.n.costs.length[len] +
						(cur_node + len)->cost_to_end;
					if (cost_to_end < best_cost_to_end) {
						best_cost_to_end = cost_to_end;
						cur_node->item = len |
							((u32)offset <<
							 OPTIMUM_OFFSET_SHIFT);
					}
				} while (++len <= match->length);
			} while (++match != cache_ptr);
			cache_ptr -= num_matches;
		}
		cur_node->cost_to_end = best_cost_to_end;
	} while (cur_node != &c->p.n.optimum_nodes[0]);
}

/*
 * Choose the literal/match sequence to use for the current block.  The basic
 * algorithm finds a minimum-cost path through the block's graph of
 * literal/match choices, given a cost model.  However, the cost of each symbol
 * is unknown until the Huffman codes have been built, but at the same time the
 * Huffman codes depend on the frequencies of chosen symbols.  Consequently,
 * multiple passes must be used to try to approximate an optimal solution.  The
 * first pass uses default costs, mixed with the costs from the previous block
 * if any.  Later passes use the Huffman codeword lengths from the previous pass
 * as the costs.
 */
static void
deflate_optimize_block(struct libdeflate_compressor *c,
		       const u8 *block_begin, u32 block_length,
		       const struct lz_match *cache_ptr, bool is_first_block,
		       bool is_final_block)
{
	unsigned num_passes_remaining = c->p.n.num_optim_passes;
	u32 lit_cost, len_sym_cost;
	u32 i;

	/*
	 * Force the block to really end at the desired length, even if some
	 * matches extend beyond it.
	 */
	for (i = block_length;
	     i <= MIN(block_length - 1 + DEFLATE_MAX_MATCH_LEN,
		      ARRAY_LEN(c->p.n.optimum_nodes) - 1); i++)
		c->p.n.optimum_nodes[i].cost_to_end = 0x80000000;

	/* Set the initial costs. */
	deflate_choose_default_litlen_costs(c, block_begin, block_length,
					    &lit_cost, &len_sym_cost);
	if (is_first_block)
		deflate_set_default_costs(c, lit_cost, len_sym_cost);
	else
		deflate_adjust_costs(c, lit_cost, len_sym_cost);

	do {
		/* Find the minimum cost path for this pass. */
		deflate_find_min_cost_path(c, block_length, cache_ptr);

		/* Compute frequencies of the chosen symbols. */
		deflate_reset_symbol_frequencies(c);
		deflate_tally_item_list(c, block_length);

		/* Make the Huffman codes. */
		deflate_make_huffman_codes(&c->freqs, &c->codes);

		/*
		 * Update the costs.  After the last optimization pass, the
		 * final costs won't be needed for this block, but they will be
		 * used in determining the initial costs for the next block.
		 */
		if (--num_passes_remaining || !is_final_block)
			deflate_set_costs_from_codes(c, &c->codes.lens);
	} while (num_passes_remaining);
}

static void
deflate_near_optimal_init_stats(struct libdeflate_compressor *c)
{
	init_block_split_stats(&c->split_stats);
	memset(c->p.n.new_match_len_freqs, 0,
	       sizeof(c->p.n.new_match_len_freqs));
	memset(c->p.n.match_len_freqs, 0, sizeof(c->p.n.match_len_freqs));
}

static void
deflate_near_optimal_merge_stats(struct libdeflate_compressor *c)
{
	unsigned i;

	merge_new_observations(&c->split_stats);
	for (i = 0; i < ARRAY_LEN(c->p.n.match_len_freqs); i++) {
		c->p.n.match_len_freqs[i] += c->p.n.new_match_len_freqs[i];
		c->p.n.new_match_len_freqs[i] = 0;
	}
}

/*
 * Save some literal/match statistics from the previous block so that
 * deflate_adjust_costs() will be able to decide how much the current block
 * differs from the previous one.
 */
static void
deflate_near_optimal_save_stats(struct libdeflate_compressor *c)
{
	int i;

	for (i = 0; i < NUM_OBSERVATION_TYPES; i++)
		c->p.n.prev_observations[i] = c->split_stats.observations[i];
	c->p.n.prev_num_observations = c->split_stats.num_observations;
}

static void
deflate_near_optimal_clear_old_stats(struct libdeflate_compressor *c)
{
	int i;

	for (i = 0; i < NUM_OBSERVATION_TYPES; i++)
		c->split_stats.observations[i] = 0;
	c->split_stats.num_observations = 0;
	memset(c->p.n.match_len_freqs, 0, sizeof(c->p.n.match_len_freqs));
}

/*
 * This is the "near-optimal" DEFLATE compressor.  It computes the optimal
 * representation of each DEFLATE block using a minimum-cost path search over
 * the graph of possible match/literal choices for that block, assuming a
 * certain cost for each Huffman symbol.
 *
 * For several reasons, the end result is not guaranteed to be optimal:
 *
 * - Nonoptimal choice of blocks
 * - Heuristic limitations on which matches are actually considered
 * - Symbol costs are unknown until the symbols have already been chosen
 *   (so iterative optimization must be used)
 */
static size_t
deflate_compress_near_optimal(struct libdeflate_compressor * restrict c,
			      const u8 * restrict in, size_t in_nbytes,
			      u8 * restrict out, size_t out_nbytes_avail)
{
	const u8 *in_next = in;
	const u8 *in_block_begin = in_next;
	const u8 *in_end = in_next + in_nbytes;
	struct deflate_output_bitstream os;
	const u8 *in_cur_base = in_next;
	const u8 *in_next_slide =
		in_next + MIN(in_end - in_next, MATCHFINDER_WINDOW_SIZE);
	unsigned max_len = DEFLATE_MAX_MATCH_LEN;
	unsigned nice_len = MIN(c->nice_match_length, max_len);
	struct lz_match *cache_ptr = c->p.n.match_cache;
	u32 next_hashes[2] = {0, 0};

	deflate_init_output(&os, out, out_nbytes_avail);
	bt_matchfinder_init(&c->p.n.bt_mf);
	deflate_near_optimal_init_stats(c);

	do {
		/* Starting a new DEFLATE block */
		const u8 * const in_max_block_end = choose_max_block_end(
				in_block_begin, in_end, SOFT_MAX_BLOCK_LENGTH);
		const u8 *prev_end_block_check = NULL;
		bool change_detected = false;
		const u8 *next_observation = in_next;
		unsigned min_len;

		/*
		 * Use the minimum match length heuristic to improve the
		 * literal/match statistics gathered during matchfinding.
		 * However, the actual near-optimal parse won't respect min_len,
		 * as it can accurately assess the costs of different matches.
		 */
		min_len = calculate_min_match_len(
					in_block_begin,
					in_max_block_end - in_block_begin,
					c->max_search_depth);

		/*
		 * Find matches until we decide to end the block.  We end the
		 * block if any of the following is true:
		 *
		 * (1) Maximum block length has been reached
		 * (2) Match catch may overflow.
		 * (3) Block split heuristic says to split now.
		 */
		for (;;) {
			struct lz_match *matches;
			unsigned best_len;
			size_t remaining = in_end - in_next;

			/* Slide the window forward if needed. */
			if (in_next == in_next_slide) {
				bt_matchfinder_slide_window(&c->p.n.bt_mf);
				in_cur_base = in_next;
				in_next_slide = in_next +
					MIN(remaining, MATCHFINDER_WINDOW_SIZE);
			}

			/*
			 * Find matches with the current position using the
			 * binary tree matchfinder and save them in match_cache.
			 *
			 * Note: the binary tree matchfinder is more suited for
			 * optimal parsing than the hash chain matchfinder.  The
			 * reasons for this include:
			 *
			 * - The binary tree matchfinder can find more matches
			 *   in the same number of steps.
			 * - One of the major advantages of hash chains is that
			 *   skipping positions (not searching for matches at
			 *   them) is faster; however, with optimal parsing we
			 *   search for matches at almost all positions, so this
			 *   advantage of hash chains is negated.
			 */
			matches = cache_ptr;
			best_len = 0;
			adjust_max_and_nice_len(&max_len, &nice_len, remaining);
			if (likely(max_len >= BT_MATCHFINDER_REQUIRED_NBYTES)) {
				cache_ptr = bt_matchfinder_get_matches(
						&c->p.n.bt_mf,
						in_cur_base,
						in_next - in_cur_base,
						max_len,
						nice_len,
						c->max_search_depth,
						next_hashes,
						matches);
				if (cache_ptr > matches)
					best_len = cache_ptr[-1].length;
			}
			if (in_next >= next_observation) {
				if (best_len >= min_len) {
					observe_match(&c->split_stats,
						      best_len);
					next_observation = in_next + best_len;
					c->p.n.new_match_len_freqs[best_len]++;
				} else {
					observe_literal(&c->split_stats,
							*in_next);
					next_observation = in_next + 1;
				}
			}

			cache_ptr->length = cache_ptr - matches;
			cache_ptr->offset = *in_next;
			in_next++;
			cache_ptr++;

			/*
			 * If there was a very long match found, don't cache any
			 * matches for the bytes covered by that match.  This
			 * avoids degenerate behavior when compressing highly
			 * redundant data, where the number of matches can be
			 * very large.
			 *
			 * This heuristic doesn't actually hurt the compression
			 * ratio very much.  If there's a long match, then the
			 * data must be highly compressible, so it doesn't
			 * matter much what we do.
			 */
			if (best_len >= DEFLATE_MIN_MATCH_LEN &&
			    best_len >= nice_len) {
				--best_len;
				do {
					remaining = in_end - in_next;
					if (in_next == in_next_slide) {
						bt_matchfinder_slide_window(
							&c->p.n.bt_mf);
						in_cur_base = in_next;
						in_next_slide = in_next +
							MIN(remaining,
							    MATCHFINDER_WINDOW_SIZE);
					}
					adjust_max_and_nice_len(&max_len,
								&nice_len,
								remaining);
					if (max_len >=
					    BT_MATCHFINDER_REQUIRED_NBYTES) {
						bt_matchfinder_skip_byte(
							&c->p.n.bt_mf,
							in_cur_base,
							in_next - in_cur_base,
							nice_len,
							c->max_search_depth,
							next_hashes);
					}
					cache_ptr->length = 0;
					cache_ptr->offset = *in_next;
					in_next++;
					cache_ptr++;
				} while (--best_len);
			}
			/* Maximum block length or end of input reached? */
			if (in_next >= in_max_block_end)
				break;
			/* Match cache overflowed? */
			if (cache_ptr >=
			    &c->p.n.match_cache[MATCH_CACHE_LENGTH])
				break;
			/* Not ready to try to end the block (again)? */
			if (!ready_to_check_block(&c->split_stats,
						  in_block_begin, in_next,
						  in_end))
				continue;
			/* Check if it would be worthwhile to end the block. */
			if (do_end_block_check(&c->split_stats,
					       in_next - in_block_begin)) {
				change_detected = true;
				break;
			}
			/* Ending the block doesn't seem worthwhile here. */
			deflate_near_optimal_merge_stats(c);
			prev_end_block_check = in_next;
		}
		/*
		 * All the matches for this block have been cached.  Now choose
		 * the precise end of the block and the sequence of items to
		 * output to represent it, then flush the block.
		 */
		if (change_detected && prev_end_block_check != NULL) {
			/*
			 * The block is being ended because a recent chunk of
			 * data differs from the rest of the block.  We could
			 * end the block at 'in_next' like the greedy and lazy
			 * compressors do, but that's not ideal since it would
			 * include the differing chunk in the block.  The
			 * near-optimal compressor has time to do a better job.
			 * Therefore, we rewind to just before the chunk, and
			 * output a block that only goes up to there.
			 *
			 * We then set things up to correctly start the next
			 * block, considering that some work has already been
			 * done on it (some matches found and stats gathered).
			 */
			struct lz_match *orig_cache_ptr = cache_ptr;
			const u8 *in_block_end = prev_end_block_check;
			u32 block_length = in_block_end - in_block_begin;
			bool is_first = (in_block_begin == in);
			bool is_final = false;
			u32 num_bytes_to_rewind = in_next - in_block_end;
			size_t cache_len_rewound;

			/* Rewind the match cache. */
			do {
				cache_ptr--;
				cache_ptr -= cache_ptr->length;
			} while (--num_bytes_to_rewind);
			cache_len_rewound = orig_cache_ptr - cache_ptr;

			deflate_optimize_block(c, in_block_begin, block_length,
					       cache_ptr, is_first, is_final);
			deflate_flush_block(c, &os, in_block_begin,
					    block_length, NULL, is_final);
			memmove(c->p.n.match_cache, cache_ptr,
				cache_len_rewound * sizeof(*cache_ptr));
			cache_ptr = &c->p.n.match_cache[cache_len_rewound];
			deflate_near_optimal_save_stats(c);
			/*
			 * Clear the stats for the just-flushed block, leaving
			 * just the stats for the beginning of the next block.
			 */
			deflate_near_optimal_clear_old_stats(c);
			in_block_begin = in_block_end;
		} else {
			/*
			 * The block is being ended for a reason other than a
			 * differing data chunk being detected.  Don't rewind at
			 * all; just end the block at the current position.
			 */
			u32 block_length = in_next - in_block_begin;
			bool is_first = (in_block_begin == in);
			bool is_final = (in_next == in_end);

			deflate_near_optimal_merge_stats(c);
			deflate_optimize_block(c, in_block_begin, block_length,
					       cache_ptr, is_first, is_final);
			deflate_flush_block(c, &os, in_block_begin,
					    block_length, NULL, is_final);
			cache_ptr = &c->p.n.match_cache[0];
			deflate_near_optimal_save_stats(c);
			deflate_near_optimal_init_stats(c);
			in_block_begin = in_next;
		}
	} while (in_next != in_end);

	return deflate_flush_output(&os);
}

/* Initialize c->p.n.offset_slot_full. */
static void
deflate_init_offset_slot_full(struct libdeflate_compressor *c)
{
	unsigned offset_slot;
	unsigned offset;
	unsigned offset_end;

	for (offset_slot = 0; offset_slot < ARRAY_LEN(deflate_offset_slot_base);
	     offset_slot++) {
		offset = deflate_offset_slot_base[offset_slot];
		offset_end = offset +
			     (1 << deflate_extra_offset_bits[offset_slot]);
		do {
			c->p.n.offset_slot_full[offset] = offset_slot;
		} while (++offset != offset_end);
	}
}

#endif /* SUPPORT_NEAR_OPTIMAL_PARSING */

LIBDEFLATEEXPORT struct libdeflate_compressor * LIBDEFLATEAPI
libdeflate_alloc_compressor(int compression_level)
{
	struct libdeflate_compressor *c;
	size_t size = offsetof(struct libdeflate_compressor, p);

	check_buildtime_parameters();

	if (compression_level < 0 || compression_level > 12)
		return NULL;

#if SUPPORT_NEAR_OPTIMAL_PARSING
	if (compression_level >= 10)
		size += sizeof(c->p.n);
	else
#endif
	{
		if (compression_level >= 2)
			size += sizeof(c->p.g);
		else if (compression_level == 1)
			size += sizeof(c->p.f);
	}

	c = libdeflate_aligned_malloc(MATCHFINDER_MEM_ALIGNMENT, size);
	if (!c)
		return NULL;

	c->compression_level = compression_level;

	/*
	 * The higher the compression level, the more we should bother trying to
	 * compress very small inputs.
	 */
	c->min_size_to_compress = 56 - (compression_level * 4);

	switch (compression_level) {
	case 0:
		c->impl = deflate_compress_none;
		break;
	case 1:
		c->impl = deflate_compress_fastest;
		/* max_search_depth is unused. */
		c->nice_match_length = 32;
		break;
	case 2:
		c->impl = deflate_compress_greedy;
		c->max_search_depth = 6;
		c->nice_match_length = 10;
		break;
	case 3:
		c->impl = deflate_compress_greedy;
		c->max_search_depth = 12;
		c->nice_match_length = 14;
		break;
	case 4:
		c->impl = deflate_compress_greedy;
		c->max_search_depth = 16;
		c->nice_match_length = 30;
		break;
	case 5:
		c->impl = deflate_compress_lazy;
		c->max_search_depth = 16;
		c->nice_match_length = 30;
		break;
	case 6:
		c->impl = deflate_compress_lazy;
		c->max_search_depth = 35;
		c->nice_match_length = 65;
		break;
	case 7:
		c->impl = deflate_compress_lazy;
		c->max_search_depth = 100;
		c->nice_match_length = 130;
		break;
	case 8:
		c->impl = deflate_compress_lazy2;
		c->max_search_depth = 300;
		c->nice_match_length = DEFLATE_MAX_MATCH_LEN;
		break;
	case 9:
#if !SUPPORT_NEAR_OPTIMAL_PARSING
	default:
#endif
		c->impl = deflate_compress_lazy2;
		c->max_search_depth = 600;
		c->nice_match_length = DEFLATE_MAX_MATCH_LEN;
		break;
#if SUPPORT_NEAR_OPTIMAL_PARSING
	case 10:
		c->impl = deflate_compress_near_optimal;
		c->max_search_depth = 35;
		c->nice_match_length = 75;
		c->p.n.num_optim_passes = 2;
		deflate_init_offset_slot_full(c);
		break;
	case 11:
		c->impl = deflate_compress_near_optimal;
		c->max_search_depth = 70;
		c->nice_match_length = 150;
		c->p.n.num_optim_passes = 3;
		deflate_init_offset_slot_full(c);
		break;
	case 12:
	default:
		c->impl = deflate_compress_near_optimal;
		c->max_search_depth = 150;
		c->nice_match_length = DEFLATE_MAX_MATCH_LEN;
		c->p.n.num_optim_passes = 4;
		deflate_init_offset_slot_full(c);
		break;
#endif /* SUPPORT_NEAR_OPTIMAL_PARSING */
	}

	deflate_init_static_codes(c);

	return c;
}

LIBDEFLATEEXPORT size_t LIBDEFLATEAPI
libdeflate_deflate_compress(struct libdeflate_compressor *c,
			    const void *in, size_t in_nbytes,
			    void *out, size_t out_nbytes_avail)
{
	if (unlikely(out_nbytes_avail < OUTPUT_END_PADDING))
		return 0;

	/* For extremely small inputs, just use a single uncompressed block. */
	if (unlikely(in_nbytes < c->min_size_to_compress)) {
		struct deflate_output_bitstream os;
		deflate_init_output(&os, out, out_nbytes_avail);
		if (in_nbytes == 0)
			in = &os; /* Avoid passing NULL to memcpy(). */
		deflate_write_uncompressed_block(&os, in, in_nbytes, true);
		return deflate_flush_output(&os);
	}

	return (*c->impl)(c, in, in_nbytes, out, out_nbytes_avail);
}

LIBDEFLATEEXPORT void LIBDEFLATEAPI
libdeflate_free_compressor(struct libdeflate_compressor *c)
{
	libdeflate_aligned_free(c);
}

unsigned int
deflate_get_compression_level(struct libdeflate_compressor *c)
{
	return c->compression_level;
}

LIBDEFLATEEXPORT size_t LIBDEFLATEAPI
libdeflate_deflate_compress_bound(struct libdeflate_compressor *c,
				  size_t in_nbytes)
{
	/*
	 * The worst case is all uncompressed blocks where one block has length
	 * <= MIN_BLOCK_LENGTH and the others have length MIN_BLOCK_LENGTH.
	 * Each uncompressed block has 5 bytes of overhead: 1 for BFINAL, BTYPE,
	 * and alignment to a byte boundary; 2 for LEN; and 2 for NLEN.
	 */
	size_t max_num_blocks =
		MAX(DIV_ROUND_UP(in_nbytes, MIN_BLOCK_LENGTH), 1);

	return (5 * max_num_blocks) + in_nbytes + 1 + OUTPUT_END_PADDING;
}
