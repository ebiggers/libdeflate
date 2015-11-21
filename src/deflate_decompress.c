/*
 * deflate_decompress.c - a decompressor for DEFLATE
 *
 * Author:	Eric Biggers
 * Year:	2014, 2015
 *
 * The author dedicates this file to the public domain.
 * You can do whatever you want with this file.
 *
 * ---------------------------------------------------------------------------
 *
 * This is a highly optimized DEFLATE decompressor.  On x86_64 it decompresses
 * data in about 59% of the time of zlib.  On other architectures it should
 * still be significantly faster than zlib, but the difference may be smaller.
 *
 * This decompressor currently only supports raw DEFLATE (not zlib or gzip), and
 * it only supports whole-buffer decompression (not streaming).
 *
 * Why this is faster than zlib's implementation:
 *
 * - Word accesses rather than byte accesses when reading input
 * - Word accesses rather than byte accesses when copying matches
 * - Faster Huffman decoding combined with various DEFLATE-specific tricks
 * - Larger bitbuffer variable that doesn't need to be filled as often
 * - Other optimizations to remove unnecessary branches
 * - Only full-buffer decompression is supported, so the code doesn't need to
 *   support stopping and resuming decompression.
 */

#include <stdlib.h>
#include <string.h>

#include "libdeflate.h"

#include "deflate_constants.h"
#include "unaligned.h"

#ifndef UNSAFE_DECOMPRESSION
#  define UNSAFE_DECOMPRESSION 0
#endif

#if UNSAFE_DECOMPRESSION
#  warning "unsafe decompression is enabled"
#  define SAFETY_CHECK(expr) 0
#else
#  define SAFETY_CHECK(expr) unlikely(expr)
#endif

/*
 * Each of these values is the base 2 logarithm of the number of entries of the
 * corresponding decode table.  Each value should be large enough to ensure that
 * for typical data, the vast majority of symbols can be decoded by a direct
 * lookup of the next TABLEBITS bits of compressed data.  However, this must be
 * balanced against the fact that a larger table requires more memory and
 * requires more time to fill.
 */
#define DEFLATE_PRECODE_TABLEBITS	7
#define DEFLATE_LITLEN_TABLEBITS	10
#define DEFLATE_OFFSET_TABLEBITS	9

/*
 * Type for codeword lengths.
 */
typedef u8 len_t;

/*
 * The main DEFLATE decompressor structure.  Since this implementation only
 * supports full buffer decompression, this structure does not store the entire
 * decompression state, but rather only some arrays that are too large to
 * comfortably allocate on the stack.
 */
struct deflate_decompressor {

	/*
	 * The arrays aren't all needed at the same time.  'precode_lens' and
	 * 'precode_decode_table' are unneeded after 'lens' has been filled.
	 * Furthermore, 'lens' need not be retained after building the litlen
	 * and offset decode tables.  In fact, 'lens' can be in union with
	 * 'litlen_decode_table' provided that 'offset_decode_table' is separate
	 * and is built first.
	 */

	union {
		len_t precode_lens[DEFLATE_NUM_PRECODE_SYMS];

		struct {
			len_t lens[DEFLATE_NUM_LITLEN_SYMS +
				   DEFLATE_NUM_OFFSET_SYMS +
				   DEFLATE_MAX_LENS_OVERRUN];

			u32 precode_decode_table[(1 << DEFLATE_PRECODE_TABLEBITS) +
						 (2 * DEFLATE_NUM_PRECODE_SYMS)];
		};

		u32 litlen_decode_table[(1 << DEFLATE_LITLEN_TABLEBITS) +
					(2 * DEFLATE_NUM_LITLEN_SYMS)];
	};

	u32 offset_decode_table[(1 << DEFLATE_OFFSET_TABLEBITS) +
				(2 * DEFLATE_NUM_OFFSET_SYMS)];

	u16 working_space[2 * (DEFLATE_MAX_CODEWORD_LEN + 1) +
			  DEFLATE_MAX_NUM_SYMS];
};

/*****************************************************************************
 *				Input bitstream                              *
 *****************************************************************************/

/*
 * The state of the "input bitstream" consists of the following variables:
 *
 *	- in_next: pointer to the next unread byte in the input buffer
 *
 *	- in_end: pointer just past the end of the input buffer
 *
 *	- bitbuf: a word-sized variable containing bits that have been read from
 *		  the input buffer.  The buffered bits are right-aligned
 *		  (they're the low-order bits).
 *
 *	- bitsleft: number of bits in 'bitbuf' that are valid.
 *
 * To make it easier for the compiler to optimize the code by keeping variables
 * in registers, these are declared as normal variables and manipulated using
 * macros.
 */

/*
 * The type for the bitbuffer variable ('bitbuf' described above).  For best
 * performance, this should have size equal to a machine word.
 */
typedef machine_word_t bitbuf_t;

/*
 * Number of bits the bitbuffer variable can hold.
 */
#define BITBUF_NBITS	(8 * sizeof(bitbuf_t))

/*
 * The maximum number of bits that can be requested to be in the bitbuffer
 * variable.  This is the maximum value of 'n' that can be passed
 * ENSURE_BITS(n).
 *
 * This not equal to BITBUF_NBITS because we never read less than one byte at a
 * time.  If the bitbuffer variable contains more than (BITBUF_NBITS - 8) bits,
 * then we can't read another byte without first consuming some bits.  So the
 * maximum count we can ensure is (BITBUF_NBITS - 7).
 */
#define MAX_ENSURE	(BITBUF_NBITS - 7)

/*
 * Evaluates to true if 'n' is a valid argument to ENSURE_BITS(n), or false if
 * 'n' is too large to be passed to ENSURE_BITS(n).  Note: if 'n' is a compile
 * time constant, then this expression will be a compile-type constant.
 * Therefore, CAN_ENSURE() can be used choose between alternative
 * implementations at compile time.
 */
#define CAN_ENSURE(n)	((n) <= MAX_ENSURE)

/*
 * Fill the bitbuffer variable, reading one byte at a time.
 *
 * Note: if we would overrun the input buffer, we just don't read anything,
 * leaving the bits as 0 but marking them as filled.  This makes the
 * implementation simpler because this removes the need to distinguish between
 * "real" overruns and overruns that occur because of our own lookahead during
 * Huffman decoding.  The disadvantage is that a "real" overrun can go
 * undetected, and deflate_decompress() may return a success status rather than
 * the expected failure status if one occurs.  However, this is irrelevant
 * because even if this specific case were to be handled "correctly", one could
 * easily come up with a different case where the compressed data would be
 * corrupted in such a way that fully retains its validity.  Users should run a
 * checksum against the uncompressed data if they wish to detect corruptions.
 */
#define FILL_BITS_BYTEWISE()						\
({									\
	do {								\
		if (likely(in_next != in_end))				\
			bitbuf |= (bitbuf_t)*in_next++ << bitsleft;	\
		else							\
			overrun_count++;				\
		bitsleft += 8;						\
	} while (bitsleft <= BITBUF_NBITS - 8);				\
})

/*
 * Fill the bitbuffer variable by reading the next word from the input buffer.
 * This can be significantly faster than FILL_BITS_BYTEWISE().  However, for
 * this to work correctly, the word must be interpreted in little-endian format.
 * In addition, the memory access may be unaligned.  Therefore, this method is
 * most efficient on little-endian architectures that support fast unaligned
 * access, such as x86 and x86_64.
 */
#define FILL_BITS_WORDWISE()						\
({									\
	bitbuf |= load_leword_unaligned(in_next) << bitsleft;		\
	in_next += (BITBUF_NBITS - bitsleft) >> 3;			\
	bitsleft += (BITBUF_NBITS - bitsleft) & ~7;			\
})

/*
 * Does the bitbuffer variable currently contain at least 'n' bits?
 */
#define HAVE_BITS(n) (bitsleft >= (n))

/*
 * Raw form of ENSURE_BITS(): the bitbuffer variable must not already contain
 * the requested number of bits.
 */
#define DO_ENSURE_BITS(n)					\
({								\
	if (CPU_IS_LITTLE_ENDIAN() &&				\
	    UNALIGNED_ACCESS_IS_FAST &&				\
	    likely(in_end - in_next >= sizeof(bitbuf_t)))	\
		FILL_BITS_WORDWISE();				\
	else							\
		FILL_BITS_BYTEWISE();				\
})

/*
 * Load more bits from the input buffer until the specified number of bits is
 * present in the bitbuffer variable.  'n' cannot be too large; see MAX_ENSURE
 * and CAN_ENSURE().
 */
#define ENSURE_BITS(n)							\
({									\
	if (!HAVE_BITS(n))						\
		DO_ENSURE_BITS(n);					\
})

/*
 * Return the next 'n' bits from the bitbuffer variable without removing them.
 */
#define BITS(n)								\
({									\
	(u32)bitbuf & (((u32)1 << (n)) - 1);				\
})

/*
 * Remove the next 'n' bits from the bitbuffer variable.
 */
#define REMOVE_BITS(n)							\
({									\
	bitbuf >>= (n);							\
	bitsleft -= (n);						\
})

/*
 * Remove and return the next 'n' bits from the bitbuffer variable.
 */
#define POP_BITS(n)							\
({									\
	u32 bits = BITS(n);						\
	REMOVE_BITS(n);							\
	bits;								\
})

/*
 * Align the input to the next byte boundary, discarding any remaining bits in
 * the current byte.
 *
 * Note that if the bitbuffer variable currently contains more than 8 bits, then
 * we must rewind 'in_next', effectively putting those bits back.  Only the bits
 * in what would be the "current" byte if we were reading one byte at a time can
 * be actually discarded.
 */
#define ALIGN_INPUT()							\
({									\
	in_next -= (bitsleft >> 3) - MIN(overrun_count, bitsleft >> 3);	\
	bitbuf = 0;							\
	bitsleft = 0;							\
})

/*
 * Read a 16-bit value from the input.  This must have been preceded by a call
 * to ALIGN_INPUT(), and the caller must have already checked for overrun.
 */
#define READ_U16()							\
({									\
	u16 v;								\
									\
	v = get_unaligned_le16(in_next);				\
	in_next += 2;							\
	v;								\
})

/*****************************************************************************
 *                              Huffman decoding                             *
 *****************************************************************************/

/*
 * A decode table for order TABLEBITS contains (1 << TABLEBITS) entries, plus
 * additional entries for non-root binary tree nodes.  The number of non-root
 * binary tree nodes is variable, but cannot possibly be more than twice the
 * number of symbols in the alphabet for which the decode table is built.
 *
 * The decoding algorithm takes the next TABLEBITS bits of compressed data and
 * uses them as an index into the decode table.  The resulting entry is either a
 * "direct entry", meaning that it contains the value desired, or a "tree root
 * entry", meaning that it is the root of a binary tree that must be traversed
 * using more bits of the compressed data (0 bit means go to the left child, 1
 * bit means go to the right child) until a leaf is reached.
 *
 * Each decode table is associated with a Huffman code.  Logically, the result
 * of a decode table lookup is a symbol from the alphabet from which the
 * corresponding Huffman code was constructed.  A symbol with codeword length n
 * <= TABLEBITS is associated with 2**(TABLEBITS - n) direct entries in the
 * table, whereas a symbol with codeword length n > TABLEBITS shares a binary
 * tree with a number of other codewords.
 *
 * On top of this basic design, we implement several optimizations:
 *
 * - We store the length of each codeword directly in each of its decode table
 *   entries.  This allows the codeword length to be produced without indexing
 *   an additional table.
 *
 * - When beneficial, we don't store the Huffman symbol itself, but instead data
 *   generated from it.  For example, when decoding an offset symbol in DEFLATE,
 *   it's more efficient if we can decode the offset base and number of extra
 *   offset bits directly rather than decoding the offset symbol and then
 *   looking up both of those values in an additional table or tables.
 *
 * - It can be possible to decode more than just a single Huffman symbol from
 *   the next TABLEBITS bits of the input.  We take advantage of this when
 *   decoding match lengths.  When possible, the decode table entry will provide
 *   the full match length.  In this case, the stored "codeword length" will
 *   actually be the codeword length plus the number of extra length bits that
 *   are being consumed.
 *
 * The size of each decode table entry is 32 bits, which provides slightly
 * better performance than 16-bit entries on 32 and 64 bit processers, provided
 * that the table doesn't get so large that it takes up too much memory and
 * starts generating cache misses.  The bits of each decode table entry are
 * defined as follows:
 *
 * - Bits 29 -- 31: flags (see below)
 * - Bits 25 -- 28: codeword length
 * - Bits 0 -- 24: decode result: a Huffman symbol or related data
 */

/*
 * Flags usage:
 *
 * The precode and offset tables only use these flags to distinguish nonleaf
 * tree nodes from other entries.  In nonleaf tree node entries, all flags are
 * set and the recommended one to test is HUFFDEC_TREE_NONLEAF_FAST_FLAG.
 *
 * The literal/length decode table uses all the flags.  During decoding, the
 * flags are designed to be tested from high to low.  If a flag is set, then all
 * higher flags are also set.
 */

/*
 * This flag is set in all entries that do not represent a literal symbol,
 * excluding tree leaves.  This enables a very fast path for non-rare literals:
 * just check if this bit is clear, and if so extract the literal from the low
 * bits.
 */
#define HUFFDEC_NOT_LITERAL		0x80000000

/*
 * This flag is set in all entries that represent neither a literal symbol nor a
 * full match length, excluding tree leaves.
 */
#define HUFFDEC_NOT_FULL_LENGTH		0x40000000

/*
 * This flag is set in all nonleaf tree entries (roots and internal nodes).
 */
#define HUFFDEC_TREE_NONLEAF		0x20000000

/*
 * HUFFDEC_TREE_NONLEAF implies that the following flags are also set.
 */
#define HUFFDEC_TREE_NONLEAF_FLAGS	0xE0000000

/*
 * For distinguishing between any direct entry and a tree root, or between an
 * internal tree node and a leaf node, this bit should be checked in preference
 * to any other in HUFFDEC_TREE_NONLEAF_FLAGS --- the reason being this is the
 * sign bit, and some architectures have special instructions to handle it.
 */
#define HUFFDEC_TREE_NONLEAF_FAST_FLAG	0x80000000

/*
 * Number of flag bits defined above.
 */
#define HUFFDEC_NUM_FLAG_BITS	3

/*
 * Number of bits reserved for the codeword length in decode table entries, and
 * the corresponding mask and limit.  4 bits provides a max length of 15, which
 * is enough for any DEFLATE codeword.  (And actually, we don't even need the
 * full 15 because only lengths less than or equal to the appropriate TABLEBITS
 * will ever be stored in this field.)
 */
#define HUFFDEC_LEN_BITS	4
#define HUFFDEC_LEN_MASK	(((u32)1 << HUFFDEC_LEN_BITS) - 1)
#define HUFFDEC_MAX_LEN		HUFFDEC_LEN_MASK

/*
 * Value by which a decode table entry can be right-shifted to get the length
 * field.  Note: the result must be AND-ed with HUFFDEC_LEN_MASK unless it is
 * guaranteed that no flag bits are set.
 */
#define HUFFDEC_LEN_SHIFT	(32 - HUFFDEC_NUM_FLAG_BITS - HUFFDEC_LEN_BITS)

/*
 * Mask to get the "value" of a decode table entry.  This is the decode result
 * and contains data dependent on the table.
 */
#define HUFFDEC_VALUE_MASK	(((u32)1 << HUFFDEC_LEN_SHIFT) - 1)

/*
 * Data needed to initialize the entries in the length/literal decode table.
 */
static const u32 deflate_litlen_symbol_data[DEFLATE_NUM_LITLEN_SYMS] = {
	/* Literals  */
	0   , 1   , 2   , 3   , 4   , 5   , 6   , 7   ,
	8   , 9   , 10  , 11  , 12  , 13  , 14  , 15  ,
	16  , 17  , 18  , 19  , 20  , 21  , 22  , 23  ,
	24  , 25  , 26  , 27  , 28  , 29  , 30  , 31  ,
	32  , 33  , 34  , 35  , 36  , 37  , 38  , 39  ,
	40  , 41  , 42  , 43  , 44  , 45  , 46  , 47  ,
	48  , 49  , 50  , 51  , 52  , 53  , 54  , 55  ,
	56  , 57  , 58  , 59  , 60  , 61  , 62  , 63  ,
	64  , 65  , 66  , 67  , 68  , 69  , 70  , 71  ,
	72  , 73  , 74  , 75  , 76  , 77  , 78  , 79  ,
	80  , 81  , 82  , 83  , 84  , 85  , 86  , 87  ,
	88  , 89  , 90  , 91  , 92  , 93  , 94  , 95  ,
	96  , 97  , 98  , 99  , 100 , 101 , 102 , 103 ,
	104 , 105 , 106 , 107 , 108 , 109 , 110 , 111 ,
	112 , 113 , 114 , 115 , 116 , 117 , 118 , 119 ,
	120 , 121 , 122 , 123 , 124 , 125 , 126 , 127 ,
	128 , 129 , 130 , 131 , 132 , 133 , 134 , 135 ,
	136 , 137 , 138 , 139 , 140 , 141 , 142 , 143 ,
	144 , 145 , 146 , 147 , 148 , 149 , 150 , 151 ,
	152 , 153 , 154 , 155 , 156 , 157 , 158 , 159 ,
	160 , 161 , 162 , 163 , 164 , 165 , 166 , 167 ,
	168 , 169 , 170 , 171 , 172 , 173 , 174 , 175 ,
	176 , 177 , 178 , 179 , 180 , 181 , 182 , 183 ,
	184 , 185 , 186 , 187 , 188 , 189 , 190 , 191 ,
	192 , 193 , 194 , 195 , 196 , 197 , 198 , 199 ,
	200 , 201 , 202 , 203 , 204 , 205 , 206 , 207 ,
	208 , 209 , 210 , 211 , 212 , 213 , 214 , 215 ,
	216 , 217 , 218 , 219 , 220 , 221 , 222 , 223 ,
	224 , 225 , 226 , 227 , 228 , 229 , 230 , 231 ,
	232 , 233 , 234 , 235 , 236 , 237 , 238 , 239 ,
	240 , 241 , 242 , 243 , 244 , 245 , 246 , 247 ,
	248 , 249 , 250 , 251 , 252 , 253 , 254 , 255 ,

#define HUFFDEC_NUM_BITS_FOR_EXTRA_LENGTH_BITS	3
#define HUFFDEC_MAX_EXTRA_LENGTH_BITS	(((u32)1 << HUFFDEC_NUM_BITS_FOR_EXTRA_LENGTH_BITS) - 1)
#define HUFFDEC_EXTRA_LENGTH_BITS_SHIFT (HUFFDEC_LEN_SHIFT - HUFFDEC_NUM_BITS_FOR_EXTRA_LENGTH_BITS)
#define HUFFDEC_LENGTH_BASE_MASK	(((u32)1 << HUFFDEC_EXTRA_LENGTH_BITS_SHIFT) - 1)
#define HUFFDEC_END_OF_BLOCK_LENGTH	0

#define ENTRY(length_base, num_extra_bits) \
	(256 + (length_base) + ((num_extra_bits) << HUFFDEC_EXTRA_LENGTH_BITS_SHIFT))

	/* End of block  */
	ENTRY(HUFFDEC_END_OF_BLOCK_LENGTH, 0),

	/* Match length data  */
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

/*
 * Data needed to initialize the entries in the offset decode table.
 */
static const u32 deflate_offset_symbol_data[DEFLATE_NUM_OFFSET_SYMS] = {

#define HUFFDEC_EXTRA_OFFSET_BITS_SHIFT 16
#define HUFFDEC_OFFSET_BASE_MASK (((u32)1 << HUFFDEC_EXTRA_OFFSET_BITS_SHIFT) - 1)

#define ENTRY(offset_base, num_extra_bits) \
	((offset_base) | ((u32)(num_extra_bits) << HUFFDEC_EXTRA_OFFSET_BITS_SHIFT))
	ENTRY(1     , 0)  , ENTRY(2     , 0)  , ENTRY(3     , 0)  , ENTRY(4     , 0)  ,
	ENTRY(5     , 1)  , ENTRY(7     , 1)  , ENTRY(9     , 2)  , ENTRY(13    , 2) ,
	ENTRY(17    , 3)  , ENTRY(25    , 3)  , ENTRY(33    , 4)  , ENTRY(49    , 4)  ,
	ENTRY(65    , 5)  , ENTRY(97    , 5)  , ENTRY(129   , 6)  , ENTRY(193   , 6)  ,
	ENTRY(257   , 7)  , ENTRY(385   , 7)  , ENTRY(513   , 8)  , ENTRY(769   , 8)  ,
	ENTRY(1025  , 9)  , ENTRY(1537  , 9)  , ENTRY(2049  , 10) , ENTRY(3073  , 10) ,
	ENTRY(4097  , 11) , ENTRY(6145  , 11) , ENTRY(8193  , 12) , ENTRY(12289 , 12) ,
	ENTRY(16385 , 13) , ENTRY(24577 , 13) , ENTRY(32769 , 14) , ENTRY(49153 , 14) ,
#undef ENTRY
};

/* Construct a direct decode table entry (not a tree node)  */
static forceinline u32
make_direct_entry(u32 value, u32 length)
{
	return (length << HUFFDEC_LEN_SHIFT) | value;
}

/*
 * The following functions define the way entries are created for each decode
 * table.  Note that these will all be inlined into build_decode_table(), which
 * will itself be inlined for each decode table.  This is important for
 * performance because the make_*_entry() functions get called from the inner
 * loop of build_decode_table().
 */

static forceinline u32
make_litlen_direct_entry(unsigned symbol, unsigned codeword_length,
			 unsigned *extra_mask_ret)
{
	u32 entry_value = deflate_litlen_symbol_data[symbol];
	u32 entry_length = codeword_length;
	unsigned length_bits;
	u32 length_base;

	STATIC_ASSERT(DEFLATE_MAX_EXTRA_LENGTH_BITS <=
		      HUFFDEC_MAX_EXTRA_LENGTH_BITS);

	if (symbol >= 256) {
		/* Match, not a literal.  (This can also be the special
		 * end-of-block symbol, which we handle identically.)  */
		entry_value -= 256;
		length_bits = entry_value >> HUFFDEC_EXTRA_LENGTH_BITS_SHIFT;
		length_base = entry_value & HUFFDEC_LENGTH_BASE_MASK;
		if (codeword_length + length_bits <= DEFLATE_LITLEN_TABLEBITS) {
			/* TABLEBITS is enough to decode the length slot as well
			 * as all the extra length bits.  So store the full
			 * length in the decode table entry.
			 *
			 * Note that a length slot may be used for multiple
			 * lengths, and multiple decode table entries may map to
			 * the same length; hence the need for the 'extra_mask',
			 * which allows build_decode_table() to cycle through
			 * the lengths that use this length slot.  */
			entry_value = length_base;
			entry_length += length_bits;
			*extra_mask_ret = (1U << length_bits) - 1;
		} else {
			/* TABLEBITS isn't enough to decode all the extra length
			 * bits.  The decoder will have to decode the extra bits
			 * separately.  This is the less common case.  */
			entry_value |= HUFFDEC_NOT_FULL_LENGTH;
		}
		entry_value |= HUFFDEC_NOT_LITERAL;
	}

	return make_direct_entry(entry_value, entry_length);
}

static forceinline u32
make_litlen_leaf_entry(unsigned sym)
{
	return deflate_litlen_symbol_data[sym];
}

static forceinline u32
make_offset_direct_entry(unsigned sym, unsigned codeword_len, unsigned *extra_mask_ret)
{
	return make_direct_entry(deflate_offset_symbol_data[sym], codeword_len);
}

static forceinline u32
make_offset_leaf_entry(unsigned sym)
{
	return deflate_offset_symbol_data[sym];
}

static forceinline u32
make_pre_direct_entry(unsigned sym, unsigned codeword_len, unsigned *extra_mask_ret)
{
	return make_direct_entry(sym, codeword_len);
}

static forceinline u32
make_pre_leaf_entry(unsigned sym)
{
	return sym;
}

/*
 * Build a table for fast Huffman decoding, using bit-reversed codewords.
 *
 * The Huffman code is assumed to be in canonical form and is specified by its
 * codeword lengths only.
 *
 * @decode_table
 *	A table with ((1 << table_bits) + (2 * num_syms)) entries.  The format
 *	of the table has been described in previous comments.
 * @lens
 *	Lengths of the Huffman codewords.  'lens[sym]' specifies the length, in
 *	bits, of the codeword for symbol 'sym'.  If a symbol is not used in the
 *	code, its length must be specified as 0.  It is valid for this parameter
 *	to alias @decode_table because nothing gets written to @decode_table
 *	until all information in @lens has been consumed.
 * @num_syms
 *	Number of symbols in the code.
 * @make_direct_entry
 *	Function to create a direct decode table entry, given the symbol and
 *	codeword length.
 * @make_leaf_entry
 *	Function to create a tree decode table entry, at a tree leaf, given the
 *	symbol.
 * @table_bits
 *	log base 2 of the size of the direct lookup portion of the decode table.
 * @max_codeword_len
 *	Maximum allowed codeword length for this Huffman code.
 * @working_space
 *	A temporary array of length '2 * (@max_codeword_len + 1) + @num_syms'.
 *
 * Returns %true if successful; %false if the codeword lengths do not form a
 * valid Huffman code.
 */
static forceinline bool
build_decode_table(u32 decode_table[],
		   const len_t lens[],
		   const unsigned num_syms,
		   u32 (*make_direct_entry)(unsigned, unsigned, unsigned *),
		   u32 (*make_leaf_entry)(unsigned),
		   const unsigned table_bits,
		   const unsigned max_codeword_len,
		   u16 working_space[])
{
	u16 * const len_counts = &working_space[0];
	u16 * const offsets = &working_space[1 * (max_codeword_len + 1)];
	u16 * const sorted_syms = &working_space[2 * (max_codeword_len + 1)];
	unsigned sym;
	unsigned len;
	s32 remainder;
	unsigned sym_idx;
	unsigned codeword_reversed;
	unsigned codeword_len;
	unsigned loop_count;

	/* Count how many symbols have each codeword length.  */
	for (len = 0; len <= max_codeword_len; len++)
		len_counts[len] = 0;
	for (sym = 0; sym < num_syms; sym++)
		len_counts[lens[sym]]++;

	/* We guarantee that all lengths are <= max_codeword_len, but we cannot
	 * assume they form a valid prefix code.  A codeword of length n should
	 * require a proportion of the codespace equaling (1/2)^n.  The code is
	 * valid if and only if the codespace is exactly filled by the lengths
	 * by this measure.  */
	remainder = 1;
	for (len = 1; len <= max_codeword_len; len++) {
		remainder <<= 1;
		remainder -= len_counts[len];
		if (unlikely(remainder < 0)) {
			/* The lengths overflow the codespace; that is, the code
			 * is over-subscribed.  */
			return false;
		}
	}

	if (unlikely(remainder != 0)) {
		/* The lengths do not fill the codespace; that is, they form an
		 * incomplete set.  */
		if (remainder == (1U << max_codeword_len)) {
			/* The code is completely empty.  By definition, no
			 * symbols can be decoded with an empty code.
			 * Consequently, we technically don't even need to fill
			 * in the decode table.  However, to avoid accessing
			 * uninitialized memory if the algorithm nevertheless
			 * attempts to decode symbols using such a code, we fill
			 * the decode table with default values.  */
			unsigned dummy;
			for (unsigned i = 0; i < (1U << table_bits); i++)
				decode_table[i] = (*make_direct_entry)(0, 1, &dummy);
			return true;
		}
		return false;
	}

	/* Sort the symbols primarily by length and secondarily by symbol value.
	 */

	/* Initialize 'offsets' so that offsets[len] is the number of codewords
	 * shorter than 'len' bits.  */
	offsets[0] = 0;
	for (len = 0; len < max_codeword_len; len++)
		offsets[len + 1] = offsets[len] + len_counts[len];

	/* Use the 'offsets' array to sort the symbols.  */
	for (sym = 0; sym < num_syms; sym++)
		sorted_syms[offsets[lens[sym]]++] = sym;

	/* Generate entries for codewords with length <= 'table_bits'.
	 * Start with codeword length 1 and proceed to longer codewords.  */
	sym_idx = offsets[0];
	codeword_reversed = 0;
	codeword_len = 1;
	loop_count = (1U << (table_bits - codeword_len));
	for (; loop_count != 0; codeword_len++, loop_count >>= 1) {

		const unsigned end_sym_idx = sym_idx + len_counts[codeword_len];
		const unsigned increment = 1U << codeword_len;

		/* Iterate through the symbols that have codewords with length
		 * 'codeword_len'.  Since the code is assumed to be canonical,
		 * we can generate the codewords by iterating in symbol order
		 * and incrementing the current codeword by 1 each time.  */

		for (; sym_idx < end_sym_idx; sym_idx++) {
			unsigned sym;
			u32 entry;
			unsigned extra_mask;
			unsigned extra;
			unsigned i;
			unsigned n;
			unsigned bit;

			sym = sorted_syms[sym_idx];
			extra_mask = 0;
			entry = (*make_direct_entry)(sym, codeword_len, &extra_mask);
			extra = 0;
			i = codeword_reversed;
			n = loop_count;
			do {
				decode_table[i] = entry + extra;
				extra = (extra + 1) & extra_mask;
				i += increment;
			} while (--n);

			/* Increment the codeword by 1.  Since DEFLATE requires
			 * bit-reversed codewords, we must manipulate bits
			 * ourselves.  */
			bit = 1U << (codeword_len - 1);
			while (codeword_reversed & bit)
				bit >>= 1;
			codeword_reversed &= bit - 1;
			codeword_reversed |= bit;
		}
	}

	/* If we've filled in the entire table, we are done.  Otherwise, there
	 * are codewords longer than 'table_bits' for which we must generate
	 * binary trees.  */
	if (max_codeword_len > table_bits &&
	    offsets[table_bits] != offsets[max_codeword_len])
	{
		unsigned i;
		unsigned bit;
		unsigned next_free_slot;

		/* First, zero out the remaining entries.  This is necessary so
		 * that those entries appear as "unallocated" in the next part.
		 * Each of these entries will eventually be filled with the
		 * representation of the root node of a binary tree.  */

		i = (1U << table_bits) - 1; /* All 1 bits */
		for (;;) {
			decode_table[i] = 0;

			if (i == codeword_reversed)
				break;

			/* Subtract 1 from the bit-reversed index.  */
			bit = 1U << table_bits;
			do {
				bit >>= 1;
				i ^= bit;
			} while (i & bit);
		}

		/* We allocate child nodes starting at the end of the direct
		 * lookup table.  Note that there should be 2*num_syms extra
		 * entries for this purpose, although fewer than this may
		 * actually be needed.  */
		next_free_slot = 1U << table_bits;

		for (; codeword_len <= max_codeword_len; codeword_len++) {

			const unsigned end_sym_idx = sym_idx + len_counts[codeword_len];

			for (; sym_idx < end_sym_idx; sym_idx++) {

				unsigned shift = table_bits;
				unsigned node_idx = codeword_reversed & ((1U << table_bits) - 1);

				/* Go through each bit of the current codeword
				 * beyond the prefix of length @table_bits and
				 * walk the appropriate binary tree, allocating
				 * any slots that have not yet been allocated.
				 *
				 * Note that the 'pointer' entry to the binary
				 * tree, which is stored in the direct lookup
				 * portion of the table, is represented
				 * identically to other internal (non-leaf)
				 * nodes of the binary tree; it can be thought
				 * of as simply the root of the tree.  The
				 * representation of these internal nodes is
				 * simply the index of the left child combined
				 * with special flags to distingush the entry
				 * from direct mapping and leaf node entries.
				 */
				do {

					/* At least one bit remains in the
					 * codeword, but the current node is
					 * unallocated.  Allocate it as an
					 * internal tree node.  */
					if (decode_table[node_idx] == 0) {
						decode_table[node_idx] =
							next_free_slot |
							HUFFDEC_TREE_NONLEAF_FLAGS;
						decode_table[next_free_slot++] = 0;
						decode_table[next_free_slot++] = 0;
					}

					/* Go to the left child if the next bit
					 * in the codeword is 0; otherwise go to
					 * the right child.  */
					node_idx = decode_table[node_idx] &
						   ~HUFFDEC_TREE_NONLEAF_FLAGS;
					node_idx += (codeword_reversed >> shift) & 1;
					shift += 1;
				} while (shift != codeword_len);

				/* Generate the leaf node, which contains the
				 * real decode table entry.  */
				decode_table[node_idx] =
					(*make_leaf_entry)(sorted_syms[sym_idx]);

				/* Increment the codeword by 1.  Since DEFLATE
				 * requires bit-reversed codewords, we must
				 * manipulate bits ourselves.  */
				bit = 1U << (codeword_len - 1);
				while (codeword_reversed & bit)
					bit >>= 1;
				codeword_reversed &= bit - 1;
				codeword_reversed |= bit;
			}
		}
	}
	return true;
}

/* Build the decode table for the precode.  */
static bool
build_precode_decode_table(struct deflate_decompressor *d)
{
	return build_decode_table(d->precode_decode_table,
				  d->precode_lens,
				  DEFLATE_NUM_PRECODE_SYMS,
				  make_pre_direct_entry,
				  make_pre_leaf_entry,
				  DEFLATE_PRECODE_TABLEBITS,
				  DEFLATE_MAX_PRE_CODEWORD_LEN,
				  d->working_space);
}

/* Build the decode table for the literal/length code.  */
static bool
build_litlen_decode_table(struct deflate_decompressor *d,
			  unsigned num_litlen_syms, unsigned num_offset_syms)
{
	return build_decode_table(d->litlen_decode_table,
				  d->lens,
				  num_litlen_syms,
				  make_litlen_direct_entry,
				  make_litlen_leaf_entry,
				  DEFLATE_LITLEN_TABLEBITS,
				  DEFLATE_MAX_LITLEN_CODEWORD_LEN,
				  d->working_space);
}

/* Build the decode table for the offset code.  */
static bool
build_offset_decode_table(struct deflate_decompressor *d,
			  unsigned num_litlen_syms, unsigned num_offset_syms)
{
	return build_decode_table(d->offset_decode_table,
				  d->lens + num_litlen_syms,
				  num_offset_syms,
				  make_offset_direct_entry,
				  make_offset_leaf_entry,
				  DEFLATE_OFFSET_TABLEBITS,
				  DEFLATE_MAX_OFFSET_CODEWORD_LEN,
				  d->working_space);
}

static forceinline machine_word_t
repeat_byte(u8 b)
{
	machine_word_t v;

	STATIC_ASSERT(WORDSIZE == 4 || WORDSIZE == 8);

	v = b;
	v |= v << 8;
	v |= v << 16;
	v |= v << ((WORDSIZE == 8) ? 32 : 0);
	return v;
}

static forceinline void
copy_word_unaligned(const void *src, void *dst)
{
	store_word_unaligned(load_word_unaligned(src), dst);
}

/*
 * Copy an LZ77 match at (dst - offset) to dst.
 *
 * The length and offset must be already validated --- that is, (dst - offset)
 * can't underrun the output buffer, and (dst + length) can't overrun the output
 * buffer.  Also, the length cannot be 0.
 *
 * @winend points to the byte past the end of the output buffer.
 * This function won't write any data beyond this position.
 */
static forceinline void
lz_copy(u8 *dst, u32 length, u32 offset, const u8 *winend)
{
	const u8 *src = dst - offset;
	const u8 * const end = dst + length;

	/*
	 * Try to copy one machine word at a time.  On i386 and x86_64 this is
	 * faster than copying one byte at a time, unless the data is
	 * near-random and all the matches have very short lengths.  Note that
	 * since this requires unaligned memory accesses, it won't necessarily
	 * be faster on every architecture.
	 *
	 * Also note that we might copy more than the length of the match.  For
	 * example, if a word is 8 bytes and the match is of length 5, then
	 * we'll simply copy 8 bytes.  This is okay as long as we don't write
	 * beyond the end of the output buffer, hence the check for (winend -
	 * end >= WORDSIZE - 1).
	 */
	if (UNALIGNED_ACCESS_IS_FAST && likely(winend - end >= WORDSIZE - 1)) {

		if (offset >= WORDSIZE) {
			/* The source and destination words don't overlap.  */

			/* To improve branch prediction, one iteration of this
			 * loop is unrolled.  Most matches are short and will
			 * fail the first check.  But if that check passes, then
			 * it becomes increasing likely that the match is long
			 * and we'll need to continue copying.  */

			copy_word_unaligned(src, dst);
			src += WORDSIZE;
			dst += WORDSIZE;

			if (dst < end) {
				do {
					copy_word_unaligned(src, dst);
					src += WORDSIZE;
					dst += WORDSIZE;
				} while (dst < end);
			}
			return;
		} else if (offset == 1) {

			/* Offset 1 matches are equivalent to run-length
			 * encoding of the previous byte.  This case is common
			 * if the data contains many repeated bytes.  */

			machine_word_t v = repeat_byte(*(dst - 1));
			do {
				store_word_unaligned(v, dst);
				src += WORDSIZE;
				dst += WORDSIZE;
			} while (dst < end);
			return;
		}
		/*
		 * We don't bother with special cases for other 'offset <
		 * WORDSIZE', which are usually rarer than 'offset == 1'.  Extra
		 * checks will just slow things down.  Actually, it's possible
		 * to handle all the 'offset < WORDSIZE' cases using the same
		 * code, but it still becomes more complicated doesn't seem any
		 * faster overall; it definitely slows down the more common
		 * 'offset == 1' case.
		 */
	}

	/* Fall back to a bytewise copy.  */
	STATIC_ASSERT(DEFLATE_MIN_MATCH_LEN == 3);
	*dst++ = *src++;
	*dst++ = *src++;
	length -= 2;
	do {
		*dst++ = *src++;
	} while (--length);
}

/*****************************************************************************
 *                         Main decompression routine
 *****************************************************************************/

/*
 * This is the main DEFLATE decompression routine.  It decompresses 'in_nbytes'
 * bytes of compressed data from the buffer 'in' and writes the uncompressed
 * data to the buffer 'out'.  The caller must know the exact length of the
 * uncompressed data and pass it as 'out_nbytes'.  The return value is %true if
 * and only if decompression was successful.  A return value of %false indicates
 * that either the compressed data is invalid or it does not decompress to
 * exactly 'out_nbytes' bytes of uncompressed data.
 */
LIBEXPORT bool
deflate_decompress(struct deflate_decompressor * restrict d,
		   const void * restrict in, size_t in_nbytes,
		   void * restrict out, size_t out_nbytes)
{
	u8 *out_next = out;
	u8 * const out_end = out_next + out_nbytes;
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	bitbuf_t bitbuf = 0;
	unsigned bitsleft = 0;
	size_t overrun_count = 0;
	unsigned i;
	unsigned is_final_block;
	unsigned block_type;
	u16 len;
	u16 nlen;
	unsigned num_litlen_syms;
	unsigned num_offset_syms;

next_block:
	/* Starting to read the next block.  */
	;

	STATIC_ASSERT(CAN_ENSURE(1 + 2 + 5 + 5 + 4));
	ENSURE_BITS(1 + 2 + 5 + 5 + 4);

	/* BFINAL: 1 bit  */
	is_final_block = POP_BITS(1);

	/* BTYPE: 2 bits  */
	block_type = POP_BITS(2);

	if (block_type == DEFLATE_BLOCKTYPE_DYNAMIC_HUFFMAN) {

		/* Dynamic Huffman block.  */

		/* The order in which precode lengths are stored.  */
		static const u8 deflate_precode_lens_permutation[DEFLATE_NUM_PRECODE_SYMS] = {
			16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15
		};

		unsigned num_explicit_precode_lens;

		/* Read the codeword length counts.  */

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == ((1 << 5) - 1) + 257);
		num_litlen_syms = POP_BITS(5) + 257;

		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == ((1 << 5) - 1) + 1);
		num_offset_syms = POP_BITS(5) + 1;

		STATIC_ASSERT(DEFLATE_NUM_PRECODE_SYMS == ((1 << 4) - 1) + 4);
		num_explicit_precode_lens = POP_BITS(4) + 4;

		/* Read the precode codeword lengths.  */
		STATIC_ASSERT(DEFLATE_MAX_PRE_CODEWORD_LEN == (1 << 3) - 1);
		if (CAN_ENSURE(DEFLATE_NUM_PRECODE_SYMS * 3)) {

			ENSURE_BITS(DEFLATE_NUM_PRECODE_SYMS * 3);

			for (i = 0; i < num_explicit_precode_lens; i++)
				d->precode_lens[deflate_precode_lens_permutation[i]] = POP_BITS(3);
		} else {
			for (i = 0; i < num_explicit_precode_lens; i++) {
				ENSURE_BITS(3);
				d->precode_lens[deflate_precode_lens_permutation[i]] = POP_BITS(3);
			}
		}

		for (; i < DEFLATE_NUM_PRECODE_SYMS; i++)
			d->precode_lens[deflate_precode_lens_permutation[i]] = 0;

		/* Build the decode table for the precode.  */
		if (!build_precode_decode_table(d))
			return false;

		/* Expand the literal/length and offset codeword lengths.  */
		for (i = 0; i < num_litlen_syms + num_offset_syms; ) {
			u32 entry;
			unsigned presym;
			u8 rep_val;
			unsigned rep_count;

			ENSURE_BITS(DEFLATE_MAX_PRE_CODEWORD_LEN + 7);

			/* (The code below assumes there are no binary trees in
			 * the decode table.)  */
			STATIC_ASSERT(DEFLATE_PRECODE_TABLEBITS == DEFLATE_MAX_PRE_CODEWORD_LEN);

			/* Read the next precode symbol.  */
			entry = d->precode_decode_table[BITS(DEFLATE_MAX_PRE_CODEWORD_LEN)];
			REMOVE_BITS(entry >> HUFFDEC_LEN_SHIFT);
			presym = entry & HUFFDEC_VALUE_MASK;

			if (presym < 16) {
				/* Explicit codeword length  */
				d->lens[i++] = presym;
				continue;
			}

			/* Run-length encoded codeword lengths  */

			/* Note: we don't need verify that the repeat count
			 * doesn't overflow the number of elements, since we
			 * have enough extra spaces to allow for the worst-case
			 * overflow (138 zeroes when only 1 length was
			 * remaining).
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
				/* Repeat the previous length 3 - 6 times  */
				if (SAFETY_CHECK(i == 0))
					return false;
				rep_val = d->lens[i - 1];
				STATIC_ASSERT(3 + ((1 << 2) - 1) == 6);
				rep_count = 3 + POP_BITS(2);
				d->lens[i + 0] = rep_val;
				d->lens[i + 1] = rep_val;
				d->lens[i + 2] = rep_val;
				d->lens[i + 3] = rep_val;
				d->lens[i + 4] = rep_val;
				d->lens[i + 5] = rep_val;
				i += rep_count;
			} else if (presym == 17) {
				/* Repeat zero 3 - 10 times  */
				STATIC_ASSERT(3 + ((1 << 3) - 1) == 10);
				rep_count = 3 + POP_BITS(3);
				d->lens[i + 0] = 0;
				d->lens[i + 1] = 0;
				d->lens[i + 2] = 0;
				d->lens[i + 3] = 0;
				d->lens[i + 4] = 0;
				d->lens[i + 5] = 0;
				d->lens[i + 6] = 0;
				d->lens[i + 7] = 0;
				d->lens[i + 8] = 0;
				d->lens[i + 9] = 0;
				i += rep_count;
			} else {
				/* Repeat zero 11 - 138 times  */
				STATIC_ASSERT(11 + ((1 << 7) - 1) == 138);
				rep_count = 11 + POP_BITS(7);
				memset(&d->lens[i], 0, rep_count * sizeof(d->lens[i]));
				i += rep_count;
			}
		}
	} else if (block_type == DEFLATE_BLOCKTYPE_UNCOMPRESSED) {

		/* Uncompressed block: copy 'len' bytes literally from the input
		 * buffer to the output buffer.  */

		ALIGN_INPUT();

		if (SAFETY_CHECK(in_end - in_next < 4))
			return false;

		len = READ_U16();
		nlen = READ_U16();

		if (SAFETY_CHECK(len != (u16)~nlen))
			return false;

		if (SAFETY_CHECK(len > out_end - out_next))
			return false;

		if (SAFETY_CHECK(len > in_end - in_next))
			return false;

		memcpy(out_next, in_next, len);
		in_next += len;
		out_next += len;

		goto block_done;

	} else if (block_type == DEFLATE_BLOCKTYPE_STATIC_HUFFMAN) {

		/* Static Huffman block: set the static Huffman codeword
		 * lengths.  Then the remainder is the same as decompressing a
		 * dynamic Huffman block.  */

		STATIC_ASSERT(DEFLATE_NUM_LITLEN_SYMS == 288);
		STATIC_ASSERT(DEFLATE_NUM_OFFSET_SYMS == 32);

		for (i = 0; i < 144; i++)
			d->lens[i] = 8;
		for (; i < 256; i++)
			d->lens[i] = 9;
		for (; i < 280; i++)
			d->lens[i] = 7;
		for (; i < 288; i++)
			d->lens[i] = 8;

		for (; i < 288 + 32; i++)
			d->lens[i] = 5;

		num_litlen_syms = 288;
		num_offset_syms = 32;

	} else {
		/* Reserved block type.  */
		return false;
	}

	/* Decompressing a Huffman block (either dynamic or static)  */

	if (!build_offset_decode_table(d, num_litlen_syms, num_offset_syms))
		return false;

	if (!build_litlen_decode_table(d, num_litlen_syms, num_offset_syms))
		return false;

	/* The main DEFLATE decode loop  */
	for (;;) {
		u32 entry;
		u32 length;
		u32 offset;

		/* If our bitbuffer variable is large enough, we load new bits
		 * only once for each match or literal decoded.  This is
		 * fastest.  Otherwise, we may need to load new bits multiple
		 * times when decoding a match.  */

		STATIC_ASSERT(CAN_ENSURE(DEFLATE_MAX_LITLEN_CODEWORD_LEN));
		ENSURE_BITS(MAX_ENSURE);

		/* Read a literal or length.  */

		entry = d->litlen_decode_table[BITS(DEFLATE_LITLEN_TABLEBITS)];

		if (CAN_ENSURE(DEFLATE_LITLEN_TABLEBITS * 2) &&
		    likely(out_end - out_next >= MAX_ENSURE / DEFLATE_LITLEN_TABLEBITS))
		{
			/* Fast path for decoding literals  */

		#define NUM_BITS_TO_ENSURE_AFTER_INLINE_LITERALS		\
			((MAX_ENSURE >= DEFLATE_MAX_MATCH_BITS) ?		\
			 DEFLATE_MAX_MATCH_BITS :				\
			  ((MAX_ENSURE >= DEFLATE_MAX_LITLEN_CODEWORD_LEN +	\
					  DEFLATE_MAX_EXTRA_LENGTH_BITS) ?	\
				DEFLATE_MAX_LITLEN_CODEWORD_LEN +		\
				DEFLATE_MAX_EXTRA_LENGTH_BITS :			\
					DEFLATE_MAX_LITLEN_CODEWORD_LEN))

		#define INLINE_LITERAL(seq)					\
			if (CAN_ENSURE(DEFLATE_LITLEN_TABLEBITS * (seq))) {	\
				entry = d->litlen_decode_table[			\
						BITS(DEFLATE_LITLEN_TABLEBITS)];\
				if (entry & HUFFDEC_NOT_LITERAL) {		\
					if ((seq) != 1)				\
						ENSURE_BITS(NUM_BITS_TO_ENSURE_AFTER_INLINE_LITERALS);	\
					goto not_literal;			\
				}						\
				REMOVE_BITS(entry >> HUFFDEC_LEN_SHIFT);	\
				*out_next++ = entry;				\
			}

			INLINE_LITERAL(1);
			INLINE_LITERAL(2);
			INLINE_LITERAL(3);
			INLINE_LITERAL(4);
			INLINE_LITERAL(5);
			INLINE_LITERAL(6);
			INLINE_LITERAL(7);
			INLINE_LITERAL(8);
			continue;
		}

		if (!(entry & HUFFDEC_NOT_LITERAL)) {
			REMOVE_BITS(entry >> HUFFDEC_LEN_SHIFT);
			if (SAFETY_CHECK(out_next == out_end))
				return false;
			*out_next++ = entry;
			continue;
		}
	not_literal:
		if (likely(!(entry & HUFFDEC_NOT_FULL_LENGTH))) {

			/* The next TABLEBITS bits were enough to directly look
			 * up a litlen symbol, which was a length slot.  In
			 * addition, the full match length, including the extra
			 * bits, fit into TABLEBITS.  So the result of the
			 * lookup was the full match length.
			 *
			 * On typical data, most match lengths are short enough
			 * to fall into this category.  */

			REMOVE_BITS((entry >> HUFFDEC_LEN_SHIFT) & HUFFDEC_LEN_MASK);
			length = entry & HUFFDEC_VALUE_MASK;

		} else if (!(entry & HUFFDEC_TREE_NONLEAF)) {

			/* The next TABLEBITS bits were enough to directly look
			 * up a litlen symbol, which was either a length slot or
			 * end-of-block.  However, the full match length,
			 * including the extra bits (0 in the case of
			 * end-of-block), requires more than TABLEBITS bits to
			 * decode.  So the result of the lookup was the length
			 * base and number of extra length bits.  We will read
			 * this number of extra length bits and add them to the
			 * length base in order to construct the full length.
			 *
			 * On typical data, this case is rare.  */

			REMOVE_BITS((entry >> HUFFDEC_LEN_SHIFT) & HUFFDEC_LEN_MASK);
			entry &= HUFFDEC_VALUE_MASK;

			if (!CAN_ENSURE(DEFLATE_MAX_LITLEN_CODEWORD_LEN +
					DEFLATE_MAX_EXTRA_LENGTH_BITS))
				ENSURE_BITS(DEFLATE_MAX_EXTRA_LENGTH_BITS);

			length = (entry & HUFFDEC_LENGTH_BASE_MASK) +
				  POP_BITS(entry >> HUFFDEC_EXTRA_LENGTH_BITS_SHIFT);
		} else {

			/* The next TABLEBITS bits were not enough to directly
			 * look up a litlen symbol.  Therefore, we must walk the
			 * appropriate binary tree to decode the symbol, which
			 * may be a literal, length slot, or end-of-block.
			 *
			 * On typical data, this case is rare.  */

			REMOVE_BITS(DEFLATE_LITLEN_TABLEBITS);
			do {
				entry &= ~HUFFDEC_TREE_NONLEAF_FLAGS;
				entry += POP_BITS(1);
				entry = d->litlen_decode_table[entry];
			} while (entry & HUFFDEC_TREE_NONLEAF_FAST_FLAG);
			if (entry < 256) {
				if (SAFETY_CHECK(out_next == out_end))
					return false;
				*out_next++ = entry;
				continue;
			}
			entry -= 256;

			if (!CAN_ENSURE(DEFLATE_MAX_LITLEN_CODEWORD_LEN +
					DEFLATE_MAX_EXTRA_LENGTH_BITS))
				ENSURE_BITS(DEFLATE_MAX_EXTRA_LENGTH_BITS);

			length = (entry & HUFFDEC_LENGTH_BASE_MASK) +
				  POP_BITS(entry >> HUFFDEC_EXTRA_LENGTH_BITS_SHIFT);
		}

		/* The match destination must not end after the end of the
		 * output buffer.  */
		if (SAFETY_CHECK(length > out_end - out_next))
			return false;

		if (unlikely(length == HUFFDEC_END_OF_BLOCK_LENGTH))
			goto block_done;

		/* Read the match offset.  */

		if (!CAN_ENSURE(DEFLATE_MAX_MATCH_BITS)) {
			if (CAN_ENSURE(DEFLATE_MAX_OFFSET_CODEWORD_LEN +
				       DEFLATE_MAX_EXTRA_OFFSET_BITS))
				ENSURE_BITS(DEFLATE_MAX_OFFSET_CODEWORD_LEN +
					    DEFLATE_MAX_EXTRA_OFFSET_BITS);
			else
				ENSURE_BITS(DEFLATE_MAX_OFFSET_CODEWORD_LEN);
		}

		entry = d->offset_decode_table[BITS(DEFLATE_OFFSET_TABLEBITS)];
		if (likely(!(entry & HUFFDEC_TREE_NONLEAF_FAST_FLAG))) {
			REMOVE_BITS(entry >> HUFFDEC_LEN_SHIFT);
			entry &= HUFFDEC_VALUE_MASK;
		} else {
			REMOVE_BITS(DEFLATE_OFFSET_TABLEBITS);
			do {
				entry &= ~HUFFDEC_TREE_NONLEAF_FLAGS;
				entry += POP_BITS(1);
				entry = d->offset_decode_table[entry];
			} while (entry & HUFFDEC_TREE_NONLEAF_FAST_FLAG);
		}

		/* The value we have here isn't the offset symbol itself, but
		 * rather the offset symbol indexed into
		 * deflate_offset_symbol_data[].  This gives us the offset base
		 * and number of extra offset bits without having to index
		 * additional tables in the main decode loop.  */

		if (!CAN_ENSURE(DEFLATE_MAX_OFFSET_CODEWORD_LEN +
				DEFLATE_MAX_EXTRA_OFFSET_BITS))
			ENSURE_BITS(DEFLATE_MAX_EXTRA_OFFSET_BITS);

		offset = (entry & HUFFDEC_OFFSET_BASE_MASK) +
			 POP_BITS(entry >> HUFFDEC_EXTRA_OFFSET_BITS_SHIFT);

		/* The match source must not begin before the beginning of the
		 * output buffer.  */
		if (SAFETY_CHECK(offset > out_next - (const u8 *)out))
			return false;

		/* Copy the match:
		 * 'length' bytes at 'out_next - offset' to 'out_next'.  */

		lz_copy(out_next, length, offset, out_end);

		out_next += length;
	}

block_done:
	/* Finished decoding a block.  */

	if (!is_final_block)
		goto next_block;

	/* That was the last block.  Return %true if we got all the output we
	 * expected, otherwise %false.  */
	return (out_next == out_end);
}

LIBEXPORT struct deflate_decompressor *
deflate_alloc_decompressor(void)
{
	return malloc(sizeof(struct deflate_decompressor));
}

LIBEXPORT void
deflate_free_decompressor(struct deflate_decompressor *d)
{
	free(d);
}
