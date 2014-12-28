/*
 * matchfinder_nonsliding.h
 *
 * Definitions for nonsliding window matchfinders.
 *
 * "Nonsliding window" means that any prior sequence can be matched.
 */

#if MATCHFINDER_WINDOW_ORDER <= 16
typedef u16 pos_t;
#else
typedef u32 pos_t;
#endif

#if MATCHFINDER_WINDOW_ORDER != 16 && MATCHFINDER_WINDOW_ORDER != 32

/* Not all the bits of the position type are needed, so the sign bit can be
 * reserved to mean "out of bounds".  */
#define MATCHFINDER_INITVAL ((pos_t)-1)

static inline bool
matchfinder_match_in_window(pos_t cur_match, const u8 *in_base, const u8 *in_next)
{
	return !(cur_match & ((pos_t)1 << (sizeof(pos_t) * 8 - 1)));
}

#else

/* All bits of the position type are needed, so use 0 to mean "out of bounds".
 * This prevents the beginning of the buffer from matching anything; however,
 * this doesn't matter much.  */

#define MATCHFINDER_INITVAL ((pos_t)0)

static inline bool
matchfinder_match_in_window(pos_t cur_match, const u8 *in_base, const u8 *in_next)
{
	return cur_match != 0;
}

#endif

static inline pos_t
matchfinder_slot_for_match(pos_t cur_match)
{
	return cur_match;
}
