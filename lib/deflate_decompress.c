/*
 * deflate_decompress.c - a decompressor for DEFLATE
 *
 * Originally public domain; changes after 2016-09-07 are copyrighted.
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
 * This is a highly optimized DEFLATE decompressor.  When compiled with gcc on
 * x86_64, it decompresses data in about 52% of the time of zlib (48% if BMI2
 * instructions are available).  On other architectures it should still be
 * significantly faster than zlib, but the difference may be smaller.
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
 * - On x86_64, compile a version of the decompression routine using BMI2
 *   instructions and use it automatically at runtime when supported.
 */

#include <stdlib.h>
#include <string.h>

#include "deflate_constants.h"
#include "unaligned.h"
#include "x86_cpu_features.h"

#include "libdeflate.h"

/*
 * If the expression passed to SAFETY_CHECK() evaluates to false, then the
 * decompression routine immediately returns LIBDEFLATE_BAD_DATA, indicating the
 * compressed data is invalid.
 *
 * Theoretically, these checks could be disabled for specialized applications
 * where all input to the decompressor will be trusted.
 */
#if 0
#  pragma message("UNSAFE DECOMPRESSION IS ENABLED. THIS MUST ONLY BE USED IF THE DECOMPRESSOR INPUT WILL ALWAYS BE TRUSTED!")
#  define SAFETY_CHECK(expr)	(void)(expr)
#else
#  define SAFETY_CHECK(expr)	if (unlikely(!(expr))) return LIBDEFLATE_BAD_DATA
#endif

/*
 * Each TABLEBITS number is the base-2 logarithm of the number of entries in the
 * main portion of the corresponding decode table.  Each number should be large
 * enough to ensure that for typical data, the vast majority of symbols can be
 * decoded by a direct lookup of the next TABLEBITS bits of compressed data.
 * However, this must be balanced against the fact that a larger table requires
 * more memory and requires more time to fill.
 *
 * Note: you cannot change a TABLEBITS number without also changing the
 * corresponding ENOUGH number!
 */
#define PRECODE_TABLEBITS	7
#define LITLEN_TABLEBITS	10
#define OFFSET_TABLEBITS	8

/*
 * Each ENOUGH number is the maximum number of decode table entries that may be
 * required for the corresponding Huffman code, including the main table and all
 * subtables.  Each number depends on three parameters:
 *
 *	(1) the maximum number of symbols in the code (DEFLATE_NUM_*_SYMBOLS)
 *	(2) the number of main table bits (the TABLEBITS numbers defined above)
 *	(3) the maximum allowed codeword length (DEFLATE_MAX_*_CODEWORD_LEN)
 *
 * The ENOUGH numbers were computed using the utility program 'enough' from
 * zlib.  This program enumerates all possible relevant Huffman codes to find
 * the worst-case usage of decode table entries.
 */
#define PRECODE_ENOUGH		128	/* enough 19 7 7	*/
#define LITLEN_ENOUGH		1334	/* enough 288 10 15	*/
#define OFFSET_ENOUGH		402	/* enough 32 8 15	*/

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
		len_t precode_lens[DEFLATE_NUM_PRECODE_SYMS];

		struct {
			len_t lens[DEFLATE_NUM_LITLEN_SYMS +
				   DEFLATE_NUM_OFFSET_SYMS +
				   DEFLATE_MAX_LENS_OVERRUN];

			u32 precode_decode_table[PRECODE_ENOUGH];
		} l;

		u32 litlen_decode_table[LITLEN_ENOUGH];
	} u;

	u32 offset_decode_table[OFFSET_ENOUGH];

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
 *
 * 64-bit platforms have a significant advantage: they get a bigger bitbuffer
 * which they have to fill less often.
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
 * undetected, and libdeflate_deflate_decompress() may return a success status
 * rather than the expected failure status if one occurs.  However, this is
 * irrelevant because even if this specific case were to be handled "correctly",
 * one could easily come up with a different case where the compressed data
 * would be corrupted in such a way that fully retains its validity.  Users
 * should run a checksum against the uncompressed data if they wish to detect
 * corruptions.
 */
#define FILL_BITS_BYTEWISE()					\
do {								\
	if (likely(in_next != in_end))				\
		bitbuf |= (bitbuf_t)*in_next++ << bitsleft;	\
	else							\
		overrun_count++;				\
	bitsleft += 8;						\
} while (bitsleft <= BITBUF_NBITS - 8)

/*
 * Fill the bitbuffer variable by reading the next word from the input buffer.
 * This can be significantly faster than FILL_BITS_BYTEWISE().  However, for
 * this to work correctly, the word must be interpreted in little-endian format.
 * In addition, the memory access may be unaligned.  Therefore, this method is
 * most efficient on little-endian architectures that support fast unaligned
 * access, such as x86 and x86_64.
 */
#define FILL_BITS_WORDWISE()					\
do {								\
	bitbuf |= get_unaligned_leword(in_next) << bitsleft;	\
	in_next += (BITBUF_NBITS - bitsleft) >> 3;		\
	bitsleft += (BITBUF_NBITS - bitsleft) & ~7;		\
} while (0)

/*
 * Does the bitbuffer variable currently contain at least 'n' bits?
 */
#define HAVE_BITS(n) (bitsleft >= (n))

/*
 * Load more bits from the input buffer until the specified number of bits is
 * present in the bitbuffer variable.  'n' cannot be too large; see MAX_ENSURE
 * and CAN_ENSURE().
 */
#define ENSURE_BITS(n)						\
if (!HAVE_BITS(n)) {						\
	if (CPU_IS_LITTLE_ENDIAN() &&				\
	    UNALIGNED_ACCESS_IS_FAST &&				\
	    likely(in_end - in_next >= sizeof(bitbuf_t)))	\
		FILL_BITS_WORDWISE();				\
	else							\
		FILL_BITS_BYTEWISE();				\
}

/*
 * Return the next 'n' bits from the bitbuffer variable without removing them.
 */
#define BITS(n) ((u32)bitbuf & (((u32)1 << (n)) - 1))

/*
 * Remove the next 'n' bits from the bitbuffer variable.
 */
#define REMOVE_BITS(n) (bitbuf >>= (n), bitsleft -= (n))

/*
 * Remove and return the next 'n' bits from the bitbuffer variable.
 */
#define POP_BITS(n) (tmp32 = BITS(n), REMOVE_BITS(n), tmp32)

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
do {									\
	in_next -= (bitsleft >> 3) - MIN(overrun_count, bitsleft >> 3);	\
	bitbuf = 0;							\
	bitsleft = 0;							\
} while(0)

/*
 * Read a 16-bit value from the input.  This must have been preceded by a call
 * to ALIGN_INPUT(), and the caller must have already checked for overrun.
 */
#define READ_U16() (tmp16 = get_unaligned_le16(in_next), in_next += 2, tmp16)

/*****************************************************************************
 *                              Huffman decoding                             *
 *****************************************************************************/

/*
 * A decode table for order TABLEBITS consists of a main table of (1 <<
 * TABLEBITS) entries followed by a variable number of subtables.
 *
 * The decoding algorithm takes the next TABLEBITS bits of compressed data and
 * uses them as an index into the decode table.  The resulting entry is either a
 * "direct entry", meaning that it contains the value desired, or a "subtable
 * pointer", meaning that the entry references a subtable that must be indexed
 * using more bits of the compressed data to decode the symbol.
 *
 * Each decode table (a main table along with with its subtables, if any) is
 * associated with a Huffman code.  Logically, the result of a decode table
 * lookup is a symbol from the alphabet from which the corresponding Huffman
 * code was constructed.  A symbol with codeword length n <= TABLEBITS is
 * associated with 2**(TABLEBITS - n) direct entries in the table, whereas a
 * symbol with codeword length n > TABLEBITS is associated with one or more
 * subtable entries.
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
 * The size of each decode table entry is 32 bits, which provides slightly
 * better performance than 16-bit entries on 32 and 64 bit processers, provided
 * that the table doesn't get so large that it takes up too much memory and
 * starts generating cache misses.  The bits of each decode table entry are
 * defined as follows:
 *
 * - Bits 30 -- 31: flags (see below)
 * - Bits 8 -- 29: decode result: a Huffman symbol or related data
 * - Bits 0 -- 7: codeword length
 */

/*
 * This flag is set in all main decode table entries that represent subtable
 * pointers.
 */
#define HUFFDEC_SUBTABLE_POINTER	0x80000000

/*
 * This flag is set in all entries in the litlen decode table that represent
 * literals.
 */
#define HUFFDEC_LITERAL			0x40000000

/* Mask for extracting the codeword length from a decode table entry.  */
#define HUFFDEC_LENGTH_MASK		0xFF

/* Shift to extract the decode result from a decode table entry.  */
#define HUFFDEC_RESULT_SHIFT		8

/* The decode result for each precode symbol.  There is no special optimization
 * for the precode; the decode result is simply the symbol value.  */
static const u32 precode_decode_results[DEFLATE_NUM_PRECODE_SYMS] = {
	0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18,
};

/* The decode result for each litlen symbol.  For literals, this is the literal
 * value itself and the HUFFDEC_LITERAL flag.  For lengths, this is the length
 * base and the number of extra length bits.  */
static const u32 litlen_decode_results[DEFLATE_NUM_LITLEN_SYMS] = {
#define ENTRY(literal)	((HUFFDEC_LITERAL >> HUFFDEC_RESULT_SHIFT) | (literal))

	/* Literals  */
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

#define HUFFDEC_EXTRA_LENGTH_BITS_MASK	0xFF
#define HUFFDEC_LENGTH_BASE_SHIFT	8
#define HUFFDEC_END_OF_BLOCK_LENGTH	0

#define ENTRY(length_base, num_extra_bits) \
	(((u32)(length_base) << HUFFDEC_LENGTH_BASE_SHIFT) | (num_extra_bits))

	/* End of block  */
	ENTRY(HUFFDEC_END_OF_BLOCK_LENGTH, 0),

	/* Lengths  */
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

/* The decode result for each offset symbol.  This is the offset base and the
 * number of extra offset bits.  */
static const u32 offset_decode_results[DEFLATE_NUM_OFFSET_SYMS] = {

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

/* Construct a decode table entry from a decode result and codeword length.  */
static forceinline u32
make_decode_table_entry(u32 result, u32 length)
{
	return (result << HUFFDEC_RESULT_SHIFT) | length;
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
 *	An array which provides, for each symbol, the actual value to store into
 *	the decode table.  This value will be directly produced as the result of
 *	decoding that symbol, thereby moving the indirection out of the decode
 *	loop and into the table initialization.
 * @table_bits
 *	The log base-2 of the number of main table entries to use.
 * @max_codeword_len
 *	The maximum allowed codeword length for this Huffman code.
 * @working_space
 *	A temporary array of length '2 * (@max_codeword_len + 1) + @num_syms'.
 *
 * Returns %true if successful; %false if the codeword lengths do not form a
 * valid Huffman code.
 */
static bool
build_decode_table(u32 decode_table[],
		   const len_t lens[],
		   const unsigned num_syms,
		   const u32 decode_results[],
		   const unsigned table_bits,
		   const unsigned max_codeword_len,
		   u16 working_space[])
{
	u16 * const len_counts = &working_space[0];
	u16 * const offsets = &working_space[1 * (max_codeword_len + 1)];
	u16 * const sorted_syms = &working_space[2 * (max_codeword_len + 1)];
	unsigned len;
	unsigned sym;
	s32 remainder;
	unsigned sym_idx;
	unsigned codeword_len;
	unsigned codeword_reversed = 0;
	unsigned cur_codeword_prefix = -1;
	unsigned cur_table_start = 0;
	unsigned cur_table_bits = table_bits;
	unsigned num_dropped_bits = 0;
	const unsigned table_mask = (1U << table_bits) - 1;

	/* Count how many symbols have each codeword length, including 0.  */
	for (len = 0; len <= max_codeword_len; len++)
		len_counts[len] = 0;
	for (sym = 0; sym < num_syms; sym++)
		len_counts[lens[sym]]++;

	/* Sort the symbols primarily by increasing codeword length and
	 * secondarily by increasing symbol value.  */

	/* Initialize 'offsets' so that offsets[len] is the number of codewords
	 * shorter than 'len' bits, including length 0.  */
	offsets[0] = 0;
	for (len = 0; len < max_codeword_len; len++)
		offsets[len + 1] = offsets[len] + len_counts[len];

	/* Use the 'offsets' array to sort the symbols.  */
	for (sym = 0; sym < num_syms; sym++)
		sorted_syms[offsets[lens[sym]]++] = sym;

	/* It is already guaranteed that all lengths are <= max_codeword_len,
	 * but it cannot be assumed they form a complete prefix code.  A
	 * codeword of length n should require a proportion of the codespace
	 * equaling (1/2)^n.  The code is complete if and only if, by this
	 * measure, the codespace is exactly filled by the lengths.  */
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
		 * incomplete code.  */

		/* Initialize the table entries to default values.  When
		 * decompressing a well-formed stream, these default values will
		 * never be used.  But since a malformed stream might contain
		 * any bits at all, these entries need to be set anyway.  */
		u32 entry = make_decode_table_entry(decode_results[0], 1);
		for (sym = 0; sym < (1U << table_bits); sym++)
			decode_table[sym] = entry;

		/* A completely empty code is permitted.  */
		if (remainder == (1U << max_codeword_len))
			return true;

		/* The code is nonempty and incomplete.  Proceed only if there
		 * is a single used symbol and its codeword has length 1.  The
		 * DEFLATE RFC is somewhat unclear regarding this case.  What
		 * zlib's decompressor does is permit this case for
		 * literal/length and offset codes and assume the codeword is 0
		 * rather than 1.  We do the same except we allow this case for
		 * precodes too.  */
		if (remainder != (1U << (max_codeword_len - 1)) ||
		    len_counts[1] != 1)
			return false;
	}

	/* Generate the decode table entries.  Since we process codewords from
	 * shortest to longest, the main portion of the decode table is filled
	 * first; then the subtables are filled.  Note that it's already been
	 * verified that the code is nonempty and not over-subscribed.  */

	/* Start with the smallest codeword length and the smallest-valued
	 * symbol which has that codeword length.  */
	sym_idx = offsets[0];
	codeword_len = 1;
	while (len_counts[codeword_len] == 0)
		codeword_len++;

	for (;;) {  /* For each used symbol and its codeword...  */
		unsigned sym;
		u32 entry;
		unsigned i;
		unsigned end;
		unsigned increment;
		unsigned bit;

		/* Get the next symbol.  */
		sym = sorted_syms[sym_idx];

		/* Start a new subtable if the codeword is long enough to
		 * require a subtable, *and* the first 'table_bits' bits of the
		 * codeword don't match the prefix for the previous subtable if
		 * any.  */
		if (codeword_len > table_bits &&
		    (codeword_reversed & table_mask) != cur_codeword_prefix) {

			cur_codeword_prefix = (codeword_reversed & table_mask);

			cur_table_start += 1U << cur_table_bits;

			/* Calculate the subtable length.  If the codeword
			 * length exceeds 'table_bits' by n, the subtable needs
			 * at least 2**n entries.  But it may need more; if
			 * there are fewer than 2**n codewords of length
			 * 'table_bits + n' remaining, then n will need to be
			 * incremented to bring in longer codewords until the
			 * subtable can be filled completely.  Note that it
			 * always will, eventually, be possible to fill the
			 * subtable, since the only case where we may have an
			 * incomplete code is a single codeword of length 1,
			 * and that never requires any subtables.  */
			cur_table_bits = codeword_len - table_bits;
			remainder = (s32)1 << cur_table_bits;
			for (;;) {
				remainder -= len_counts[table_bits +
							cur_table_bits];
				if (remainder <= 0)
					break;
				cur_table_bits++;
				remainder <<= 1;
			}

			/* Create the entry that points from the main table to
			 * the subtable.  This entry contains the index of the
			 * start of the subtable and the number of bits with
			 * which the subtable is indexed (the log base 2 of the
			 * number of entries it contains).  */
			decode_table[cur_codeword_prefix] =
				HUFFDEC_SUBTABLE_POINTER |
				make_decode_table_entry(cur_table_start,
							cur_table_bits);

			/* Now that we're filling a subtable, we need to drop
			 * the first 'table_bits' bits of the codewords.  */
			num_dropped_bits = table_bits;
		}

		/* Create the decode table entry, which packs the decode result
		 * and the codeword length (minus 'table_bits' for subtables)
		 * together.  */
		entry = make_decode_table_entry(decode_results[sym],
						codeword_len - num_dropped_bits);

		/* Fill in as many copies of the decode table entry as are
		 * needed.  The number of entries to fill is a power of 2 and
		 * depends on the codeword length; it could be as few as 1 or as
		 * large as half the size of the table.  Since the codewords are
		 * bit-reversed, the indices to fill are those with the codeword
		 * in its low bits; it's the high bits that vary.  */
		i = cur_table_start + (codeword_reversed >> num_dropped_bits);
		end = cur_table_start + (1U << cur_table_bits);
		increment = 1U << (codeword_len - num_dropped_bits);
		do {
			decode_table[i] = entry;
			i += increment;
		} while (i < end);

		/* Advance to the next codeword by incrementing it.  But since
		 * our codewords are bit-reversed, we must manipulate the bits
		 * ourselves rather than simply adding 1.  */
		bit = 1U << (codeword_len - 1);
		while (codeword_reversed & bit)
			bit >>= 1;
		codeword_reversed &= bit - 1;
		codeword_reversed |= bit;

		/* Advance to the next symbol.  This will either increase the
		 * codeword length, or keep the same codeword length but
		 * increase the symbol value.  Note: since we are using
		 * bit-reversed codewords, we don't need to explicitly append
		 * zeroes to the codeword when the codeword length increases. */
		if (++sym_idx == num_syms)
			return true;
		len_counts[codeword_len]--;
		while (len_counts[codeword_len] == 0)
			codeword_len++;
	}
}

/* Build the decode table for the precode.  */
static bool
build_precode_decode_table(struct libdeflate_decompressor *d)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(PRECODE_TABLEBITS == 7 && PRECODE_ENOUGH == 128);

	return build_decode_table(d->u.l.precode_decode_table,
				  d->u.precode_lens,
				  DEFLATE_NUM_PRECODE_SYMS,
				  precode_decode_results,
				  PRECODE_TABLEBITS,
				  DEFLATE_MAX_PRE_CODEWORD_LEN,
				  d->working_space);
}

/* Build the decode table for the literal/length code.  */
static bool
build_litlen_decode_table(struct libdeflate_decompressor *d,
			  unsigned num_litlen_syms, unsigned num_offset_syms)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(LITLEN_TABLEBITS == 10 && LITLEN_ENOUGH == 1334);

	return build_decode_table(d->u.litlen_decode_table,
				  d->u.l.lens,
				  num_litlen_syms,
				  litlen_decode_results,
				  LITLEN_TABLEBITS,
				  DEFLATE_MAX_LITLEN_CODEWORD_LEN,
				  d->working_space);
}

/* Build the decode table for the offset code.  */
static bool
build_offset_decode_table(struct libdeflate_decompressor *d,
			  unsigned num_litlen_syms, unsigned num_offset_syms)
{
	/* When you change TABLEBITS, you must change ENOUGH, and vice versa! */
	STATIC_ASSERT(OFFSET_TABLEBITS == 8 && OFFSET_ENOUGH == 402);

	return build_decode_table(d->offset_decode_table,
				  d->u.l.lens + num_litlen_syms,
				  num_offset_syms,
				  offset_decode_results,
				  OFFSET_TABLEBITS,
				  DEFLATE_MAX_OFFSET_CODEWORD_LEN,
				  d->working_space);
}

static forceinline machine_word_t
repeat_byte(u8 b)
{
	machine_word_t v;

	STATIC_ASSERT(WORDBITS == 32 || WORDBITS == 64);

	v = b;
	v |= v << 8;
	v |= v << 16;
	v |= v << ((WORDBITS == 64) ? 32 : 0);
	return v;
}

static forceinline void
copy_word_unaligned(const void *src, void *dst)
{
	store_word_unaligned(load_word_unaligned(src), dst);
}

/*****************************************************************************
 *                         Main decompression routine
 *****************************************************************************/

#define FUNCNAME deflate_decompress_default
#define ATTRIBUTES
#include "decompress_impl.h"
#undef FUNCNAME
#undef ATTRIBUTES

#if X86_CPU_FEATURES_ENABLED && \
	COMPILER_SUPPORTS_BMI2_TARGET && !defined(__BMI2__)
#  define FUNCNAME deflate_decompress_bmi2
#  define ATTRIBUTES __attribute__((target("bmi2")))
#  include "decompress_impl.h"
#  undef FUNCNAME
#  undef ATTRIBUTES
#  define DISPATCH_ENABLED 1
#else
#  define DISPATCH_ENABLED 0
#endif

#if DISPATCH_ENABLED

static enum libdeflate_result
dispatch(struct libdeflate_decompressor * restrict d,
	 const void * restrict in, size_t in_nbytes,
	 void * restrict out, size_t out_nbytes_avail,
	 size_t *actual_out_nbytes_ret);

typedef enum libdeflate_result (*decompress_func_t)
	(struct libdeflate_decompressor * restrict d,
	 const void * restrict in, size_t in_nbytes,
	 void * restrict out, size_t out_nbytes_avail,
	 size_t *actual_out_nbytes_ret);

static decompress_func_t decompress_impl = dispatch;

static enum libdeflate_result
dispatch(struct libdeflate_decompressor * restrict d,
	 const void * restrict in, size_t in_nbytes,
	 void * restrict out, size_t out_nbytes_avail,
	 size_t *actual_out_nbytes_ret)
{
	decompress_func_t f = deflate_decompress_default;
#if X86_CPU_FEATURES_ENABLED
	if (x86_have_cpu_features(X86_CPU_FEATURE_BMI2))
		f = deflate_decompress_bmi2;
#endif
	decompress_impl = f;
	return (*f)(d, in, in_nbytes, out, out_nbytes_avail,
		    actual_out_nbytes_ret);
}
#endif /* DISPATCH_ENABLED */


/*
 * This is the main DEFLATE decompression routine.  See libdeflate.h for the
 * documentation.
 *
 * Note that the real code is in decompress_impl.h.  The part here just handles
 * calling the appropriate implementation depending on the CPU features at
 * runtime.
 */
LIBDEFLATEAPI enum libdeflate_result
libdeflate_deflate_decompress(struct libdeflate_decompressor * restrict d,
			      const void * restrict in, size_t in_nbytes,
			      void * restrict out, size_t out_nbytes_avail,
			      size_t *actual_out_nbytes_ret)
{
#if DISPATCH_ENABLED
	return (*decompress_impl)(d, in, in_nbytes, out, out_nbytes_avail,
				  actual_out_nbytes_ret);
#else
	return deflate_decompress_default(d, in, in_nbytes, out,
					  out_nbytes_avail,
					  actual_out_nbytes_ret);
#endif
}

LIBDEFLATEAPI struct libdeflate_decompressor *
libdeflate_alloc_decompressor(void)
{
	return malloc(sizeof(struct libdeflate_decompressor));
}

LIBDEFLATEAPI void
libdeflate_free_decompressor(struct libdeflate_decompressor *d)
{
	free(d);
}
