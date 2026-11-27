/*
 * decompress_multilane.h - multi-lane speculative decoding of Huffman blocks
 *
 * The fastloop is limited by its chain of dependent table lookups and by
 * mispredicted branches on the symbol type.  But decoding from an arbitrary
 * bit position soon (after about 100 bits on real data) lands on a symbol
 * boundary and then yields the true symbols.  So MLSD_LANES "lanes" decode
 * parts of a block at once, branch-free: lane 0 from the known position, the
 * others spaced by the distance a lane covers in a round, less a margin.  Each
 * step decodes a litlen entry (and the offset after a length) into a 64-bit
 * record and saves its bit position.
 *
 * Lane i+1 takes over where lane i ended if it has a step at that bit position
 * (found by binary search), as both decode the same symbols from there;
 * otherwise lane i is extended until they meet.  The verified records are
 * executed while the lanes decode the next round (the lanes are ALU-bound, the
 * execution store-bound).
 *
 * The lanes' tables have 64-bit entries holding the record bits ready to be
 * added up.  They and the records live at the end of the output buffer, where
 * the lanes' output stops (struct mlsd_scratch).  Litlen entries, including
 * literal+literal and literal+length entries as in the normal table:
 *
 *	Bit 58-32:  bits 26-0 of the record
 *	Bit 31-27:  extra length bits mask
 *	Bit 21-16:  codeword length
 *	Bit 8:      1 if no offset follows (MLSD_NOOFF)
 *	Bit 5-0:    codeword length + extra bits
 *
 * Offset entries:
 *
 *	Bit 63-58:  codeword length + extra bits
 *	Bit 47-32:  offset base
 *	Bit 31-19:  extra offset bits mask
 *	Bit 5-0:    codeword length
 *
 * Offset table entries 256-511 consume no bits and give MLSD_DUMMY_OFF: literal
 * and EOB steps OR MLSD_NOOFF into the offset table index instead of selecting.
 * Subtable entries hold the full codeword length; subtable pointers have the
 * subtable start index in bits 58-32, the index mask in bits 31-24 and bit 14
 * set (MLSD_SUB).  Records (other bits are undefined):
 *
 *	Bit 47-32:  match offset (MLSD_DUMMY_OFF if none)
 *	Bit 26-18:  match length (0 if none, MLSD_EOB_LEN if EOB)
 *	Bit 17-16:  number of literals (0-2)
 *	Bit 15-0:   the literal(s)
 */

#ifndef MLSD_MIN_STEPS	/* (can be overridden for testing) */
#  define MLSD_MIN_STEPS	128	/* steps per lane per round at first */
#  define MLSD_GROW		4096	/* ... until this many records */
#endif
#define MLSD_EOB_LEN		511
#define MLSD_DUMMY_OFF		4096
#define MLSD_MAX_STEP_BITS	(LENGTH_MAXBITS + OFFSET_MAXBITS)
#define MLSD_MAX_REC_OUT	(2 + DEFLATE_MAX_MATCH_LEN)
/*
 * A block uses the lanes only if:
 * - the output buffer has MLSD_MIN_BUF_BYTES (with less, the lanes stop too
 *   soon to pay off on highly compressible data),
 * - MLSD_MIN_IN_BYTES of input and MLSD_MIN_OUT_BYTES of output space before
 *   the scratch remain,
 * - the previous block produced MLSD_MIN_PREV_OUT bytes, and
 * - the length codewords take at least 1/8 of the codespace (the fastloop
 *   does better on runs of literals), or 1/4 if the literal codewords average
 *   more than MLSD_MAX_LIT_BITS bits (then few literal pairs fit in the main
 *   table; such blocks were up to 20% slower on an M1).
 */
#ifndef MLSD_MIN_PREV_OUT
#  define MLSD_MIN_BUF_BYTES	524288
#  define MLSD_MIN_IN_BYTES	4096
#  define MLSD_MIN_OUT_BYTES	32768
#  define MLSD_MIN_PREV_OUT	4096
#  define MLSD_MIN_LEN_SPACE	(1U << (DEFLATE_MAX_CODEWORD_LEN - 3))
#endif
#define MLSD_MAX_LIT_BITS	6

#ifndef MLSD_SHARED_DEFINED
#define MLSD_SHARED_DEFINED
#define MLSD_SUB		((u64)1 << 14)
#define MLSD_NOOFF		((u64)1 << 8)
#if defined(__clang__)
#  define MLSD_UNROLL_LOOP	_Pragma("clang loop unroll(full)")
#else
#  define MLSD_UNROLL_LOOP	_Pragma("GCC unroll 8")
#endif

#include <arm_neon.h>
#include <sys/qos.h>
#include <sys/resource.h>
#ifndef PRIO_DARWIN_THREAD	/* (hidden by a strict _POSIX_C_SOURCE) */
#  define PRIO_DARWIN_THREAD	3
#endif
#ifndef PRIO_DARWIN_PROCESS
#  define PRIO_DARWIN_PROCESS	4
#endif

/*
 * Whether this thread may run on the efficiency cores, where the lanes lose:
 * a QoS class of utility or lower, or a "darwinbg" thread or process
 */
static bool
mlsd_low_priority(void)
{
	const qos_class_t q = qos_class_self();

#ifdef MLSD_TEST_ANY_PRIORITY
	return false;
#endif
	return (q != QOS_CLASS_UNSPECIFIED && q <= QOS_CLASS_UTILITY) ||
	       getpriority(PRIO_DARWIN_PROCESS, 0) > 0 ||
	       getpriority(PRIO_DARWIN_THREAD, 0) > 0;
}

/* The lane-format entry for symbol 'sym' with codeword length 'len' */
static forceinline u64
mlsd_entry(unsigned sym, unsigned len, bool litlen)
{
	u32 res, base, mask;

	if (litlen && sym < DEFLATE_NUM_LITERALS)
		return MLSD_NOOFF | ((u64)sym << 32) | ((u64)1 << 48) |
		       (len << 16) | len;
	if (litlen && sym == DEFLATE_END_OF_BLOCK)
		return MLSD_NOOFF | ((u64)MLSD_EOB_LEN << 50) |
		       (len << 16) | len;
	res = litlen ? litlen_decode_results[sym] : offset_decode_results[sym];
	base = res >> 16;
	mask = (1U << (res & 0x1F)) - 1;
	if (litlen)
		return ((u64)base << 50) | ((u64)mask << 27) | (len << 16) |
		       (len + (res & 0x1F));
	return ((u64)(len + (res & 0x1F)) << 58) | ((u64)base << 32) |
	       ((u64)mask << 19) | len;
}
#endif /* MLSD_SHARED_DEFINED */

/*
 * Build a lane-format table with 2^tb main entries and subtables from index
 * 'sub_base'.  Like build_decode_table(), fill the main table one codeword
 * length at a time, doubling it in between; add the compound entries of total
 * length 'len' (litlen only) once it has 2^len entries.  Return false unless
 * the code is complete and, for litlen, passes the gate (MLSD_MIN_LEN_SPACE).
 */
static ATTRIBUTES bool
ADD_SUFFIX(mlsd_build_table)(u64 * restrict t, const u8 * restrict lens,
			     unsigned num_syms, unsigned tb, unsigned sub_base,
			     bool litlen, u16 * restrict sorted)
{
	unsigned count[DEFLATE_MAX_CODEWORD_LEN + 1];
	unsigned start[DEFLATE_MAX_CODEWORD_LEN + 2];
	unsigned pos[DEFLATE_MAX_CODEWORD_LEN + 1];
	u16 rc[DEFLATE_MAX_NUM_SYMS];	/* codewords (bit-reversed), sorted */
	u64 ent[DEFLATE_MAX_NUM_SYMS];	/* entries, sorted */
	unsigned sym, len, i, n, cw = 0, end = sub_base, sub = 0, prefix = ~0U;
	u32 space = 0;

	if (litlen) {
		/*
		 * The gate, before any other work: the length codewords' share
		 * of the codespace if the code is complete, from the literals'
		 * codeword lengths
		 */
		const uint16x8_t top =
			vdupq_n_u16(1U << DEFLATE_MAX_CODEWORD_LEN);
		uint32x4_t vspace = vdupq_n_u32(0), vbits = vdupq_n_u32(0);
		u32 lit_space, lit_bits;

		for (sym = 0; sym < DEFLATE_NUM_LITERALS; sym += 8) {
			const uint16x8_t l = vmovl_u8(vld1_u8(&lens[sym]));
			const int16x8_t sh = vnegq_s16(vreinterpretq_s16_u16(l));
			/* 2^(15 - len) each, or 0 for an unused symbol */
			const uint16x8_t sp = vandq_u16(vshlq_u16(top, sh),
							vtstq_u16(l, l));

			vspace = vpadalq_u16(vspace, sp);
			vbits = vpadalq_u16(vbits, vmulq_u16(sp, l));
		}
		lit_space = vaddvq_u32(vspace);
		lit_bits = vaddvq_u32(vbits);
		space = (1U << DEFLATE_MAX_CODEWORD_LEN) - lit_space;
		if (lens[DEFLATE_END_OF_BLOCK] != 0)
			space -= (1U << DEFLATE_MAX_CODEWORD_LEN) >>
				 lens[DEFLATE_END_OF_BLOCK];
		if (space < MLSD_MIN_LEN_SPACE ||
		    (space < 2 * MLSD_MIN_LEN_SPACE &&
		     lit_bits > MLSD_MAX_LIT_BITS * lit_space))
			return false;
	}
	/* Count the codewords of each length, 16 symbols at a time */
	count[0] = 0;
	for (len = 1; len <= DEFLATE_MAX_CODEWORD_LEN; len++) {
		uint8x16_t c = vdupq_n_u8(0);

		NO_UNROLL_LOOP
		for (sym = 0; sym + 16 <= num_syms; sym += 16)
			c = vsubq_u8(c, vceqq_u8(vld1q_u8(&lens[sym]),
						 vdupq_n_u8(len)));
		count[len] = vaddlvq_u8(c);
	}
	NO_UNROLL_LOOP
	for (sym = num_syms & ~15U; sym < num_syms; sym++)
		count[lens[sym]]++;
	start[1] = 0;
	space = 0;
	NO_UNROLL_LOOP
	for (len = 1; len <= DEFLATE_MAX_CODEWORD_LEN; len++) {
		space += count[len] << (DEFLATE_MAX_CODEWORD_LEN - len);
		pos[len] = start[len];
		start[len + 1] = start[len] + count[len];
	}
	if (space != 1U << DEFLATE_MAX_CODEWORD_LEN)
		return false;
	for (sym = 0; sym < num_syms; sym++) {
		len = lens[sym];
		if (len != 0) {
			ent[pos[len]] = mlsd_entry(sym, len, litlen);
			sorted[pos[len]++] = sym;
		}
	}
	for (i = 0, len = 1; len <= tb; len++) {
		memcpy(&t[1U << (len - 1)], t, sizeof(t[0]) << (len - 1));
		for (n = count[len]; n != 0; n--, i++) {
			rc[i] = cw;
			t[cw] = ent[i];
			if (cw != (1U << len) - 1) {	/* next codeword */
				sym = 1U << bsr32(cw ^ ((1U << len) - 1));
				cw = (cw & (sym - 1)) | sym;
			}
		}
		for (n = 1; litlen && n < len; n++) {
			unsigned a, b;

			/* 1st codeword: a literal of length n */
			if (start[n] == start[n + 1] ||
			    sorted[start[n]] >= DEFLATE_NUM_LITERALS)
				continue;
			for (b = start[len - n]; b < start[len - n + 1]; b++) {
				u64 * const dst = &t[(unsigned)rc[b] << n];
				const u64 lit2 = ent[b] & ((u64)0xFF << 32);
				const u64 e = ent[b] + (lit2 << 8) - lit2 +
					      ((u64)1 << 48) + n * 0x10001;

				if (sorted[b] == DEFLATE_END_OF_BLOCK)
					continue;
				for (a = start[n]; a < start[n + 1] &&
				     sorted[a] < DEFLATE_NUM_LITERALS; a++)
					dst[rc[a]] = e + ((u64)sorted[a] << 32);
			}
		}
	}
	/* Longer codewords, in subtables (as in build_decode_table()) */
	for (; i < start[DEFLATE_MAX_CODEWORD_LEN + 1]; i++) {
		len = lens[sorted[i]];
		if ((cw & ((1U << tb) - 1)) != prefix) {
			unsigned bits = len - tb;
			u32 used = start[len + 1] - i;

			while (used < 1U << bits) {
				bits++;
				used = (used << 1) + count[tb + bits];
			}
			prefix = cw & ((1U << tb) - 1);
			sub = end;
			end += 1U << bits;
			t[prefix] = ((u64)sub << 32) |
				    (((1U << bits) - 1) << 24) | MLSD_SUB;
		}
		for (n = sub + (cw >> tb); n < end; n += 1U << (len - tb))
			t[n] = ent[i];
		if (cw != (1U << len) - 1) {
			n = 1U << bsr32(cw ^ ((1U << len) - 1));
			cw = (cw & (n - 1)) | n;
		}
	}
	return true;
}

/* Resolve a subtable pointer to its entry, which works like a main-table one */
#define MLSD_SUB_ENTRY(table, entry, bits, tb)				\
	((table)[(u32)((entry) >> 32) +					\
		 (((bits) >> (tb)) & ((u32)(entry) >> 24))])

/*
 * Decode the litlen entry (and any offset) at bit position 'p' into the record
 * *rp, branch-free except for the rare subtable lookups; save 'p' in *pp and
 * advance it.
 */
#define MLSD_STEP(in, p, rp, pp)					\
do {									\
	const u64 w_ = get_unaligned_le64((in) + ((p) >> 3)) >> ((p) & 7);\
	u64 e1_ = lt[w_ & ((1U << MLSD_TABLEBITS) - 1)];		\
	u64 w1_, e2_, x1_, x2_;						\
									\
	if (unlikely(e1_ & MLSD_SUB))					\
		e1_ = MLSD_SUB_ENTRY(lt, e1_, w_, MLSD_TABLEBITS);	\
	w1_ = w_ >> (e1_ & 63);						\
	e2_ = ot[(w1_ & 0xFF) | (e1_ & MLSD_NOOFF)];			\
	if (unlikely(e2_ & MLSD_SUB))					\
		e2_ = MLSD_SUB_ENTRY(ot, e2_, w1_, OFFSET_TABLEBITS);	\
	x1_ = (w_ >> ((e1_ >> 16) & 63)) & ((u32)e1_ >> 27);		\
	x2_ = (w1_ >> (e2_ & 63)) & ((u32)e2_ >> 19);			\
	*(rp) = ((e1_ + (x1_ << 50)) >> 32) |				\
		((e2_ + (x2_ << 32)) & ~(u64)0xFFFFFFFF);		\
	*(pp) = (u16)(p);						\
	(p) += (u8)e1_ + (e2_ >> 58);					\
} while (0)

/* A round's work for mlsd_run() */
struct ADD_SUFFIX(mlsd_job) {
	size_t pos[MLSD_LANES];		/* the lanes' bit positions */
	struct {
		const u64 *p, *end;
	} seg[MLSD_LANES + 1];		/* verified records; p == NULL ends them */
	size_t r;			/* records to execute per step */
	u8 *out;			/* where their output goes */
	const u8 *out_limit;
};

/*
 * Copy 64 bytes, 16 at a time: correct for offsets of 16 or more, and for the
 * first 'offset' bytes of shorter ones
 */
#define MLSD_COPY64(dst, src)						\
do {									\
	vst1q_u8((dst), vld1q_u8(src));					\
	vst1q_u8((dst) + 16, vld1q_u8((src) + 16));			\
	vst1q_u8((dst) + 32, vld1q_u8((src) + 32));			\
	vst1q_u8((dst) + 48, vld1q_u8((src) + 48));			\
} while (0)

/*
 * In mlsd_run(): execute up to 'r' records of the current range, unless 'out'
 * is past 'out_limit'.  A match of at most 64 bytes that doesn't overlap
 * itself takes one MLSD_COPY64().
 */
#define MLSD_EXEC_SOME()						\
do {									\
	const u64 *xm;							\
									\
	if (unlikely(out > out_limit))					\
		goto out;						\
	for (xm = xp + MIN(r, (size_t)(xe - xp)); xp != xm; xp++) {	\
		const u64 x = *xp;					\
		const u32 len = (x >> 18) & 0x1FF;			\
		const u32 off = (x >> 32) & 0xFFFF;			\
		const u8 *src;						\
									\
		put_unaligned_le16((u16)x, out);			\
		out += (x >> 16) & 3;					\
		src = out - off;					\
		if (likely(len <= 64 && len <= off)) {			\
			MLSD_COPY64(out, src);				\
		} else if (len == MLSD_EOB_LEN) {			\
			goto out;					\
		} else if (off >= 16) {					\
			u8 *dst = out;					\
									\
			do {						\
				MLSD_COPY64(dst, src);			\
				dst += 64;				\
				src += 64;				\
			} while (dst < out + len);			\
		} else {						\
			COPY_MATCH(out, src, off, out + len);		\
		}							\
		out += len;						\
	}								\
	if (xp == xe) {							\
		xp = job->seg[++k].p;					\
		xe = job->seg[k].end;					\
	}								\
} while (0)

/*
 * Run the lanes from job->pos[] for 'nsteps' steps, filling round 'round' of
 * 's', while executing the records of job->seg[] at job->out.  Stop at an
 * end-of-block record, or at the next record once job->out is past
 * job->out_limit (which must leave room for job->r more records), and return a
 * pointer to it; else return NULL.  job->r must be large enough for the steps
 * to execute all the records, and at least DEFLATE_MAX_MATCH_OFFSET bytes must
 * have been written.
 */
static NOINLINE ATTRIBUTES const u64 *
ADD_SUFFIX(mlsd_run)(const u8 * restrict in, struct mlsd_scratch * restrict s,
		     unsigned round, unsigned nsteps,
		     struct ADD_SUFFIX(mlsd_job) * restrict job)
{
	const u64 * const lt = s->lt, * const ot = s->ot;
	u64 (* const recs)[MLSD_NREC] = s->recs[round];
	u16 (* const poss)[MLSD_NREC + 1] = s->poss[round];
	const size_t r = job->r;
	const u8 * const out_limit = job->out_limit;
	size_t p[MLSD_LANES];
	u8 *out = job->out;
	const u64 *xp = job->seg[0].p, *xe = job->seg[0].end;
	unsigned n, k = 0;
	int i;

	for (i = 0; i < MLSD_LANES; i++)
		p[i] = job->pos[i];
	for (n = 0; n < nsteps; n++) {
		MLSD_UNROLL_LOOP
		for (i = 0; i < MLSD_LANES; i++)
			MLSD_STEP(in, p[i], &recs[i][n], &poss[i][n]);
		if (xp != NULL)
			MLSD_EXEC_SOME();
	}
	ASSERT(xp == NULL);
	for (i = 0; i < MLSD_LANES; i++) {
		poss[i][nsteps] = (u16)p[i];
		job->pos[i] = p[i];
	}
out:
	job->out = out;
	return xp;
}
#undef MLSD_EXEC_SOME
#undef MLSD_COPY64

/*
 * Build the lanes' tables from the block's codeword lengths and decode as much
 * of the block as the lanes can, leaving ctx at a symbol boundary (past the
 * end-of-block symbol if the result is FASTLOOP_BLOCK_DONE).  ctx is unchanged
 * if the block shouldn't or can't use the lanes.  At least
 * DEFLATE_MAX_MATCH_OFFSET bytes must have been written, and the output stops
 * short of 's'.  state[] carries the lanes' bits per step (times 16) and the
 * margin across blocks, as differences from their initial values.
 */
static NOINLINE ATTRIBUTES enum fastloop_result
ADD_SUFFIX(decompress_multilane)(struct libdeflate_decompressor * restrict d,
				 struct fastloop_ctx * restrict ctx,
				 struct mlsd_scratch * restrict s,
				 const u8 * const in, const u8 * const in_end,
				 size_t * restrict state, const u8 *lens,
				 unsigned num_litlen_syms,
				 unsigned num_offset_syms)
{
	const u64 * const lt = s->lt, * const ot = s->ot;
	const u8 * const out_end = (const u8 *)s;
	/*
	 * A round's bit positions are relative to its base 'rin': 'xin' for
	 * the round whose records are being executed.
	 */
	const u8 *rin, *xin = in;
	struct ADD_SUFFIX(mlsd_job) job;
	size_t end_pos = (size_t)(ctx->in_next - in) * 8 - (u8)ctx->bitsleft;
	size_t rate16 = state[0] + 12 * 16, margin = state[1] + 256, nrec = 0;
	unsigned cur = 0;
	enum fastloop_result res = FASTLOOP_BLOCK_NOT_DONE;
	int i;

	/* (s->lt[] has LITLEN_ENOUGH entries) */
	STATIC_ASSERT(MLSD_TABLEBITS == LITLEN_TABLEBITS);
	if (!ADD_SUFFIX(mlsd_build_table)(s->lt, lens, num_litlen_syms,
					  MLSD_TABLEBITS, 1U << MLSD_TABLEBITS,
					  true, d->sorted_syms))
		return res;
	for (i = 1U << OFFSET_TABLEBITS; i < 2U << OFFSET_TABLEBITS; i++)
		s->ot[i] = (u64)MLSD_DUMMY_OFF << 32;
	if (!ADD_SUFFIX(mlsd_build_table)(s->ot, lens + num_litlen_syms,
					  num_offset_syms, OFFSET_TABLEBITS,
					  2U << OFFSET_TABLEBITS, false,
					  d->sorted_syms))
		return res;
	job.seg[0].p = NULL;
	job.r = 0;
	job.out = ctx->out_next;
	/* (a round's positions fit in 16 bits) */
	STATIC_ASSERT(8 + ((MLSD_LANES - 1) * MLSD_ROUND_STEPS + MLSD_NREC) *
		      MLSD_MAX_STEP_BITS <= 0x10000);
	for (;;) {
		const size_t space = (job.r + 1) * MLSD_MAX_REC_OUT +
				     FASTLOOP_MAX_MATCH_OVERRUN;
		unsigned nsteps = nrec < MLSD_GROW ? MLSD_MIN_STEPS :
				  MLSD_ROUND_STEPS;
		size_t dist = MAX(rate16 * nsteps / 16, margin + 64) - margin;
		size_t start = end_pos & 7;
		u16 (*pp)[MLSD_NREC + 1];
		unsigned ans[MLSD_LANES], nseg = 0, half, lo = 0;
		size_t prog = 0, r = 0;
		const u64 *stop;

		/*
		 * Start a round of lanes.  Near the end of the input, the last
		 * round's lanes redo the start of the input instead, unused.
		 */
		STATIC_ASSERT(MLSD_MIN_IN_BYTES * 8 >=
			      MLSD_ROUND_STEPS * MLSD_MAX_STEP_BITS + 64);
		rin = in + (end_pos >> 3);
		if (end_pos + (MLSD_LANES - 1) * dist +
		    (nsteps + MLSD_CATCHUP) * MLSD_MAX_STEP_BITS + 64 >
		    (size_t)(in_end - in) * 8) {
			if (job.seg[0].p == NULL)
				return res;	/* (ctx is unchanged) */
			rin = in;
			start = dist = 0;
		}
		for (i = 0; i < MLSD_LANES; i++)
			job.pos[i] = start + (size_t)i * dist;
		/* ... while executing the verified records of the last one. */
		job.out_limit = out_end - job.out > (ptrdiff_t)space ?
				out_end - space : job.out - 1;
		stop = ADD_SUFFIX(mlsd_run)(rin, s, cur ^ 1, nsteps, &job);
		if (stop != NULL) {
			/* At end-of-block, or out of space */
			const size_t idx = stop - &s->recs[cur][0][0];
			const bool eob = (*stop >> 18 & 0x1FF) == MLSD_EOB_LEN;

			/* (each lane has one more position than records) */
			end_pos = (size_t)(xin - in) * 8 +
				  (&s->poss[cur][0][0])[idx + idx / MLSD_NREC +
							eob];
			if (eob)
				res = FASTLOOP_BLOCK_DONE;
			break;
		}
		if (dist == 0)
			break;
		cur ^= 1;
		xin = rin;
		pp = s->poss[cur];
		/*
		 * Look for each lane's end among the next lane's positions,
		 * with interleaved branchless binary searches.
		 */
		for (i = 0; i < MLSD_LANES; i++) {
			prog += job.pos[i] - start - (size_t)i * dist;
			ans[i] = 0;
		}
		for (half = nsteps / 2; half >= 1; half >>= 1) {
			MLSD_UNROLL_LOOP
			for (i = 0; i + 1 < MLSD_LANES; i++) {
				const unsigned a = ans[i] + half;

				ans[i] = pp[i + 1][a] < job.pos[i] ? a : ans[i];
			}
		}
		/*
		 * Collect the verified records: lane i+1 takes over where it
		 * meets lane i, else lane i is extended until it does (or ends
		 * the round).  The margin settles where 1/6 of the boundaries
		 * need such a catch-up.
		 */
		for (i = 0; ; i++) {
			unsigned cnt = nsteps;
			int next = -1;

			if (i + 1 < MLSD_LANES) {
				size_t p = job.pos[i];
				unsigned h = ans[i] + 1;

				if (pp[i + 1][h] == p) {
					next = h;
					if (margin > 32)
						margin -= 4;
				} else {
					margin += 20;
				}
				while (next < 0 &&
				       cnt < nsteps + MLSD_CATCHUP) {
					MLSD_STEP(xin, p, &s->recs[cur][i][cnt],
						  &pp[i][cnt]);
					pp[i][++cnt] = (u16)p;
					while (h <= nsteps && pp[i + 1][h] < p)
						h++;
					if (h > nsteps)
						break;
					if (pp[i + 1][h] == p)
						next = h;
				}
				job.pos[i] = p;
			}
			if (cnt > lo) {
				job.seg[nseg].p = &s->recs[cur][i][lo];
				job.seg[nseg].end = &s->recs[cur][i][cnt];
				r += cnt - lo;
				nseg++;
			}
			if (next < 0)
				break;
			lo = next;
		}
		job.seg[nseg].p = NULL;
		end_pos = (size_t)(xin - in) * 8 + job.pos[i];
		rate16 = prog * 16 / (MLSD_LANES * nsteps);
		margin = MIN(margin, 1024);
		nrec += r;
		nsteps = nrec < MLSD_GROW ? MLSD_MIN_STEPS : MLSD_ROUND_STEPS;
		/* (each of up to MLSD_LANES ranges can end a step early) */
		job.r = r / (nsteps - MLSD_LANES) + 1;
	}
	state[0] = rate16 - 12 * 16;
	state[1] = margin - 256;
	ctx->bitbuf = get_unaligned_leword(in + (end_pos >> 3)) >>
		      (end_pos & 7);
	ctx->in_next = in + (end_pos >> 3) + sizeof(bitbuf_t) - 1;
	ctx->bitsleft = (MAX_BITSLEFT & ~7) - (end_pos & 7);
	ctx->out_next = job.out;
	return res;
}
