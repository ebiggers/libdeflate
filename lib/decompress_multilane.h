/*
 * decompress_multilane.h - multi-lane speculative decoding of Huffman blocks
 *
 * Included by decompress_template.h when MLSD_ENABLED (see
 * deflate_decompress.c: on by default on Apple arm64, else only with
 * LIBDEFLATE_MLSD, where it's experimental).
 *
 * Decoding a Huffman block is a serial chain: the next table index isn't known
 * until the previous entry has been loaded and its bits consumed.  But Huffman
 * codes self-synchronize: decoding from an arbitrary bit position soon lands
 * on a true symbol boundary (on real DEFLATE data, after ~100 bits on average),
 * and from then on decodes exactly the true symbols.
 *
 * So decode several "lanes" of the same block at once: lane 0 starts at the
 * known position, lanes 1..K-1 start at guessed positions D, 2D, ... bits
 * further on, where D is a little less than the distance a lane is expected to
 * cover in a round.  Each lane decodes 'nsteps' symbols into its own record
 * buffer, with no data-dependent branches, and the lanes run interleaved so
 * that their dependency chains overlap.
 *
 * Then the lanes are stitched together.  Lane i+1 takes over from lane i at
 * the position where lane i ended, if that is also a symbol boundary of lane
 * i+1 (found by binary search over lane i+1's positions); since decoding is
 * deterministic, the two lanes decode the same symbols from there on, so lane
 * i+1 is correct from there if lane i was.  If lane i+1 hasn't synchronized
 * yet at that point, lane i is extended serially ("catch-up") until it lands
 * on a boundary of lane i+1.  Anything that doesn't verify is simply discarded
 * and redone.
 *
 * The verified records of a round are executed while the lanes of the next
 * round run, in the same loop: the lanes are bound by ALU throughput, while
 * executing records is bound by stores and branch mispredictions, so the two
 * overlap well.  The executor always copies 32, 48, or 64 bytes per record
 * (chosen per block from the length code); longer matches, matches with offset
 * < 16, and the end-of-block symbol take a slow path.
 *
 * The lanes use their own 64-bit decode table entries, which are built
 * directly from the codeword lengths, instead of the normal tables (those are
 * built only if the block isn't finished here).  Litlen entries:
 *
 *	Bit 63:     1 if "slow" (EOB, or the match length can exceed the
 *		    number of bytes the executor always copies)
 *	Bit 58-32:  record template (bits 26-0 of the record, see below), or
 *		    for a subtable pointer, the subtable's start index
 *	Bit 31-27:  extra length bits mask
 *	Bit 21-16:  codeword length (number of subtable bits, if pointer)
 *	Bit 14:     1 if subtable pointer (MLSD_SUB)
 *	Bit 8:      1 if no offset follows: literal(s) or EOB (MLSD_NOOFF)
 *	Bit 5-0:    codeword length + extra bits (main table bits, if pointer)
 *
 * Offset entries:
 *
 *	Bit 63:     1 if "slow" (the offset can be < 16)
 *	Bit 47-32:  offset base, or for a subtable pointer, the subtable's
 *		    start index
 *	Bit 31-19:  extra offset bits mask
 *	Bit 14:     1 if subtable pointer (MLSD_SUB)
 *	Bit 13-8:   codeword length (number of subtable bits, if pointer)
 *	Bit 5-0:    codeword length + extra bits (main table bits, if pointer)
 *
 * The offset table has 256 extra "dummy" entries after the main table, at
 * indices 256-511, that consume no bits and give offset MLSD_DUMMY_OFF; a lane
 * step ORs MLSD_NOOFF into the offset table index, so that literal and EOB
 * steps decode a harmless dummy offset instead of needing selects.
 * Subtables follow at index 512.
 *
 * Each record is a fully decoded symbol, ready to execute:
 *
 *	Bit 63:     slow flag from the offset entry
 *	Bit 47-32:  match offset (MLSD_DUMMY_OFF if no match)
 *	Bit 31:     slow flag from the litlen entry
 *	Bit 26-18:  match length (0 if no match, MLSD_EOB_LEN if end-of-block)
 *	Bit 17-16:  number of literals (0-2)
 *	Bit 15-0:   the literal(s)
 *
 * Other bits are garbage.
 */

/*
 * MLSD_LANES, MLSD_MAX_STEPS, MLSD_CATCHUP, and MLSD_NREC are defined in
 * deflate_decompress.c, since they size the work area in the decompressor.
 *
 * The number of steps per round, 'nsteps', is a power of 2 that grows within
 * a block (MLSD_MIN_STEPS, then 2x after MLSD_GROW1 records, 4x after
 * MLSD_GROW2 records): larger rounds have relatively less overlap between
 * lanes, but waste more work when the block ends early in the round.
 */
#ifndef MLSD_MIN_STEPS
#  define MLSD_MIN_STEPS	128
#endif
#ifndef MLSD_GROW1
#  define MLSD_GROW1	8192
#endif
#ifndef MLSD_GROW2
#  define MLSD_GROW2	32768
#endif
/*
 * Lane i+1 starts this many bits before where lane i is expected to end.  The
 * margin adapts: it grows by MLSD_CU_RATIO * MLSD_MARGIN_STEP bits whenever a
 * lane boundary needs a catch-up and shrinks by MLSD_MARGIN_STEP bits when it
 * doesn't, which settles where 1 / (1 + MLSD_CU_RATIO) of the boundaries need
 * a catch-up.  That balances the cost of overlap (decoded twice, in
 * parallel) against the cost of catch-ups (serial, about MLSD_CU_RATIO times
 * as expensive per step).
 */
#ifndef MLSD_MARGIN
#  define MLSD_MARGIN	256	/* initial value */
#endif
#ifndef MLSD_MARGIN_STEP
#  define MLSD_MARGIN_STEP	4
#endif
#ifndef MLSD_CU_RATIO
#  define MLSD_CU_RATIO	5
#endif
#define MLSD_MARGIN_MIN		32
#define MLSD_MARGIN_MAX		1024
/*
 * Gating: the multi-lane decoder is used for a dynamic Huffman block only if
 * the previous block (if any) produced at least MLSD_MIN_PREV_BLOCK_OUT bytes,
 * and at least MLSD_MIN_IN_BYTES bytes of input and enough output space
 * remain: MLSD_MIN_START_OUT bytes to start using it in a stream, after that
 * just enough for one round.  It has per-stream costs (the normal decode
 * tables still have to be built for the block where it stops near the end of
 * the stream, the first round, and the offset-checked first 32 KiB) that take
 * on the order of 100 KiB of output to recoup.
 */
#ifndef MLSD_MIN_PREV_BLOCK_OUT
#  define MLSD_MIN_PREV_BLOCK_OUT	4096
#endif
#ifndef MLSD_MIN_START_OUT
#  define MLSD_MIN_START_OUT	393216
#endif
/*
 * Adaptive gating, in two parts:
 *
 * - A per-block data check (MLSD_ADAPTIVE, on where measured arbitration isn't
 *   available, since on Apple cores it turned off too many winning blocks,
 *   e.g. in archives of binaries): once a block has executed
 *   MLSD_GATE_MIN_RECS records, if it has fewer than MLSD_GATE_MIN_ALT
 *   literal/match alternations per 1000 records (long runs of literals, as in
 *   poorly compressible data), the fastloop's branches predict well and the
 *   lanes can't win: hand the rest of the block to the fastloop and skip the
 *   next 1, 3, 7, then MLSD_GATE_MAX_SKIP eligible blocks.  (Ported from the
 *   AVX-512 version's gate; its bytes-per-record and lane-efficiency tests are
 *   left out, since on Apple cores files with long matches are among the
 *   biggest wins, and the adaptive margin keeps the efficiency high.)
 *
 * - Measured arbitration (MLSD_ARBITRATE; needs a clock): now and then, an
 *   eligible block is decoded both ways over part of it, and the times are
 *   compared.  Whichever of the two is currently used decodes up to a
 *   pseudo-random point in the first half of the block (the start of a block
 *   isn't typical, e.g. in tar archives).  Then the lanes run for
 *   at least MLSD_EVAL_WARM_ROUNDS rounds (so that their tables and buffers
 *   are in the cache, as they normally are) and then over a window of two
 *   parts, each at least one round and half of a quarter of the previous
 *   block's output (MLSD_EVAL_MIN_BYTES to MLSD_EVAL_MAX_BYTES in all), or up
 *   to the end of the block; the second part is timed.  Then the fastloop
 *   decodes the window again, timing the second part; the first part lets it
 *   warm up, since right after the lanes it runs much slower for a while
 *   (it then also finds the data in the cache, which errs on its side).
 *   Running averages of the two times per byte (not of their ratios, so that
 *   each part of the data counts as much as the time it takes: e.g. the zero
 *   padding in a tar archive is fast either way) decide which one the other
 *   blocks use, so that it adapts to the core it runs on (narrower cores
 *   favor the fastloop) and to the data.  One comparison counts as at most a
 *   2x difference and for at most 4 times the averages' total, since a
 *   thread switch in the middle of it inflates one of the times.  The
 *   decision changes when the averages favor the other one by more than
 *   MLSD_EVAL_HYST percent (after requiring the lanes to be MLSD_EVAL_BIAS
 *   percent faster).  Blocks expected to be shorter than MLSD_EVAL_MIN_BLOCK
 *   aren't measured, and use the lanes only if they're at least
 *   MLSD_EVAL_SMALL percent ahead, since the lanes' extra time per block (a
 *   few microseconds on narrow cores) counts for more there.  (Optionally,
 *   MLSD_EVAL_USE_BMIN instead measures that extra time and derives the
 *   shortest block worth the lanes.)  The comparisons are spaced 64 KiB,
 *   192 KiB, 448 KiB, ... up to MLSD_EVAL_PERIOD_MAX bytes of output apart,
 *   restarting at 256 KiB whenever the decision changes, so that they cost a
 *   small, bounded fraction of the time.  The state carries over between
 *   streams, but if a stream's data looks different (its first block's
 *   input bits per output byte differ by more than a factor of 1.5 from a
 *   running average of those of the data compared), it gets a comparison at
 *   its second eligible block (over at most MLSD_EVAL_STREAM_BYTES), which
 *   counts for half of the running averages (after scaling them to the same
 *   total, so that only their ratio carries over); if it disagrees with the
 *   decision, the spacing of the comparisons restarts at 64 KiB.
 *
 * Threads with a utility (or lower) QoS class, or in the background
 * ("darwinbg") state, which the scheduler often or always runs on the
 * efficiency cores, where the lanes lose, don't use the lanes at all
 * (MLSD_QOS_CHECK, MLSD_QOS_MAX_OFF; checked once per stream).  The measured
 * arbitration catches only part of that case (e.g. threads of higher QoS
 * that the scheduler moves there under load): on narrow cores, the windows
 * still read the lanes as only somewhat slower when whole blocks show them
 * 20-30% slower.
 */
#if defined(__APPLE__) && !defined(FREESTANDING)
#  define MLSD_HAVE_CLOCK	1
#else
#  define MLSD_HAVE_CLOCK	0
#endif
#ifndef MLSD_ARBITRATE
#  define MLSD_ARBITRATE	MLSD_HAVE_CLOCK
#endif
#ifndef MLSD_ADAPTIVE
#  define MLSD_ADAPTIVE		(!MLSD_ARBITRATE)
#endif
#ifndef MLSD_GATE_MIN_RECS
#  define MLSD_GATE_MIN_RECS	2048
#endif
#ifndef MLSD_GATE_MIN_ALT
#  define MLSD_GATE_MIN_ALT	32
#endif
#ifndef MLSD_GATE_MAX_SKIP
#  define MLSD_GATE_MAX_SKIP	8
#endif
#ifndef MLSD_QOS_CHECK
#  define MLSD_QOS_CHECK	MLSD_HAVE_CLOCK
#endif
#ifndef MLSD_QOS_MAX_OFF	/* highest QoS class not using the lanes */
#  define MLSD_QOS_MAX_OFF	QOS_CLASS_UTILITY
#endif
#ifndef MLSD_EVAL_MIN_BYTES
#  define MLSD_EVAL_MIN_BYTES	4096
#endif
#ifndef MLSD_EVAL_MAX_BYTES
#  define MLSD_EVAL_MAX_BYTES	16384
#endif
#ifndef MLSD_EVAL_STREAM_BYTES	/* for a stream's first comparison */
#  define MLSD_EVAL_STREAM_BYTES	8192
#endif
#ifndef MLSD_EVAL_WARM_ROUNDS	/* rounds before the measured ones */
#  define MLSD_EVAL_WARM_ROUNDS	1
#endif
#ifndef MLSD_EVAL_MIN_ROUNDS	/* min rounds measured */
#  define MLSD_EVAL_MIN_ROUNDS	1
#endif
#ifndef MLSD_EVAL_MIN_BLOCK	/* min expected block size to measure */
#  define MLSD_EVAL_MIN_BLOCK	16384
#endif
#ifndef MLSD_EVAL_HYST
#  define MLSD_EVAL_HYST	2
#endif
#ifndef MLSD_EVAL_PERIOD_MAX
#  define MLSD_EVAL_PERIOD_MAX	(8 << 20)
#endif
#ifndef MLSD_EVAL_PERIOD_MIN
#  define MLSD_EVAL_PERIOD_MIN	65536
#endif
#ifndef MLSD_EVAL_PERIOD_FLIP
#  define MLSD_EVAL_PERIOD_FLIP	262144
#endif
/* score scale (see mlsd_record_eval()), and the hysteresis in score units */
#define MLSD_SCORE_ONE		4096
#define MLSD_SCORE_HYST		(MLSD_EVAL_HYST * MLSD_SCORE_ONE / 100)
#ifndef MLSD_EVAL_BIAS		/* percent the lanes must be faster by */
#  define MLSD_EVAL_BIAS	0
#endif
#define MLSD_SCORE_BIAS		(MLSD_EVAL_BIAS * MLSD_SCORE_ONE / 100)
#ifndef MLSD_EVAL_USE_BMIN	/* model the lanes' extra time per block? */
#  define MLSD_EVAL_USE_BMIN	0
#endif
/* how many percent ahead the lanes must be to be used on blocks too short to
   be measured */
#ifndef MLSD_EVAL_SMALL
#  define MLSD_EVAL_SMALL	15
#endif
#define MLSD_SCORE_SMALL	(MLSD_EVAL_SMALL * MLSD_SCORE_ONE / 100)
/* What to do with an eligible block */
#define MLSD_MODE_OFF		0	/* fastloop only */
#define MLSD_MODE_LANES		1	/* lanes (fastloop if they stop) */
#define MLSD_MODE_EVAL		2	/* a measured comparison, then the
					   decision */

#ifndef MLSD_DUMMY_OFF
#  define MLSD_DUMMY_OFF	4096	/* any offset >= 16 that's always valid in
					   the unchecked executor */
#endif
/*
 * Cost of copying 16 more bytes for every record, relative to the cost of a
 * slow-path record, in units of 2^-15 (i.e. a slow-path record costs as much
 * as 32768 / MLSD_COPY_COST extra 16-byte copies).
 */
#ifndef MLSD_COPY_COST
#  define MLSD_COPY_COST	250
#endif
#define MLSD_MAX_COPY		64	/* max bytes copied for every record */
#define MLSD_MAX_STEP_BITS	(LENGTH_MAXBITS + OFFSET_MAXBITS)
#ifndef MLSD_MIN_IN_BYTES
#  define MLSD_MIN_IN_BYTES	16384
#endif
#define MLSD_MAX_OUT_PER_REC	(2 + DEFLATE_MAX_MATCH_LEN)
#define MLSD_OUT_SLOP		(MLSD_MAX_COPY + 96)
/* Output space needed to execute one lane's records, with 'n' steps/round */
#define MLSD_LANE_OUT_BYTES(n)	\
	(((n) + MLSD_CATCHUP) * MLSD_MAX_OUT_PER_REC + MLSD_OUT_SLOP)
#define MLSD_MIN_OUT_BYTES	MLSD_LANE_OUT_BYTES(MLSD_MIN_STEPS)
#define MLSD_EOB_LEN		511

#ifndef MLSD_REC_DEFINED
#define MLSD_REC_DEFINED
#if defined(__clang__)
#  define MLSD_UNROLL_LOOP	_Pragma("clang loop unroll(full)")
#elif GCC_PREREQ(8, 0)
#  define MLSD_UNROLL_LOOP	_Pragma("GCC unroll 16")
#else
#  define MLSD_UNROLL_LOOP
#endif

#define MLSD_SUB		((u64)1 << 14)
#define MLSD_NOOFF		((u64)1 << 8)
#define MLSD_SLOW		(((u64)1 << 63) | ((u64)1 << 31))
#define MLSD_L_CW_SHIFT		16
#define MLSD_O_CW_SHIFT		8
#define MLSD_O_SUB_BASE		512	/* offset subtables start here */

static const u8 mlsd_dummy_src[64];

#if MLSD_HAVE_CLOCK
#  include <mach/mach_time.h>
#  include <sys/qos.h>
#  include <sys/resource.h>
#  ifndef PRIO_DARWIN_THREAD	/* (hidden by a strict _POSIX_C_SOURCE) */
#    define PRIO_DARWIN_THREAD	3
#  endif
#  ifndef PRIO_DARWIN_PROCESS
#    define PRIO_DARWIN_PROCESS	4
#  endif
/* A cheap monotonic clock, in arbitrary units */
static forceinline u64
mlsd_now(void)
{
	return mach_absolute_time();
}
/*
 * Is this thread likely to run on the efficiency cores: does it have a low
 * QoS class (utility or lower), or is it or its process in the background
 * ("darwinbg") state, e.g. from 'taskpolicy -b' or a launchd job with
 * ProcessType=Background, where its threads still report the QoS class they
 * asked for?  (The latter takes two system calls, about 0.1 us each.)
 */
static forceinline bool
mlsd_low_priority(void)
{
	const qos_class_t q = qos_class_self();

	if (q != QOS_CLASS_UNSPECIFIED && q <= MLSD_QOS_MAX_OFF)
		return true;
	return getpriority(PRIO_DARWIN_PROCESS, 0) > 0 ||
	       getpriority(PRIO_DARWIN_THREAD, 0) > 0;
}
#else
static forceinline u64
mlsd_now(void)
{
	return 0;
}
static forceinline bool
mlsd_low_priority(void)
{
	return false;
}
#endif

/*
 * Choose what to do with an eligible block, expected to produce about
 * 'est_bytes' of output; the previous block took 'bpb' input bits per output
 * byte, times 64 (0 if there wasn't one).  'can_eval' says whether there's
 * room for a measured comparison here.  '*qos' holds the state of the
 * current call: 0 = no eligible block yet, 1 = ok (priority check passed),
 * 2 = low priority, 3 = ok and past the stream's second eligible block.
 */
static forceinline int
mlsd_choose_mode(struct libdeflate_decompressor *d, int *qos,
		 u32 est_bytes, u32 bpb, bool can_eval)
{
	if (*qos == 0) {
		/* the first eligible block of this stream */
		*qos = (MLSD_QOS_CHECK && mlsd_low_priority()) ? 2 : 1;
	} else if (*qos == 1) {
		/*
		 * The second one: if the stream's data looks different from
		 * the data last compared, compare now.
		 */
		*qos = 3;
		if (MLSD_ARBITRATE && d->mlsd_evals != 0 && bpb != 0 &&
		    (2 * (u64)bpb > 3 * (u64)d->mlsd_bpb ||
		     3 * (u64)bpb < 2 * (u64)d->mlsd_bpb)) {
			d->mlsd_countdown = 0;
			d->mlsd_stream_eval = true;
			d->mlsd_bpb = bpb;
		}
	}
	if (MLSD_QOS_CHECK && *qos == 2)
		return MLSD_MODE_OFF;
#ifdef MLSD_TEST_RANDOM_MODE
	/* testing: exercise all the paths, in a reproducible way */
	d->mlsd_test_rng = d->mlsd_test_rng * 1103515245 + 12345;
	switch ((d->mlsd_test_rng >> 16) & 3) {
	case 0:
		return MLSD_MODE_OFF;
	case 1:
		return MLSD_MODE_LANES;
	default:
		return can_eval ? MLSD_MODE_EVAL : MLSD_MODE_LANES;
	}
#endif
	if (MLSD_ADAPTIVE && d->mlsd_skip_left) {
		/* a recent block failed the data check */
		d->mlsd_skip_left--;
		return MLSD_MODE_OFF;
	}
	if (MLSD_ARBITRATE) {
		if (d->mlsd_countdown == 0 &&
		    est_bytes >= MLSD_EVAL_MIN_BLOCK && can_eval) {
			/* (a running average, for the check above) */
			if (bpb != 0)
				d->mlsd_bpb = d->mlsd_bpb == 0 ? bpb :
					      (3 * d->mlsd_bpb + bpb) / 4;
			return MLSD_MODE_EVAL;
		}
		d->mlsd_countdown -= MIN(d->mlsd_countdown, est_bytes);
		if (d->mlsd_lanes_on && est_bytes >= d->mlsd_bmin)
			return MLSD_MODE_LANES;
		return MLSD_MODE_OFF;
	}
	return MLSD_MODE_LANES;
}

/*
 * Record a measured comparison: the lanes took 'tl' clock units for 'bl'
 * output bytes, and the fastloop 'tf' for 'bf', in the steady state; and the
 * lanes take about 'ovh' more per block (tables and first round).  If it's too
 * short to measure, leave the countdown at 0, so the next eligible block tries
 * again.
 */
static forceinline void
mlsd_record_eval(struct libdeflate_decompressor *d, u64 tl, u64 bl,
		 u64 tf, u64 bf, u64 ovh)
{
	const bool was_on = d->mlsd_lanes_on;
	bool stream_eval;
	u64 l, f, lt, ft;
	s32 r;

	if (bl < MLSD_EVAL_MIN_BYTES / 2 || bf < MLSD_EVAL_MIN_BYTES / 2 ||
	    tl == 0 || tf == 0 || tl >= ((u64)1 << 24) || tf >= ((u64)1 << 24))
		return;
	/* time per MiB of output */
	l = (tl << 20) / bl;
	f = (tf << 20) / bf;
	if (l >= ((u64)1 << 24) || f >= ((u64)1 << 24))
		return;
	ovh = MIN(ovh, ((u64)1 << 24) - 1);
	/*
	 * Limit what one comparison can do, since a thread switch or an
	 * interrupt in the middle of either part inflates its time: it counts
	 * as at most a 2x difference, and (except as a stream's first one) for
	 * at most 4 times the averages' total.
	 */
	l = MIN(l, 2 * f);
	f = MIN(f, 2 * l);
	if (d->mlsd_evals != 0 && !d->mlsd_stream_eval &&
	    l + f > 4 * ((u64)d->mlsd_lt + d->mlsd_ft)) {
		const u64 cap = 4 * ((u64)d->mlsd_lt + d->mlsd_ft);
		const u64 sum = l + f;

		l = l * cap / sum;
		f = f * cap / sum;
	}
	/*
	 * Running averages.  The first comparison counts fully, a stream's
	 * first comparison for half, others for 1/8.
	 */
	if (d->mlsd_evals++ == 0) {
		lt = l;
		ft = f;
		d->mlsd_ovh = ovh;
	} else {
		lt = d->mlsd_lt;
		ft = d->mlsd_ft;
		if (d->mlsd_stream_eval) {
			const u64 sum = lt + ft;

			lt = (lt * (l + f) / sum + l) / 2;
			ft = (ft * (l + f) / sum + f) / 2;
			d->mlsd_ovh = (d->mlsd_ovh + ovh) / 2;
		} else {
			lt = (7 * lt + l) / 8;
			ft = (7 * ft + f) / 8;
			d->mlsd_ovh = (7 * d->mlsd_ovh + ovh) / 8;
		}
	}
	d->mlsd_lt = lt;
	d->mlsd_ft = ft;
	stream_eval = d->mlsd_stream_eval;
	d->mlsd_stream_eval = false;
	/* a score of 2(f - l)/(f + l) is about ln(f/l), but bounded */
	r = (s32)((s64)(2 * MLSD_SCORE_ONE) * ((s64)ft - (s64)lt) /
		  (s64)(ft + lt + 1)) - MLSD_SCORE_BIAS;
	/* decide, with hysteresis */
	if (d->mlsd_evals == 1)
		d->mlsd_lanes_on = r > 0;
	else if (d->mlsd_lanes_on)
		d->mlsd_lanes_on = r >= -MLSD_SCORE_HYST;
	else
		d->mlsd_lanes_on = r > MLSD_SCORE_HYST;
#ifdef MLSD_TEST_RANDOM_MODE
	d->mlsd_test_rng = d->mlsd_test_rng * 1103515245 + 12345;
	d->mlsd_lanes_on = (d->mlsd_test_rng >> 16) & 1;
#endif
	/*
	 * The lanes win on blocks where the steady-state difference makes up
	 * for their extra time per block.  By default, that's modeled simply:
	 * blocks too short to be measured use the lanes only if they're well
	 * ahead.  (Their extra time per block counts for more there, and on
	 * narrow cores it's several microseconds.)
	 */
	if (MLSD_EVAL_USE_BMIN)
		d->mlsd_bmin = ft <= lt ? 0xFFFFFFFF :
			       (u32)MIN(((u64)d->mlsd_ovh << 20) / (ft - lt),
					0xFFFFFFFF);
	else
		d->mlsd_bmin = r >= MLSD_SCORE_SMALL ? 0 : MLSD_EVAL_MIN_BLOCK;
	/*
	 * Compare less often while the decision holds.  Start over if a
	 * stream's re-check disagrees with the decision (its data may favor
	 * the other one, even if one comparison didn't change the decision).
	 */
	if (d->mlsd_evals == 1 || (stream_eval && (l < f) != d->mlsd_lanes_on))
		d->mlsd_period = MLSD_EVAL_PERIOD_MIN;
	else if (d->mlsd_lanes_on != was_on)
		d->mlsd_period = MLSD_EVAL_PERIOD_FLIP;
	else
		d->mlsd_period = MIN(2 * d->mlsd_period + MLSD_EVAL_PERIOD_MIN,
				     MLSD_EVAL_PERIOD_MAX);
	d->mlsd_countdown = d->mlsd_period;
}

/*
 * Static parts of the lane-format decode table entries, indexed by symbol.
 * mlsd_make_entry() adds the codeword length.  (Compare litlen_decode_results[]
 * and offset_decode_results[].)
 */
#define MLSD_LE_LIT(l)		(MLSD_NOOFF | ((u64)(l) << 32) | ((u64)1 << 48))
#define MLSD_LE_LIT4(l)		MLSD_LE_LIT(l), MLSD_LE_LIT((l) + 1), \
				MLSD_LE_LIT((l) + 2), MLSD_LE_LIT((l) + 3)
#define MLSD_LE_LIT16(l)	MLSD_LE_LIT4(l), MLSD_LE_LIT4((l) + 4), \
				MLSD_LE_LIT4((l) + 8), MLSD_LE_LIT4((l) + 12)
#define MLSD_LE_LEN(b, n, F)	(((u64)(b) << 50) | (n) |			\
				 ((u64)((1U << (n)) - 1) << 27) |		\
				 ((u64)((b) + (1U << (n)) - 1 > (F)) << 63))
/* The litlen entries depend on how many bytes the fast path copies, 'F'. */
#define MLSD_LITLEN_RESULTS(F) {					\
	MLSD_LE_LIT16(0),   MLSD_LE_LIT16(16),  MLSD_LE_LIT16(32),	\
	MLSD_LE_LIT16(48),  MLSD_LE_LIT16(64),  MLSD_LE_LIT16(80),	\
	MLSD_LE_LIT16(96),  MLSD_LE_LIT16(112), MLSD_LE_LIT16(128),	\
	MLSD_LE_LIT16(144), MLSD_LE_LIT16(160), MLSD_LE_LIT16(176),	\
	MLSD_LE_LIT16(192), MLSD_LE_LIT16(208), MLSD_LE_LIT16(224),	\
	MLSD_LE_LIT16(240),						\
	/* End of block */						\
	MLSD_NOOFF | ((u64)MLSD_EOB_LEN << 50) | ((u64)1 << 63),	\
	/* Lengths */							\
	MLSD_LE_LEN(3  , 0, F), MLSD_LE_LEN(4  , 0, F),			\
	MLSD_LE_LEN(5  , 0, F), MLSD_LE_LEN(6  , 0, F),			\
	MLSD_LE_LEN(7  , 0, F), MLSD_LE_LEN(8  , 0, F),			\
	MLSD_LE_LEN(9  , 0, F), MLSD_LE_LEN(10 , 0, F),			\
	MLSD_LE_LEN(11 , 1, F), MLSD_LE_LEN(13 , 1, F),			\
	MLSD_LE_LEN(15 , 1, F), MLSD_LE_LEN(17 , 1, F),			\
	MLSD_LE_LEN(19 , 2, F), MLSD_LE_LEN(23 , 2, F),			\
	MLSD_LE_LEN(27 , 2, F), MLSD_LE_LEN(31 , 2, F),			\
	MLSD_LE_LEN(35 , 3, F), MLSD_LE_LEN(43 , 3, F),			\
	MLSD_LE_LEN(51 , 3, F), MLSD_LE_LEN(59 , 3, F),			\
	MLSD_LE_LEN(67 , 4, F), MLSD_LE_LEN(83 , 4, F),			\
	MLSD_LE_LEN(99 , 4, F), MLSD_LE_LEN(115, 4, F),			\
	MLSD_LE_LEN(131, 5, F), MLSD_LE_LEN(163, 5, F),			\
	MLSD_LE_LEN(195, 5, F), MLSD_LE_LEN(227, 5, F),			\
	MLSD_LE_LEN(258, 0, F), MLSD_LE_LEN(258, 0, F),			\
	MLSD_LE_LEN(258, 0, F),						\
}
static const u64 mlsd_litlen_results[3][DEFLATE_NUM_LITLEN_SYMS] = {
	MLSD_LITLEN_RESULTS(32), MLSD_LITLEN_RESULTS(48),
	MLSD_LITLEN_RESULTS(64),
};

#define MLSD_OE(b, n)		(((u64)(b) << 32) | (n) |			\
				 ((u64)((1U << (n)) - 1) << 19) |		\
				 ((u64)((b) < 16) << 63))
static const u64 mlsd_offset_results[DEFLATE_NUM_OFFSET_SYMS] = {
	MLSD_OE(1     , 0)  , MLSD_OE(2     , 0)  , MLSD_OE(3     , 0)  ,
	MLSD_OE(4     , 0)  , MLSD_OE(5     , 1)  , MLSD_OE(7     , 1)  ,
	MLSD_OE(9     , 2)  , MLSD_OE(13    , 2)  , MLSD_OE(17    , 3)  ,
	MLSD_OE(25    , 3)  , MLSD_OE(33    , 4)  , MLSD_OE(49    , 4)  ,
	MLSD_OE(65    , 5)  , MLSD_OE(97    , 5)  , MLSD_OE(129   , 6)  ,
	MLSD_OE(193   , 6)  , MLSD_OE(257   , 7)  , MLSD_OE(385   , 7)  ,
	MLSD_OE(513   , 8)  , MLSD_OE(769   , 8)  , MLSD_OE(1025  , 9)  ,
	MLSD_OE(1537  , 9)  , MLSD_OE(2049  , 10) , MLSD_OE(3073  , 10) ,
	MLSD_OE(4097  , 11) , MLSD_OE(6145  , 11) , MLSD_OE(8193  , 12) ,
	MLSD_OE(12289 , 12) , MLSD_OE(16385 , 13) , MLSD_OE(24577 , 13) ,
	MLSD_OE(24577 , 13) , MLSD_OE(24577 , 13) ,
};

static forceinline u64
mlsd_make_entry(const u64 results[], u32 sym, u32 len, unsigned cw_shift)
{
	return results[sym] + len + ((u64)len << cw_shift);
}

/*
 * Like struct litlen_compound_syms, for building lane-format tables.  The
 * short_lits[] entries have the same format as in build_decode_table().
 */
struct mlsd_compound_syms {
	u16 lit_start[LITLEN_TABLEBITS];
	u16 lit_end[LITLEN_TABLEBITS];
	u8 lensym_start[LITLEN_TABLEBITS];
	u8 lensym_end[LITLEN_TABLEBITS];
	u64 short_lensym_res[MAX_NUM_SHORT_LENSYMS];
	u16 short_lensym_cw[MAX_NUM_SHORT_LENSYMS];
	unsigned min_lit_cw_len;
	unsigned min_lensym_cw_len;
};

/* Position 'i' of a lane, relative to the lane's start position 'base'. */
static forceinline unsigned
mlsd_rel(const u16 *pp, unsigned i, u16 base)
{
	return (u16)(pp[i] - base);
}
#endif /* MLSD_REC_DEFINED */

/*
 * Lane-format version of fill_compound_litlen_entries(); see there.
 */
static NOINLINE ATTRIBUTES void
ADD_SUFFIX(mlsd_fill_compound)(u64 * restrict decode_table,
			       const u32 * restrict short_lits,
			       const struct mlsd_compound_syms * restrict cs,
			       unsigned total_cw_len)
{
	const unsigned min_lit_cw_len = cs->min_lit_cw_len;
	const unsigned min_lensym_cw_len = cs->min_lensym_cw_len;
	unsigned len1;

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
				u64 *dst = &decode_table[
					(lit2 & SHORT_LIT_CODEWORD_MASK) << len1];
				u64 base = MLSD_NOOFF | total_cw_len |
					   ((u64)(lit2 >> HUFFDEC_LITERAL1_SHIFT)
					    << 40) | ((u64)2 << 48);
				unsigned i1;

				for (i1 = s1; i1 < e1; i1++) {
					u32 lit1 = short_lits[i1];

					dst[lit1 & SHORT_LIT_CODEWORD_MASK] =
						base |
						((u64)(lit1 >>
						       HUFFDEC_LITERAL1_SHIFT)
						 << 32);
				}
			}
		}
	}

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
				u64 *dst = &decode_table[
					(unsigned)cs->short_lensym_cw[i2] << len1];
				u64 base = cs->short_lensym_res[i2] +
					   total_cw_len +
					   ((u64)total_cw_len << MLSD_L_CW_SHIFT) +
					   ((u64)1 << 48);
				unsigned i1;

				for (i1 = s1; i1 < e1; i1++) {
					u32 lit1 = short_lits[i1];

					dst[lit1 & SHORT_LIT_CODEWORD_MASK] =
						base |
						((u64)(lit1 >>
						       HUFFDEC_LITERAL1_SHIFT)
						 << 32);
				}
			}
		}
	}
}

/*
 * Lane-format version of build_decode_table(); see there.  The differences
 * are the 64-bit entries, the 'cw_shift' (position of the codeword length
 * field), 'sub_base' (where subtables start, if not right after the main
 * table), and a separate scratch buffer for the short literals.  It accepts
 * and rejects exactly the same codes.
 */
static ATTRIBUTES bool
ADD_SUFFIX(build_mlsd_table)(u64 decode_table[], const u8 lens[],
			     const unsigned num_syms,
			     const u64 decode_results[], unsigned table_bits,
			     unsigned max_codeword_len, u16 *sorted_syms,
			     const unsigned cw_shift, const unsigned sub_base,
			     unsigned *table_bits_ret, bool allow_compound)
{
	union {
		u16 c[4];
		u64 w;
	} part_counts[DEFLATE_MAX_CODEWORD_LEN + 1];
	u16 len_counts[DEFLATE_MAX_CODEWORD_LEN + 1] = { 0 };
	const unsigned part_len = num_syms / 4;
	unsigned sym;
	unsigned codeword;
	unsigned len;
	unsigned count;
	u32 codespace_used;
	unsigned cur_table_end;
	unsigned subtable_prefix;
	unsigned subtable_start;
	unsigned subtable_bits;
	struct mlsd_compound_syms cs;
	u32 short_lits[DEFLATE_NUM_LITERALS];
	unsigned num_short_lits = 0;
	unsigned num_short_lensyms = 0;
	unsigned min_compound_cw_len = LITLEN_TABLEBITS + 1;
	unsigned max_short_lit_cw_len = 0;
	unsigned max_short_lensym_cw_len = 0;

	memset(part_counts, 0, sizeof(part_counts));
	for (sym = 0; sym < part_len; sym++) {
		part_counts[lens[0 * part_len + sym]].c[0]++;
		part_counts[lens[1 * part_len + sym]].c[1]++;
		part_counts[lens[2 * part_len + sym]].c[2]++;
		part_counts[lens[3 * part_len + sym]].c[3]++;
	}
	for (sym = 4 * part_len; sym < num_syms; sym++)
		part_counts[lens[sym]].c[3]++;

	while (max_codeword_len > 1 && part_counts[max_codeword_len].w == 0)
		max_codeword_len--;

	codespace_used = 0;
	{
		unsigned a = part_counts[0].c[0];
		unsigned b = part_counts[0].c[1];
		unsigned c = part_counts[0].c[2];
		unsigned e = part_counts[0].c[3];

		part_counts[0].c[0] = 0;
		part_counts[0].c[1] = a;
		part_counts[0].c[2] = a + b;
		part_counts[0].c[3] = a + b + c;
		len_counts[0] = a + b + c + e;
		count = len_counts[0];
	}
	for (len = 1; len <= max_codeword_len; len++) {
		unsigned a = part_counts[len].c[0];
		unsigned b = part_counts[len].c[1];
		unsigned c = part_counts[len].c[2];
		unsigned e = part_counts[len].c[3];

		part_counts[len].c[0] = count;
		part_counts[len].c[1] = count + a;
		part_counts[len].c[2] = count + a + b;
		part_counts[len].c[3] = count + a + b + c;
		len_counts[len] = a + b + c + e;
		count += len_counts[len];
		codespace_used = (codespace_used << 1) + len_counts[len];
	}

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
	sorted_syms += len_counts[0];

	/* overfull code? */
	if (unlikely(codespace_used > (1U << max_codeword_len)))
		return false;

	/* incomplete code? */
	if (unlikely(codespace_used < (1U << max_codeword_len))) {
		u64 entry;
		unsigned i;

		if (codespace_used == 0) {
			sym = 0;
		} else {
			if (codespace_used != (1U << (max_codeword_len - 1)) ||
			    len_counts[1] != 1)
				return false;
			sym = sorted_syms[0];
		}
		if (table_bits_ret != NULL) {
			table_bits = 1;
			*table_bits_ret = 1;
		}
		entry = mlsd_make_entry(decode_results, sym, 1, cw_shift);
		for (i = 0; i < (1U << table_bits); i++)
			decode_table[i] = entry;
		return true;
	}

	codeword = 0;
	len = 1;
	while ((count = len_counts[len]) == 0)
		len++;
	if (table_bits_ret != NULL) {
		if (allow_compound) {
			unsigned sym_idx = 0;
			unsigned max_l = MIN(max_codeword_len, table_bits - 1);
			u32 lit_cw_len_mask = 0;
			u32 lensym_cw_len_mask = 0;
			unsigned min_lit_cw_len = LITLEN_TABLEBITS + 1;
			unsigned min_lensym_cw_len = LITLEN_TABLEBITS + 1;
			unsigned min_second_cw_len;
			unsigned l;

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

			if (lit_cw_len_mask != 0 &&
			    min_lit_cw_len + min_second_cw_len <= table_bits) {
				if (max_codeword_len < table_bits) {
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
						min_table_bits =
							MAX(min_table_bits,
							    l1 + bsr32(partner_mask));
						m &= m - 1;
					} while (m != 0);
					table_bits = min_table_bits;
				}
				min_compound_cw_len =
					min_lit_cw_len + min_second_cw_len;
				max_short_lit_cw_len =
					table_bits - min_second_cw_len;
				if (lensym_cw_len_mask != 0 &&
				    min_lit_cw_len + min_lensym_cw_len <=
				    table_bits)
					max_short_lensym_cw_len =
						table_bits - min_lit_cw_len;
				cs.min_lit_cw_len = min_lit_cw_len;
				cs.min_lensym_cw_len = min_lensym_cw_len;
				memset(cs.lit_start, 0, sizeof(cs.lit_start));
				memset(cs.lit_end, 0, sizeof(cs.lit_end));
				memset(cs.lensym_start, 0,
				       sizeof(cs.lensym_start));
				memset(cs.lensym_end, 0, sizeof(cs.lensym_end));
			} else {
				table_bits = MIN(LITLEN_SMALL_TABLEBITS,
						 max_codeword_len);
			}
		} else {
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

		do {
			unsigned bit;

			sym = *sorted_syms++;
			decode_table[codeword] =
				mlsd_make_entry(decode_results, sym, len,
						cw_shift);
			if (len <= max_short_lit_cw_len) {
				if (sym < DEFLATE_NUM_LITERALS) {
					short_lits[num_short_lits++] =
						MAKE_SHORT_LIT(sym, codeword);
				} else if (len <= max_short_lensym_cw_len &&
					   sym >= FIRST_SHORT_LENSYM &&
					   sym <= LAST_SHORT_LENSYM) {
					cs.short_lensym_res[num_short_lensyms] =
						decode_results[sym];
					cs.short_lensym_cw[num_short_lensyms] =
						(u16)codeword;
					num_short_lensyms++;
				}
			}

			if (codeword == cur_table_end - 1) {
				if (len <= max_short_lit_cw_len)
					cs.lit_end[len] = num_short_lits;
				if (len <= max_short_lensym_cw_len)
					cs.lensym_end[len] = num_short_lensyms;
				if (len >= min_compound_cw_len)
					ADD_SUFFIX(mlsd_fill_compound)(
							decode_table,
							short_lits, &cs, len);
				for (; len < table_bits; len++) {
					memcpy(&decode_table[cur_table_end],
					       decode_table,
					       cur_table_end *
						sizeof(decode_table[0]));
					cur_table_end <<= 1;
					if (len + 1 >= min_compound_cw_len)
						ADD_SUFFIX(mlsd_fill_compound)(
							decode_table,
							short_lits, &cs,
							len + 1);
				}
				return true;
			}
			bit = 1U << bsr32(codeword ^ (cur_table_end - 1));
			codeword &= bit - 1;
			codeword |= bit;
		} while (--count);

		if (len <= max_short_lit_cw_len)
			cs.lit_end[len] = num_short_lits;
		if (len <= max_short_lensym_cw_len)
			cs.lensym_end[len] = num_short_lensyms;
		if (len >= min_compound_cw_len)
			ADD_SUFFIX(mlsd_fill_compound)(decode_table,
						       short_lits, &cs, len);

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
				ADD_SUFFIX(mlsd_fill_compound)(
						decode_table, short_lits, &cs,
						len);
		}
	}

	/* Process codewords with len > table_bits.  These require subtables. */
	cur_table_end = sub_base ? sub_base : 1U << table_bits;
	subtable_prefix = -1;
	subtable_start = 0;
	for (;;) {
		u64 entry;
		unsigned i;
		unsigned stride;
		unsigned bit;

		if ((codeword & ((1U << table_bits) - 1)) != subtable_prefix) {
			subtable_prefix = (codeword & ((1U << table_bits) - 1));
			subtable_start = cur_table_end;
			subtable_bits = len - table_bits;
			codespace_used = count;
			while (codespace_used < (1U << subtable_bits)) {
				subtable_bits++;
				codespace_used = (codespace_used << 1) +
					len_counts[table_bits + subtable_bits];
			}
			cur_table_end = subtable_start + (1U << subtable_bits);
			decode_table[subtable_prefix] =
				((u64)subtable_start << 32) | MLSD_SUB |
				((u64)subtable_bits << cw_shift) | table_bits;
		}

		entry = mlsd_make_entry(decode_results, *sorted_syms++,
					len - table_bits, cw_shift);
		i = subtable_start + (codeword >> table_bits);
		stride = 1U << (len - table_bits);
		do {
			decode_table[i] = entry;
			i += stride;
		} while (i < cur_table_end);

		if (codeword == (1U << len) - 1)
			return true;
		bit = 1U << bsr32(codeword ^ ((1U << len) - 1));
		codeword &= bit - 1;
		codeword |= bit;
		count--;
		while (count == 0)
			count = len_counts[++len];
	}
}

/*
 * Build the lane-format litlen and offset decode tables from d->u.l.lens.
 * This leaves 'lens' intact, so the normal tables can still be built later.
 */
static ATTRIBUTES bool
ADD_SUFFIX(build_mlsd_tables_body)(struct libdeflate_decompressor * restrict d,
				   unsigned num_litlen_syms,
				   unsigned num_offset_syms)
{
	if (!ADD_SUFFIX(build_mlsd_table)(d->mlsd_offset_table,
					  d->u.l.lens + num_litlen_syms,
					  num_offset_syms, mlsd_offset_results,
					  OFFSET_TABLEBITS,
					  DEFLATE_MAX_OFFSET_CODEWORD_LEN,
					  d->sorted_syms, MLSD_O_CW_SHIFT,
					  MLSD_O_SUB_BASE, NULL, false))
		return false;
	if (!d->mlsd_dummy_init) {
		unsigned i;

		for (i = 1U << OFFSET_TABLEBITS; i < MLSD_O_SUB_BASE; i++)
			d->mlsd_offset_table[i] = (u64)MLSD_DUMMY_OFF << 32;
		d->mlsd_dummy_init = true;
	}
	{
		/*
		 * Choose how many bytes the executor copies for every record
		 * (32, 48, or 64), trading the cost of the extra copying
		 * against the branch mispredictions of the slow path for
		 * longer matches.  Estimate the probability of each length
		 * symbol from its codeword length (in units of 2^-15).
		 * Length symbols 272+, 274+, and 276+ can exceed 32, 48, and
		 * 64 bytes respectively.
		 */
		const u8 *lens = d->u.l.lens + DEFLATE_FIRST_LEN_SYM;
		const unsigned n = num_litlen_syms - DEFLATE_FIRST_LEN_SYM;
		u32 p[3] = { 0, 0, 0 };
		unsigned k, c = 0;

		for (k = 15; k < n; k++) {
			const u32 w = lens[k] ? 1U << (15 - lens[k]) : 0;

			p[0] += w;
			p[1] += k >= 17 ? w : 0;
			p[2] += k >= 19 ? w : 0;
		}
		if (p[1] + MLSD_COPY_COST < p[c])
			c = 1;
		if (p[2] + 2 * MLSD_COPY_COST < p[c] + c * MLSD_COPY_COST)
			c = 2;
		d->mlsd_copy = 32 + 16 * c;
		return ADD_SUFFIX(build_mlsd_table)(
				d->mlsd_litlen_table, d->u.l.lens,
				num_litlen_syms, mlsd_litlen_results[c],
				LITLEN_TABLEBITS,
				DEFLATE_MAX_LITLEN_CODEWORD_LEN, d->sorted_syms,
				MLSD_L_CW_SHIFT, 0, &d->mlsd_litlen_tablebits,
				true);
	}
}

static forceinline bool
ADD_SUFFIX(build_mlsd_tables)(struct libdeflate_decompressor * restrict d,
			      unsigned num_litlen_syms, unsigned num_offset_syms)
{
	return ADD_SUFFIX(build_mlsd_tables_body)(d, num_litlen_syms,
						  num_offset_syms);
}

/* Resolve a subtable pointer into an entry that looks like a main-table one. */
static forceinline u64
ADD_SUFFIX(mlsd_sub)(const u64 *table, u64 entry, u64 bits,
		     const unsigned cw_shift)
{
	const u32 mb = entry & 63;
	const u64 sub = table[(u32)(entry >> 32) +
			      ((bits >> mb) &
			       ((1U << ((entry >> cw_shift) & 63)) - 1))];

	return sub + mb + ((u64)mb << cw_shift);
}

/*
 * Decode one symbol (a litlen symbol, plus the offset symbol if it was a
 * length) at bit position 'p', with no data-dependent branches except for
 * the rare subtable lookups.  Writes the record to *rp and the position to
 * *pp, and advances 'p'.
 */
#define MLSD_STEP(p, rp, pp)						\
do {									\
	const u64 w_ = get_unaligned_le64(in + ((p) >> 3)) >> ((p) & 7);\
	u64 e1_ = lt[w_ & lmask];					\
	u64 w1_, e2_, x1_, x2_, off_;					\
									\
	if (unlikely(e1_ & MLSD_SUB))					\
		e1_ = ADD_SUFFIX(mlsd_sub)(lt, e1_, w_, MLSD_L_CW_SHIFT);\
	w1_ = w_ >> (e1_ & 63);						\
	e2_ = ot[(w1_ & 0xFF) | (e1_ & MLSD_NOOFF)];			\
	if (unlikely(e2_ & MLSD_SUB))					\
		e2_ = ADD_SUFFIX(mlsd_sub)(ot, e2_, w1_, MLSD_O_CW_SHIFT);\
	x1_ = (w_ >> ((e1_ >> MLSD_L_CW_SHIFT) & 63)) &			\
	      ((u32)e1_ >> 27);						\
	x2_ = (w1_ >> ((e2_ >> MLSD_O_CW_SHIFT) & 63)) &		\
	      ((u32)e2_ >> 19);						\
	off_ = (e2_ >> 32) + x2_;					\
	*(rp) = (e1_ >> 32) + (x1_ << 18) + (off_ << 32);		\
	*(pp) = (u16)(p);						\
	(p) += (u8)e1_ + (u8)e2_;					\
} while (0)

/* Run all lanes for 'nsteps' steps. */
static NOINLINE ATTRIBUTES void
ADD_SUFFIX(mlsd_lanes)(const u8 * restrict in, size_t * restrict posv,
		       const u64 * restrict lt, const u64 * restrict ot,
		       const u32 lmask, u64 (* restrict recs)[MLSD_NREC],
		       u16 (* restrict poss)[MLSD_NREC + 1],
		       const unsigned nsteps)
{
	size_t pos[MLSD_LANES];
	int i, n;

	for (i = 0; i < MLSD_LANES; i++)
		pos[i] = posv[i];
	for (n = 0; n < (int)nsteps; n++) {
		MLSD_UNROLL_LOOP
		for (i = 0; i < MLSD_LANES; i++)
			MLSD_STEP(pos[i], &recs[i][n], &poss[i][n]);
	}
	for (i = 0; i < MLSD_LANES; i++) {
		poss[i][nsteps] = (u16)pos[i];
		posv[i] = pos[i];
	}
}

/*
 * Extend lane 'i' (whose position is *pp, having decoded *cntp records) until
 * it lands on a symbol boundary of the next lane, whose positions relative to
 * its start are mlsd_rel(pp1, 0..nsteps, b1) and whose start position is
 * 'base1'.  'h' is where to start looking among the next lane's positions.
 * Returns the index of the matching position in the next lane, or -1 if none
 * was found within MLSD_CATCHUP extra steps (the input was checked for that).
 */
static NOINLINE ATTRIBUTES int
ADD_SUFFIX(mlsd_catchup)(const u8 * restrict in, size_t * restrict pp,
			 const u64 * restrict lt, const u64 * restrict ot,
			 const u32 lmask, u64 * restrict rec,
			 u16 * restrict poss, unsigned * restrict cntp,
			 const u16 * restrict pp1, size_t base1, unsigned h,
			 const unsigned nsteps)
{
	size_t p = *pp;
	unsigned cnt = *cntp;
	const u16 b1 = (u16)base1;
	int ret = -1;

	while (cnt < nsteps + MLSD_CATCHUP) {
		ptrdiff_t t;

		MLSD_STEP(p, &rec[cnt], &poss[cnt]);
		cnt++;
		poss[cnt] = (u16)p;
		t = (ptrdiff_t)(p - base1);
		if (t <= 0)
			continue;
		while (h <= nsteps && mlsd_rel(pp1, h, b1) < (size_t)t)
			h++;
		if (h > nsteps)
			break;
		if (mlsd_rel(pp1, h, b1) == (size_t)t) {
			ret = h;
			break;
		}
	}
	*pp = p;
	*cntp = cnt;
	return ret;
}

/*
 * Copy a match with offset < 16, or a long match.  'out' is where the match
 * goes; up to MLSD_OUT_SLOP bytes may be written past out + len.
 */
static forceinline void
ADD_SUFFIX(mlsd_copy_slow)(u8 *out, const u8 *src, u32 off, u32 len)
{
	u8 *d = out;
	const u8 *s = src;
	u8 * const dend = out + len;

	if (off >= 16) {
		do {
			memcpy(d, s, 16);
			memcpy(d + 16, s + 16, 16);
			memcpy(d + 32, s + 32, 16);
			memcpy(d + 48, s + 48, 16);
			d += 64;
			s += 64;
		} while (d < dend);
		return;
	}
#if defined(ARCH_ARM64) && HAVE_NEON_NATIVE && CPU_IS_LITTLE_ENDIAN() && \
	!defined(MLSD_NO_NEON)
	{
		/* Same approach as copy_match_neon() */
		static const u8 _aligned_attribute(16) mlsd_repeat[16][16] = {
			{ 0, 16, 16, 15, 16, 15, 12, 14, 16, 9, 10, 11, 12, 13, 14, 15 },
			{ 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
			{ 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1 },
			{ 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0, 1, 2, 0 },
			{ 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3, 0, 1, 2, 3 },
			{ 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0, 1, 2, 3, 4, 0 },
			{ 0, 1, 2, 3, 4, 5, 0, 1, 2, 3, 4, 5, 0, 1, 2, 3 },
			{ 0, 1, 2, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6, 0, 1 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 0, 1, 2, 3, 4, 5, 6, 7 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 0, 1, 2, 3, 4, 5, 6 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1, 2, 3, 4, 5 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 0, 1, 2, 3, 4 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 0, 1, 2, 3 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 0, 1, 2 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 0, 1 },
			{ 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 0 },
		};
		const uint8x16_t v = vqtbl1q_u8(vld1q_u8(s),
						vld1q_u8(mlsd_repeat[off]));
		const size_t step = mlsd_repeat[0][off];

		vst1q_u8(d, v);
		d += step;
		vst1q_u8(d, v);
		d += step;
		while (d < dend) {
			vst1q_u8(d, v);
			d += step;
		}
	}
#else
	if (off >= 8) {
		do {
			store_word_unaligned(load_word_unaligned(s), d);
			s += 8;
			d += 8;
		} while (d < dend);
	} else if (off == 1) {
		const u64 v = (u64)0x0101010101010101 * s[0];

		do {
			put_unaligned_le64(v, d);
			put_unaligned_le64(v, d + 8);
			d += 16;
		} while (d < dend);
	} else {
		const u64 v = get_unaligned_le64(s);

		do {
			put_unaligned_le64(v, d);
			d += off;
		} while (d < dend);
	}
#endif
}

/*
 * Execute 'n' records starting at 'r'.  Returns the number of records
 * executed: n, or fewer if an end-of-block record was reached (it isn't
 * executed), or (size_t)-1 if a match offset was invalid.
 */
static forceinline size_t
ADD_SUFFIX(mlsd_exec_body)(const u64 * restrict r, size_t n,
			   u8 ** restrict out_p, const u8 * restrict out_start,
			   const unsigned copy, const bool check_offset)
{
	u8 *out = *out_p;
	size_t j;

	for (j = 0; j < n; j++) {
		const u64 x = r[j];
		const u8 *src;

		put_unaligned_le16((u16)x, out);
		out += (x >> 16) & 3;
		src = out - ((x >> 32) & 0xFFFF);
		if (check_offset) {
			const u32 len = (x >> 18) & 0x1FF;
			const u32 off = (x >> 32) & 0xFFFF;

			if (len == MLSD_EOB_LEN)
				break;
			if (len == 0)
				src = mlsd_dummy_src;
			else if (unlikely(off > (size_t)(out - out_start)))
				return (size_t)-1;
		}
		if (likely(!(x & MLSD_SLOW))) {
			memcpy(out, src, 16);
			memcpy(out + 16, src + 16, 16);
			if (copy > 32) {
				memcpy(out + 32, src + 32, 16);
				if (copy > 48)
					memcpy(out + 48, src + 48, 16);
			}
		} else {
			const u32 len = (x >> 18) & 0x1FF;

			if (len == MLSD_EOB_LEN)
				break;
			ADD_SUFFIX(mlsd_copy_slow)(out, src,
						   (x >> 32) & 0xFFFF, len);
		}
		out += (x >> 18) & 0x1FF;
	}
	*out_p = out;
	return j;
}

static NOINLINE ATTRIBUTES size_t
ADD_SUFFIX(mlsd_exec_checked)(const u64 * restrict r, size_t n,
			      u8 ** restrict out_p,
			      const u8 * restrict out_start, unsigned copy)
{
	return ADD_SUFFIX(mlsd_exec_body)(r, n, out_p, out_start, copy, true);
}

static NOINLINE ATTRIBUTES size_t
ADD_SUFFIX(mlsd_exec_unchecked)(const u64 * restrict r, size_t n,
				u8 ** restrict out_p,
				const u8 * restrict out_start, unsigned copy)
{
	return ADD_SUFFIX(mlsd_exec_body)(r, n, out_p, out_start, copy, false);
}


/*
 * Execute the record 'x' at 'out', in unchecked mode (at least
 * DEFLATE_MAX_MATCH_OFFSET bytes have been written).  At an end-of-block
 * record, does 'eob_action' instead.
 */
#define MLSD_EXEC_ONE(x, out, copy, eob_action)				\
do {									\
	const u8 *src_;							\
									\
	put_unaligned_le16((u16)(x), (out));				\
	(out) += ((x) >> 16) & 3;					\
	src_ = (out) - (((x) >> 32) & 0xFFFF);				\
	if (likely(!((x) & MLSD_SLOW))) {				\
		memcpy((out), src_, 16);				\
		memcpy((out) + 16, src_ + 16, 16);			\
		if ((copy) > 32) {					\
			memcpy((out) + 32, src_ + 32, 16);		\
			if ((copy) > 48)				\
				memcpy((out) + 48, src_ + 48, 16);	\
		}							\
	} else {							\
		const u32 len_ = ((x) >> 18) & 0x1FF;			\
									\
		if (len_ == MLSD_EOB_LEN)				\
			eob_action;					\
		ADD_SUFFIX(mlsd_copy_slow)((out), src_,			\
					   ((x) >> 32) & 0xFFFF, len_);	\
	}								\
	(out) += ((x) >> 18) & 0x1FF;					\
} while (0)

/*
 * Run all lanes for 'nsteps' steps, like mlsd_lanes(), while also executing
 * the records xp[0 .. xend - xp) of the previous round, R of them per step.
 * The lanes are bound by ALU throughput and the record execution by stores, so
 * the two overlap well.  Requires unchecked mode and enough output space for
 * the records.  Returns a pointer to the end-of-block record if one was
 * reached (the lanes are then left unfinished), else xend.
 */
static NOINLINE ATTRIBUTES const u64 *
ADD_SUFFIX(mlsd_lanes_exec)(const u8 * restrict in, size_t * restrict posv,
			    const u64 * restrict lt, const u64 * restrict ot,
			    const u32 lmask, u64 (* restrict recs)[MLSD_NREC],
			    u16 (* restrict poss)[MLSD_NREC + 1],
			    const u64 * restrict xp, const u64 * const xend,
			    const size_t R, u8 ** restrict out_p,
			    const unsigned copy, const unsigned nsteps)
{
	size_t pos[MLSD_LANES];
	u8 *out = *out_p;
	int i, n;

	for (i = 0; i < MLSD_LANES; i++)
		pos[i] = posv[i];
	for (n = 0; n < (int)nsteps; n++) {
		MLSD_UNROLL_LOOP
		for (i = 0; i < MLSD_LANES; i++)
			MLSD_STEP(pos[i], &recs[i][n], &poss[i][n]);
		if ((size_t)(xend - xp) >= R) {
			size_t k;

			for (k = 0; k < R; k++) {
				const u64 x = *xp++;

				MLSD_EXEC_ONE(x, out, copy, { xp--; goto eob; });
			}
		}
	}
	while (xp != xend) {
		const u64 x = *xp++;

		MLSD_EXEC_ONE(x, out, copy, { xp--; goto eob; });
	}
	for (i = 0; i < MLSD_LANES; i++) {
		poss[i][nsteps] = (u16)pos[i];
		posv[i] = pos[i];
	}
eob:
	*out_p = out;
	return xp;
}

/* One round of lanes */
struct ADD_SUFFIX(mlsd_round) {
	size_t pos[MLSD_LANES];		/* current positions */
	size_t pos_base[MLSD_LANES];	/* start positions */
	unsigned nsteps;		/* steps per lane in this round */
	u64 (*recs)[MLSD_NREC];		/* records (in the decompressor) */
	u16 (*poss)[MLSD_NREC + 1];	/* positions (in the decompressor) */
};

/* A verified range of records of one lane */
struct ADD_SUFFIX(mlsd_seg) {
	u16 lane, lo, hi;
};

/*
 * Stitch the lanes of a round together: for each pair of adjacent lanes,
 * find where lane i+1 takes over from lane i, extending lane i where needed.
 * Fills seg[] with the verified record ranges, in order, and returns their
 * number; '*next_pos' is set to the position after the last verified record.
 */
static forceinline unsigned
ADD_SUFFIX(mlsd_stitch)(struct ADD_SUFFIX(mlsd_round) * restrict rd,
			const u8 * restrict in, const u64 * restrict lt,
			const u64 * restrict ot, const u32 lmask,
			struct ADD_SUFFIX(mlsd_seg) * restrict seg,
			size_t * restrict next_pos, int * restrict margin_adj)
{
	int adj = 0;
	unsigned ans[MLSD_LANES];
	u32 tgt[MLSD_LANES];
	unsigned s, start = 0, nseg = 0;
	int i;

	/*
	 * Look for lane i's end position among lane i+1's positions, with
	 * branchless binary searches that run interleaved.  ans[i] becomes
	 * the index of the first position of lane i+1 that is >= lane i's end.
	 */
	for (i = 0; i + 1 < MLSD_LANES; i++) {
		const ptrdiff_t t = (ptrdiff_t)(rd->pos[i] - rd->pos_base[i + 1]);

		/* tgt == 0 (lane i stopped short) never matches */
		tgt[i] = t <= 0 ? 0 : t > 0xFFFF ? 0xFFFF : (u32)t;
		ans[i] = 0;
	}
	for (s = rd->nsteps / 2; s >= 1; s >>= 1) {
		MLSD_UNROLL_LOOP
		for (i = 0; i + 1 < MLSD_LANES; i++) {
			const unsigned a = ans[i] + s;

			ans[i] = mlsd_rel(rd->poss[i + 1], a,
					  (u16)rd->pos_base[i + 1]) < tgt[i] ?
				 a : ans[i];
		}
	}

	for (i = 0; ; i++) {
		unsigned cnt = rd->nsteps;
		int next;

		seg[nseg].lane = i;
		seg[nseg].lo = start;
		seg[nseg].hi = cnt;
		nseg++;
		if (i + 1 == MLSD_LANES)
			break;
		if (tgt[i] != 0 &&
		    mlsd_rel(rd->poss[i + 1], ans[i] + 1,
			     (u16)rd->pos_base[i + 1]) == tgt[i]) {
			start = ans[i] + 1;
			adj -= MLSD_MARGIN_STEP;
			continue;
		}
		adj += MLSD_CU_RATIO * MLSD_MARGIN_STEP;
		/*
		 * Lane i+1 hasn't synchronized by the end of lane i.  Extend
		 * lane i until it lands on a boundary of lane i+1, or give up
		 * and end the round with lane i.
		 */
		next = ADD_SUFFIX(mlsd_catchup)(in, &rd->pos[i], lt, ot, lmask,
						rd->recs[i], rd->poss[i], &cnt,
						rd->poss[i + 1],
						rd->pos_base[i + 1], ans[i] + 1,
						rd->nsteps);
		seg[nseg - 1].hi = cnt;
		if (next < 0) {
			break;
		}
		start = next;
	}
	*next_pos = rd->pos[i];
	*margin_adj = adj;
	return nseg;
}

/*
 * Start a round of lanes of 'nsteps' steps at position 'p0', spaced 'dist'
 * bits apart.
 */
static forceinline void
ADD_SUFFIX(mlsd_setup)(struct ADD_SUFFIX(mlsd_round) * restrict rd,
		       size_t p0, size_t dist, unsigned nsteps)
{
	int i;

	for (i = 0; i < MLSD_LANES; i++)
		rd->pos[i] = rd->pos_base[i] = p0 + (size_t)i * dist;
	rd->nsteps = nsteps;
}

/*
 * Decode as much of the current Huffman block as possible with multiple lanes.
 * On return, ctx holds a verified state (a symbol boundary), possibly the
 * position just past the end-of-block symbol (FASTLOOP_BLOCK_DONE).
 * 'state' carries the lanes' progress rate (bits per step, times 16) and the
 * margin across blocks.  'block_recs0' is about how many records of this block
 * were decoded before (which sets the size of the rounds).  If 'ev' isn't
 * NULL, this is a measured comparison: a window of the rounds that execute
 * records while decoding the next round (the steady state) starts with the
 * first such round after MLSD_EVAL_WARM_ROUNDS of them and after 'skip_bytes'
 * bytes; its second part, which is timed, starts at the first round boundary
 * at least 'stop_bytes' / 2 bytes into it (the first part is there for the
 * fastloop to warm up in, see the caller), and it ends at a round boundary at
 * least 'stop_bytes' / 2 bytes later, or at the end of the block.  Then stop,
 * and set ev[0] and ev[1] to the clock units and output bytes of the second
 * part
 * (or to 0 if there wasn't one), ev[2] to the offset of the window's first
 * output byte from where this started, ev[3] to the bit position in 'in'
 * where its first record starts, ev[5] to the size of its first part, and
 * ev[4] to the clock units from the start
 * to the first round that executed records (scaled to MLSD_MIN_STEPS steps
 * per round: about what this costs at the start of a block, beyond the steady
 * state).
 * Also stops early (returning FASTLOOP_BLOCK_NOT_DONE) if the block fails the
 * data check.
 */
static NOINLINE ATTRIBUTES enum fastloop_result
ADD_SUFFIX(decompress_multilane)(struct libdeflate_decompressor * restrict d,
				 struct fastloop_ctx * restrict ctx,
				 const u8 * const in, const u8 * const in_end,
				 u8 * const out_end, const u32 lmask,
				 size_t * restrict state, size_t block_recs0,
				 size_t skip_bytes, size_t stop_bytes,
				 u64 * restrict ev)
{
	const u64 * const lt = d->mlsd_litlen_table;
	const u64 * const ot = d->mlsd_offset_table;
	const unsigned copy = d->mlsd_copy;
	const u8 * const out_start = ctx->out;
	struct ADD_SUFFIX(mlsd_round) rounds[2];
	struct ADD_SUFFIX(mlsd_round) *cur = &rounds[0], *nxt = &rounds[1];
	u64 * const xr = d->mlsd_xr;
	struct ADD_SUFFIX(mlsd_seg) seg[MLSD_LANES];
	u8 *out_next = ctx->out_next;
	size_t pos0 = (size_t)(ctx->in_next - in) * 8 - (u8)ctx->bitsleft;
	size_t rate16 = state[0];
	size_t margin = state[1];
	size_t block_recs = block_recs0;
	unsigned nsteps = MIN(block_recs < MLSD_GROW1 ? MLSD_MIN_STEPS :
			      block_recs < MLSD_GROW2 ? 2 * MLSD_MIN_STEPS :
			      4 * MLSD_MIN_STEPS, MLSD_MAX_STEPS);
	size_t dist = MAX(rate16 * nsteps / 16, margin + 64) - margin;
	const size_t in_nbits = (size_t)(in_end - in) * 8;
	enum fastloop_result res = FASTLOOP_BLOCK_NOT_DONE;
	bool have_lanes = false;	/* 'cur' holds a round of lanes */
	size_t end_pos;
	u8 * const out_entry = out_next;
	bool stop = false;		/* stop after this round */
	bool judged = !MLSD_ADAPTIVE;	/* data check done? */
	size_t g_alt = 0, g_samp = 0;	/* literal/match alternations, samples */
	unsigned g_prev = 2;		/* kind of the last record (2 = none) */
	const u64 tm_entry = ev != NULL ? mlsd_now() : 0;
	u8 *om0 = NULL, *om1 = NULL;	/* output at the start and end of the */
	u64 tm1 = 0;			/* ... measured window, clock at its end, */
	u64 tmh = 0;			/* ... and where its second part */
	u8 *omh = NULL;			/* ... (the timed one) starts, */
	size_t wpos = 0;		/* ... and its starting bit position */
	unsigned wrounds = 0;		/* rounds in the measured window */
	unsigned xrounds = 0;		/* rounds before it */
	const unsigned nsteps0 = nsteps;
	u64 tm_r1 = 0;			/* clock at the first such round */

	rounds[0].recs = d->mlsd_recs[0];
	rounds[0].poss = d->mlsd_poss[0];
	rounds[1].recs = d->mlsd_recs[1];
	rounds[1].poss = d->mlsd_poss[1];

/*
 * Is there enough input and output space to start a round of 'n' steps at
 * position 'p'?
 */
#define MLSD_CAN_START(p, n)						\
	((p) + (MLSD_LANES - 1) * dist +				\
	 ((n) + MLSD_CATCHUP) * MLSD_MAX_STEP_BITS + 2 * 64 <= in_nbits &&\
	 (size_t)(out_end - out_next) >= MLSD_LANE_OUT_BYTES(n))

	for (;;) {
		size_t next_pos, nrec, prog;
		unsigned nseg, k, next_nsteps;
		int adj;
		int i;

		if (!have_lanes) {
			if (!MLSD_CAN_START(pos0, nsteps)) {
				if (nsteps == MLSD_MIN_STEPS)
					break;
				nsteps = MLSD_MIN_STEPS;
				dist = MAX(rate16 * nsteps / 16, margin + 64) -
				       margin;
				continue;
			}
			ADD_SUFFIX(mlsd_setup)(cur, pos0, dist, nsteps);
			ADD_SUFFIX(mlsd_lanes)(in, cur->pos, lt, ot, lmask,
					       cur->recs, cur->poss, nsteps);
		}
		nseg = ADD_SUFFIX(mlsd_stitch)(cur, in, lt, ot, lmask, seg,
					       &next_pos, &adj);

		nrec = 0;
		for (k = 0; k < nseg; k++)
			nrec += seg[k].hi - seg[k].lo;
		block_recs += nrec;
		if (!judged) {
			/* data check: count literal/match alternations */
			for (k = 0; k < nseg; k++) {
				const u64 *r = &cur->recs[seg[k].lane][seg[k].lo];
				const u64 *r_end = r + (seg[k].hi - seg[k].lo);

				for (; r < r_end; r++) {
					const unsigned kind =
						(*r & ((u64)0x1FF << 18)) != 0;

					g_alt += kind != g_prev;
					g_prev = kind;
				}
			}
			g_samp += nrec;
			if (g_samp >= MLSD_GATE_MIN_RECS) {
				judged = true;
				if (1000 * (g_alt - 1) < MLSD_GATE_MIN_ALT * g_samp) {
					d->mlsd_skip = MIN(2 * d->mlsd_skip + 1,
							   MLSD_GATE_MAX_SKIP);
					d->mlsd_skip_left = d->mlsd_skip;
					stop = true;
				} else {
					d->mlsd_skip = 0;
				}
			}
		}
		if (omh != NULL &&
		    (size_t)(out_next - omh) >= stop_bytes / 2 &&
		    wrounds >= MLSD_EVAL_MIN_ROUNDS)
			stop = true;

		/*
		 * Adapt the lane spacing to the lanes' progress, and choose the
		 * size of the next round.
		 */
		prog = 0;
		for (i = 0; i < MLSD_LANES; i++)
			prog += mlsd_rel(cur->poss[i], cur->nsteps,
					 (u16)cur->pos_base[i]);
		rate16 = prog * 16 / (MLSD_LANES * cur->nsteps);
		margin = (size_t)MIN(MAX((ptrdiff_t)margin + adj,
					 MLSD_MARGIN_MIN), MLSD_MARGIN_MAX);
		next_nsteps = block_recs < MLSD_GROW1 ? MLSD_MIN_STEPS :
			      block_recs < MLSD_GROW2 ? 2 * MLSD_MIN_STEPS :
			      4 * MLSD_MIN_STEPS;
		if (next_nsteps > MLSD_MAX_STEPS)
			next_nsteps = MLSD_MAX_STEPS;
		nsteps = next_nsteps;
		dist = MAX(rate16 * nsteps / 16, margin + 64) - margin;
		if (nsteps != MLSD_MIN_STEPS && !MLSD_CAN_START(next_pos, nsteps)) {
			nsteps = MLSD_MIN_STEPS;
			dist = MAX(rate16 * nsteps / 16, margin + 64) - margin;
		}

		if (!stop &&
		    out_next - out_start >= DEFLATE_MAX_MATCH_OFFSET &&
		    (size_t)(out_end - out_next) >=
		    nrec * MLSD_MAX_OUT_PER_REC + MLSD_OUT_SLOP &&
		    MLSD_CAN_START(next_pos, nsteps)) {
			/*
			 * Execute this round's records while running the next
			 * round's lanes.
			 */
			u64 *xw = xr;
			const u64 *xstop;

			for (k = 0; k < nseg; k++) {
				const unsigned n = seg[k].hi - seg[k].lo;

				memcpy(xw, &cur->recs[seg[k].lane][seg[k].lo],
				       n * sizeof(u64));
				xw += n;
			}
			ADD_SUFFIX(mlsd_setup)(nxt, next_pos, dist, nsteps);
			if (ev != NULL && om0 == NULL) {
				if (tm_r1 == 0)
					tm_r1 = mlsd_now();
				if (xrounds++ >= MLSD_EVAL_WARM_ROUNDS &&
				    (size_t)(out_next - out_entry) >=
				    skip_bytes) {
					/* this round executes cur's records,
					   which start at lane 0's start */
					om0 = out_next;
					wpos = cur->pos_base[0];
				}
			}
			xstop = ADD_SUFFIX(mlsd_lanes_exec)(
					in, nxt->pos, lt, ot, lmask, nxt->recs,
					nxt->poss, xr, xw,
					(nrec + nsteps - 1) / nsteps,
					&out_next, copy, nsteps);
			if (xstop != xw) {
				/* End of block: find the position past it. */
				size_t idx = xstop - xr;

				for (k = 0; idx >= (size_t)(seg[k].hi -
							    seg[k].lo); k++)
					idx -= seg[k].hi - seg[k].lo;
				i = seg[k].lane;
				end_pos = cur->pos_base[i] +
					  mlsd_rel(cur->poss[i],
						   seg[k].lo + idx + 1,
						   (u16)cur->pos_base[i]);
				res = FASTLOOP_BLOCK_DONE;
				goto out;
			}
			if (om0 != NULL) {
				tm1 = mlsd_now();
				om1 = out_next;
				if (omh == NULL &&
				    (size_t)(out_next - om0) >= stop_bytes / 2) {
					tmh = tm1;
					omh = out_next;
				} else if (omh != NULL) {
					wrounds++;
				}
			}
			{
				struct ADD_SUFFIX(mlsd_round) *tmp = cur;

				cur = nxt;
				nxt = tmp;
			}
			have_lanes = true;
			continue;
		}

		/* Execute this round's records on their own. */
		for (k = 0; k < nseg; k++) {
			const unsigned lo = seg[k].lo;
			const size_t n = seg[k].hi - lo;
			size_t r;

			i = seg[k].lane;
			if ((size_t)(out_end - out_next) <
			    n * MLSD_MAX_OUT_PER_REC + MLSD_OUT_SLOP) {
				/* Not enough output space; stop at 'lo' */
				pos0 = cur->pos_base[i] +
				       mlsd_rel(cur->poss[i], lo,
						(u16)cur->pos_base[i]);
				goto done;
			}
			if (out_next - out_start >= DEFLATE_MAX_MATCH_OFFSET)
				r = ADD_SUFFIX(mlsd_exec_unchecked)(
					&cur->recs[i][lo], n, &out_next,
					out_start, copy);
			else
				r = ADD_SUFFIX(mlsd_exec_checked)(
					&cur->recs[i][lo], n, &out_next,
					out_start, copy);
			if (r == (size_t)-1)
				return FASTLOOP_BAD_DATA;
			if (r < n) {
				/* end of block: the position just past it */
				end_pos = cur->pos_base[i] +
					  mlsd_rel(cur->poss[i], lo + r + 1,
						   (u16)cur->pos_base[i]);
				res = FASTLOOP_BLOCK_DONE;
				goto out;
			}
		}
		pos0 = next_pos;
		have_lanes = false;
		if (stop)
			break;
	}
#undef MLSD_CAN_START
done:
	end_pos = pos0;
out:
	state[0] = rate16;
	state[1] = margin;
	if (ev != NULL) {
		const bool w = omh != NULL && om1 > omh;

		ev[0] = w ? tm1 - tmh : 0;
		ev[1] = w ? (u64)(om1 - omh) : 0;
		ev[2] = w ? (u64)(om0 - out_entry) : 0;
		ev[3] = wpos;
		ev[5] = w ? (u64)(omh - om0) : 0;
		/* the time to the first such round, for nsteps0 = min */
		ev[4] = om1 != NULL ?
			(tm_r1 - tm_entry) * MLSD_MIN_STEPS / nsteps0 : 0;
	}
	{
		const u8 *ip = in + (end_pos >> 3);

		ctx->bitbuf = get_unaligned_leword(ip) >> (end_pos & 7);
		ctx->in_next = ip + sizeof(bitbuf_t) - 1;
		ctx->bitsleft = (MAX_BITSLEFT & ~7) - (end_pos & 7);
	}
	ctx->out_next = out_next;
	return res;
}
