/*
 * matchfinder_common.h - common code for Lempel-Ziv matchfinding
 */

#ifndef LIB_MATCHFINDER_COMMON_H
#define LIB_MATCHFINDER_COMMON_H

#include "lib_common.h"
#include "unaligned.h"

#ifndef MATCHFINDER_WINDOW_ORDER
#  error "MATCHFINDER_WINDOW_ORDER must be defined!"
#endif

#define MATCHFINDER_WINDOW_SIZE (1UL << MATCHFINDER_WINDOW_ORDER)

typedef s16 mf_pos_t;

#define MATCHFINDER_INITVAL ((mf_pos_t)-MATCHFINDER_WINDOW_SIZE)

#define MATCHFINDER_ALIGNMENT 8

typedef bool (*matchfinder_func_t)(mf_pos_t *, size_t);

/** No-op dispatch function. **/
static inline bool arch_matchfinder_noop(mf_pos_t *pos, size_t size)
{
	return false;
}

#undef DEFAULT_MATCHFINDER_INIT
#undef DEFAULT_MATCHFINDER_REBASE
#undef DISPATCH
#ifdef _aligned_attribute
#  if defined(__arm__) || defined(__aarch64__)
#    include "arm/matchfinder_impl.h"
#  elif defined(__i386__) || defined(__x86_64__)
#    include "x86/matchfinder_impl.h"
#  endif
#endif

#if defined(DEFAULT_MATCHFINDER_INIT) && defined(DEFAULT_MATCHFINDER_REBASE)
#  define matchfinder_init_impl 	DEFAULT_MATCHFINDER_INIT 
#  define matchfinder_rebase_impl	DEFAULT_MATCHFINDER_REBASE
#elif defined(DISPATCH)
static bool dispatch_init(mf_pos_t *, size_t);
static bool dispatch_rebase(mf_pos_t *, size_t);

static volatile matchfinder_func_t matchfinder_init_impl = dispatch_init;
static volatile matchfinder_func_t matchfinder_rebase_impl = dispatch_rebase;

/* Choose the fastest implementation at runtime */
static bool dispatch_init(mf_pos_t *pos, size_t size)
{
	matchfinder_func_t f = arch_select_matchfinder_init();

	if (f == NULL)
		f = arch_matchfinder_noop;

	matchfinder_init_impl = f;
	return matchfinder_init_impl(pos, size);
}

static bool dispatch_rebase(mf_pos_t *pos, size_t size)
{
	matchfinder_func_t f = arch_select_matchfinder_rebase();

	if (f == NULL)
		f = arch_matchfinder_noop;

	matchfinder_rebase_impl = f;
	return matchfinder_rebase_impl(pos, size);
}
#else
#  define matchfinder_init_impl 	arch_matchfinder_noop 
#  define matchfinder_rebase_impl	arch_matchfinder_noop
#endif

/*
 * Initialize the hash table portion of the matchfinder.
 *
 * Essentially, this is an optimized memset().
 *
 * 'data' must be aligned to a MATCHFINDER_ALIGNMENT boundary.
 */
static forceinline void
matchfinder_init(mf_pos_t *data, size_t num_entries)
{
	size_t i;

	if (matchfinder_init_impl(data, num_entries * sizeof(data[0])))
		return;

	for (i = 0; i < num_entries; i++)
		data[i] = MATCHFINDER_INITVAL;
}

/*
 * Slide the matchfinder by WINDOW_SIZE bytes.
 *
 * This must be called just after each WINDOW_SIZE bytes have been run through
 * the matchfinder.
 *
 * This will subtract WINDOW_SIZE bytes from each entry in the array specified.
 * The effect is that all entries are updated to be relative to the current
 * position, rather than the position WINDOW_SIZE bytes prior.
 *
 * Underflow is detected and replaced with signed saturation.  This ensures that
 * once the sliding window has passed over a position, that position forever
 * remains out of bounds.
 *
 * The array passed in must contain all matchfinder data that is
 * position-relative.  Concretely, this will include the hash table as well as
 * the table of positions that is used to link together the sequences in each
 * hash bucket.  Note that in the latter table, the links are 1-ary in the case
 * of "hash chains", and 2-ary in the case of "binary trees".  In either case,
 * the links need to be rebased in the same way.
 */
static forceinline void
matchfinder_rebase(mf_pos_t *data, size_t num_entries)
{
	size_t i;

	if (matchfinder_rebase_impl(data, num_entries * sizeof(data[0])))
		return;

	if (MATCHFINDER_WINDOW_SIZE == 32768) {
		/* Branchless version for 32768 byte windows.  If the value was
		 * already negative, clear all bits except the sign bit; this
		 * changes the value to -32768.  Otherwise, set the sign bit;
		 * this is equivalent to subtracting 32768.  */
		for (i = 0; i < num_entries; i++) {
			u16 v = data[i];
			u16 sign_bit = v & 0x8000;
			v &= sign_bit - ((sign_bit >> 15) ^ 1);
			v |= 0x8000;
			data[i] = v;
		}
		return;
	}

	for (i = 0; i < num_entries; i++) {
		if (data[i] >= 0)
			data[i] -= (mf_pos_t)-MATCHFINDER_WINDOW_SIZE;
		else
			data[i] = (mf_pos_t)-MATCHFINDER_WINDOW_SIZE;
	}
}

/*
 * The hash function: given a sequence prefix held in the low-order bits of a
 * 32-bit value, multiply by a carefully-chosen large constant.  Discard any
 * bits of the product that don't fit in a 32-bit value, but take the
 * next-highest @num_bits bits of the product as the hash value, as those have
 * the most randomness.
 */
static forceinline u32
lz_hash(u32 seq, unsigned num_bits)
{
	return (u32)(seq * 0x1E35A7BD) >> (32 - num_bits);
}

/*
 * Return the number of bytes at @matchptr that match the bytes at @strptr, up
 * to a maximum of @max_len.  Initially, @start_len bytes are matched.
 */
static forceinline unsigned
lz_extend(const u8 * const strptr, const u8 * const matchptr,
	  const unsigned start_len, const unsigned max_len)
{
	unsigned len = start_len;
	machine_word_t v_word;

	if (UNALIGNED_ACCESS_IS_FAST) {

		if (likely(max_len - len >= 4 * WORDBYTES)) {

		#define COMPARE_WORD_STEP				\
			v_word = load_word_unaligned(&matchptr[len]) ^	\
				 load_word_unaligned(&strptr[len]);	\
			if (v_word != 0)				\
				goto word_differs;			\
			len += WORDBYTES;				\

			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
			COMPARE_WORD_STEP
		#undef COMPARE_WORD_STEP
		}

		while (len + WORDBYTES <= max_len) {
			v_word = load_word_unaligned(&matchptr[len]) ^
				 load_word_unaligned(&strptr[len]);
			if (v_word != 0)
				goto word_differs;
			len += WORDBYTES;
		}
	}

	while (len < max_len && matchptr[len] == strptr[len])
		len++;
	return len;

word_differs:
	if (CPU_IS_LITTLE_ENDIAN())
		len += (bsfw(v_word) >> 3);
	else
		len += (WORDBITS - 1 - bsrw(v_word)) >> 3;
	return len;
}

#endif /* LIB_MATCHFINDER_COMMON_H */
