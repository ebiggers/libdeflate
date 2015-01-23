/*
 * matchfinder_sliding.h
 *
 * Definitions for sliding window matchfinders.
 *
 * "Sliding window" means that only sequences beginning in the most recent
 * MATCHFINDER_WINDOW_SIZE bytes can be matched.
 *
 * This file has no copyright assigned and is placed in the Public Domain.
 */

#if MATCHFINDER_WINDOW_ORDER <= 15
typedef s16 pos_t;
#else
typedef s32 pos_t;
#endif

#define MATCHFINDER_NULL ((pos_t)-MATCHFINDER_WINDOW_SIZE)

/* In the sliding window case, positions are stored relative to 'in_base'.  */

static inline bool
matchfinder_node_valid(pos_t cur_node, const u8 *in_base, const u8 *in_next)
{
	return cur_node > (pos_t)((in_next - in_base) - MATCHFINDER_WINDOW_SIZE);
}

static inline pos_t
matchfinder_slot_for_match(pos_t cur_node)
{
	return cur_node & (MATCHFINDER_WINDOW_SIZE - 1);
}
