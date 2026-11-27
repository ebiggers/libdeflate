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
 * record that also holds the step's bit position.
 *
 * Lane i+1 takes over where lane i ended if it has a step at that bit
 * position, as both decode the same symbols from there; otherwise the lanes
 * get more steps until they meet.  The verified records are chained by "jump"
 * records and executed while the lanes decode the next round (the lanes are
 * ALU-bound, the execution store-bound).
 *
 * The lanes' tables have 64-bit entries holding the record bits ready to be
 * added up.  They and the records live at the end of the output buffer, where
 * the lanes' output stops (struct mlsd_scratch).  Litlen entries, including
 * literal+literal and literal+length entries as in the normal table:
 *
 *	Bit 58-32:  bits 26-0 of the record
 *	Bit 30-26:  extra length bits mask
 *	Bit 21-16:  codeword length
 *	Bit 8:      1 if no offset follows (MLSD_NOOFF)
 *	Bit 5-0:    codeword length + extra bits
 *
 * Offset entries:
 *
 *	Bit 63-58:  codeword length + extra bits
 *	Bit 47-32:  offset base
 *	Bit 30-18:  extra offset bits mask
 *	Bit 5-0:    codeword length
 *
 * Offset table entries 256-511 consume no bits and give MLSD_DUMMY_OFF: literal
 * and EOB steps OR MLSD_NOOFF into the offset table index instead of selecting.
 * Subtable entries hold the full codeword length; subtable pointers have the
 * subtable's offset in the table in bits 62-32, bit 31 set (MLSD_SUB) and the
 * index mask in bits 6-0.  Records:
 *
 *	Bit 63-48:  bit position of the step, from the round's first byte
 *	Bit 47-32:  match offset (MLSD_DUMMY_OFF if none)
 *	Bit 27:     0 (MLSD_JUMP)
 *	Bit 26-18:  match length (0 if none, MLSD_EOB_LEN if EOB)
 *	Bit 17-16:  number of literals (0-2)
 *	Bit 15-0:   the literal(s)
 *
 * A jump record has MLSD_JUMP set, length MLSD_EOB_LEN and no literals; its
 * offset is the signed distance in bytes to the next record, or 0 if there is
 * none.
 */

#ifndef MLSD_MIN_STEPS	/* (can be overridden for testing) */
#  define MLSD_MIN_STEPS	128	/* steps per lane per round at first */
#  define MLSD_GROW		4096	/* ... until this many records */
#endif
#define MLSD_EOB_LEN		511
#define MLSD_DUMMY_OFF		4096
#define MLSD_MAX_STEP_BITS	(LENGTH_MAXBITS + OFFSET_MAXBITS)
#define MLSD_MAX_REC_OUT	(2 + DEFLATE_MAX_MATCH_LEN)
/* The most records executed per step: a round's, over the next one's steps */
#define MLSD_MAX_EXEC		(MLSD_LANES + 1 + \
				 (MLSD_LANES * MLSD_CATCHUP + MLSD_LANES) / \
				 MLSD_MIN_STEPS)
/* The last lane starts at least this many bits before the end of the input */
#define MLSD_IN_MARGIN		(MLSD_NREC * MLSD_MAX_STEP_BITS + 64)
/* The output stops this far before the scratch */
#define MLSD_OUT_MARGIN		((MLSD_MAX_EXEC + 1) * MLSD_MAX_REC_OUT + \
				 FASTLOOP_MAX_MATCH_OVERRUN)
/*
 * A block uses the lanes only if:
 * - the output buffer has MLSD_MIN_BUF_BYTES (with less, the lanes stop too
 *   soon to pay off on highly compressible data),
 * - MLSD_MIN_IN_BYTES of input and MLSD_MIN_OUT_BYTES of output space before
 *   the scratch remain,
 * - the previous block produced MLSD_MIN_PREV_OUT bytes, and
 * - the length codewords take at least 7/32 of the codespace (the fastloop
 *   does better on runs of literals).
 */
#ifndef MLSD_MIN_PREV_OUT
#  define MLSD_MIN_BUF_BYTES	524288
#  define MLSD_MIN_IN_BYTES	4096
#  define MLSD_MIN_OUT_BYTES	32768
#  define MLSD_MIN_PREV_OUT	4096
#  define MLSD_MIN_LEN_SPACE	(7U << (DEFLATE_MAX_CODEWORD_LEN - 5))
#endif

#ifndef MLSD_SHARED_DEFINED
#define MLSD_SHARED_DEFINED
#define MLSD_SUB		((u64)1 << 31)
#define MLSD_NOOFF		((u64)1 << 8)
#define MLSD_JUMP		((u64)1 << 27)
#define MLSD_POS(rec)		((u16)((rec) >> 48))
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

/*
 * The gate on the litlen codeword lengths: whether the length codewords take
 * at least MLSD_MIN_LEN_SPACE of the codespace
 */
static forceinline bool
mlsd_gate(const u8 *lens, unsigned num_syms)
{
	u32 space = 0;
	unsigned sym;

	NO_VECTORIZE_LOOP
	for (sym = DEFLATE_END_OF_BLOCK + 1; sym < num_syms; sym++)
		space += ((1U << DEFLATE_MAX_CODEWORD_LEN) >> lens[sym]) &
			 ((1U << DEFLATE_MAX_CODEWORD_LEN) - 1);
	return space >= MLSD_MIN_LEN_SPACE;
}

/*
 * The lane-format entry for symbol 'sym' with codeword length 'len';
 * 'results' is litlen_decode_results or offset_decode_results
 */
static forceinline u64
mlsd_entry(unsigned sym, unsigned len, bool litlen, const u32 *results)
{
	u32 res, base, mask;

	if (litlen && sym < DEFLATE_NUM_LITERALS)
		return MLSD_NOOFF | ((u64)sym << 32) | ((u64)1 << 48) |
		       (len << 16) | len;
	if (litlen && sym == DEFLATE_END_OF_BLOCK)
		return MLSD_NOOFF | ((u64)MLSD_EOB_LEN << 50) |
		       (len << 16) | len;
	res = results[sym];
	base = res >> 16;
	mask = (1U << (res & 0x1F)) - 1;
	if (litlen)
		return ((u64)base << 50) | ((u64)mask << 26) | (len << 16) |
		       (len + (res & 0x1F));
	return ((u64)(len + (res & 0x1F)) << 58) | ((u64)base << 32) |
	       ((u64)mask << 18) | len;
}
#endif /* MLSD_SHARED_DEFINED */

/*
 * Build the lanes' litlen or offset table from the 'num_syms' codeword lengths
 * at 'lens', which get 16 bytes of zero padding.  Like build_decode_table(),
 * fill the main table one codeword length at a time, doubling it in between;
 * add the compound entries of total length 'len' (litlen only) once it has
 * 2^len entries.  Return false unless the code is complete.
 */
static NOINLINE ATTRIBUTES bool
ADD_SUFFIX(mlsd_build_table)(struct mlsd_scratch * restrict s,
			     u8 * restrict lens, unsigned num_syms, bool litlen)
{
	u64 * const t = litlen ? s->lt : s->ot;
	const unsigned tb = litlen ? MLSD_TABLEBITS : OFFSET_TABLEBITS;
	/* (the subtables start after the main or dummy entries) */
	unsigned sub_end = litlen ? 1U << MLSD_TABLEBITS :
				    2U << OFFSET_TABLEBITS;
	const u32 * const results = litlen ? litlen_decode_results :
					     offset_decode_results;
	u64 * const ent = s->u.tmp.ent;	/* entries, in codeword order */
	u16 * const rc = s->u.tmp.rc;	/* codewords, bit-reversed */
	/* end[len]: the number of codewords of at most this length */
	u16 * const end = s->u.tmp.end, * const lit_end = s->u.tmp.lit_end;
	unsigned sym, len, n, i = 0, code = 0, sub = 0, prefix = ~0U;

	vst1q_u8(&lens[num_syms], vdupq_n_u8(0));
	if (!litlen) {
		for (i = 1U << OFFSET_TABLEBITS; i < 2U << OFFSET_TABLEBITS; i++)
			t[i] = (u64)MLSD_DUMMY_OFF << 32;
		i = 0;
	}
	end[0] = 0;
	for (len = 1; len <= DEFLATE_MAX_CODEWORD_LEN; len++) {
		unsigned le = i;	/* past the literals of this length */

		code <<= 1;
		if (len <= tb) {
			/* (may copy too much) */
			const u64 *src = t;
			u64 *dst = &t[1U << (len - 1)];

			do {
				const uint64x2_t a = vld1q_u64(src),
						 b = vld1q_u64(src + 2),
						 c = vld1q_u64(src + 4),
						 d = vld1q_u64(src + 6);

				vst1q_u64(dst, a);
				vst1q_u64(dst + 2, b);
				vst1q_u64(dst + 4, c);
				vst1q_u64(dst + 6, d);
				src += 8;
				dst += 8;
			} while (src < &t[1U << (len - 1)]);
		}
		NO_UNROLL_LOOP
		for (sym = 0; sym < num_syms; sym += 16) {
			/* a bit per symbol with this codeword length */
			u64 m = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(
					vreinterpretq_u16_u8(vceqq_u8(
						vld1q_u8(&lens[sym]),
						vdupq_n_u8(len))), 4)), 0) &
				0x1111111111111111;

			for (; m != 0; m &= m - 1, i++, code++) {
				const unsigned v = sym + (bsf64(m) >> 2);

				rc[i] = rbit32(code) >> (32 - len);
				ent[i] = mlsd_entry(v, len, litlen, results);
				if (len <= tb)
					t[rc[i]] = ent[i];
				if (v < DEFLATE_NUM_LITERALS)
					le = i + 1;
			}
		}
		end[len] = i;
		lit_end[len] = le;
		/* 1st codeword: a literal of length n */
		for (n = 1; litlen && len <= tb && n < len; n++) {
			unsigned a, b;

			if (lit_end[n] == end[n - 1])
				continue;
			for (b = end[len - n - 1]; b < end[len - n]; b++) {
				u64 * const dst = &t[(unsigned)rc[b] << n];
				const u64 lit2 = ent[b] & ((u64)0xFF << 32);
				const u64 e = ent[b] + (lit2 << 8) - lit2 -
					      MLSD_NOOFF;

				for (a = end[n - 1]; a < lit_end[n]; a++)
					dst[rc[a]] = e + ent[a];
			}
		}
	}
	/* (an overfull code overshoots too) */
	if (code != 1U << DEFLATE_MAX_CODEWORD_LEN)
		return false;
	/* Longer codewords, in subtables (as in build_decode_table()) */
	for (i = end[tb], len = tb + 1; len <= DEFLATE_MAX_CODEWORD_LEN; len++) {
		NO_UNROLL_LOOP
		for (; i < end[len]; i++) {
			if ((rc[i] & ((1U << tb) - 1)) != prefix) {
				unsigned bits = len - tb;
				u32 used = end[len] - i;

				while (used < 1U << bits) {
					bits++;
					used = (used << 1) + end[tb + bits] -
					       end[tb + bits - 1];
				}
				prefix = rc[i] & ((1U << tb) - 1);
				sub = sub_end;
				sub_end += 1U << bits;
				t[prefix] = ((u64)sub * sizeof(t[0]) << 32) |
					    MLSD_SUB | ((1U << bits) - 1);
			}
			NO_UNROLL_LOOP
			for (n = sub + (rc[i] >> tb); n < sub_end;
			     n += 1U << (len - tb))
				t[n] = ent[i];
		}
	}
	return true;
}

/* Resolve a subtable pointer to its entry, which works like a main-table one */
#define MLSD_SUB_ENTRY(table, entry, bits, tb)				\
	(((const u64 *)((const u8 *)(table) + ((entry) >> 32)))		\
		[(u32)(entry) & ((u32)(bits) >> (tb))])

/*
 * Decode the litlen entry (and any offset) at bit position 'p' into the record
 * *rp, branch-free except for the rare subtable lookups; advance 'p'.
 */
#define MLSD_STEP(in, p, rp)						\
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
	x1_ = (w_ >> ((e1_ >> 16) & 63)) & ((u32)e1_ >> 26);		\
	x2_ = (w1_ >> (e2_ & 63)) & ((u32)e2_ >> 18);			\
	*(rp) = ((e1_ + (x1_ << 50)) >> 32) |				\
		((e2_ + (x2_ << 32)) & ~(u64)0xFFFFFFFF);		\
	put_unaligned_le16((u16)(p), (u8 *)(rp) + 6);			\
	(p) += (u8)e1_ + (e2_ >> 58);					\
} while (0)

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
 * Execute the next 'r' records at 'xp', unless 'out' is past 'out_limit'.  A
 * match of at most 64 bytes that doesn't overlap itself takes one
 * MLSD_COPY64().
 */
#define MLSD_EXEC(r)							\
do {									\
	size_t k = (r);							\
									\
	if (unlikely(out > out_limit))					\
		goto stopped;						\
	do {								\
		const u64 x = *xp;					\
		const u32 len = (x >> 18) & 0x1FF;			\
		const u32 off = (x >> 32) & 0xFFFF;			\
		const u8 *src;						\
									\
		put_unaligned_le16((u16)x, out);			\
		out += (x >> 16) & 3;					\
		src = out - off;					\
		if (likely(len <= 64 && len <= off)) {			\
			const uint8x16_t a = vld1q_u8(src),		\
					 b = vld1q_u8(src + 16),	\
					 c = vld1q_u8(src + 32),	\
					 d = vld1q_u8(src + 48);	\
									\
			vst1q_u8(out, a);				\
			vst1q_u8(out + 16, b);				\
			vst1q_u8(out + 32, c);				\
			vst1q_u8(out + 48, d);				\
		} else if (len == MLSD_EOB_LEN) {			\
			if (!(x & MLSD_JUMP)) {				\
				res = FASTLOOP_BLOCK_DONE;		\
				xp += MLSD_LANES;			\
				goto stopped;				\
			}						\
			xp = (const u64 *)((const u8 *)xp + (s16)off);	\
			if (off != 0)					\
				continue;				\
			xp = NULL;					\
			break;						\
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
		xp += MLSD_LANES;					\
	} while (--k != 0);						\
} while (0)

/*
 * Build the lanes' tables from the block's codeword lengths in s->lens and
 * decode as much of the block as the lanes can, leaving ctx at a symbol
 * boundary (past the end-of-block symbol if the result is
 * FASTLOOP_BLOCK_DONE).  At least DEFLATE_MAX_MATCH_OFFSET bytes must have been
 * written, and the output stops short of the scratch.
 */
static NOINLINE ATTRIBUTES enum fastloop_result
ADD_SUFFIX(decompress_multilane)(struct fastloop_ctx * restrict ctx,
				 struct mlsd_state * restrict state,
				 const u8 * const in, const u8 * const in_end,
				 unsigned num_litlen_syms,
				 unsigned num_offset_syms)
{
	struct mlsd_scratch * const s = (struct mlsd_scratch *)state->scratch;
	const u64 * const lt = s->lt, * const ot = s->ot;
	const u8 * const out_limit = (const u8 *)s - MLSD_OUT_MARGIN;
	const size_t in_limit = (size_t)(in_end - in) * 8 - MLSD_IN_MARGIN;
	u8 *out = ctx->out_next;
	/* The next record to execute, and its round's first byte, in bits */
	const u64 *xp = NULL;
	size_t xbase = 0;
	size_t end_pos = (size_t)(ctx->in_next - in) * 8 - (u8)ctx->bitsleft;
	size_t rate16 = state->rate16 + 12 * 16, margin = state->margin + 256;
	size_t nrec = 0, total = 0;
	unsigned cur = 0;
	enum fastloop_result res = FASTLOOP_BLOCK_NOT_DONE;
	int i;

	/* (s->lt[] has LITLEN_ENOUGH entries) */
	STATIC_ASSERT(MLSD_TABLEBITS == LITLEN_TABLEBITS);
	/* (the offset table first: the litlen padding overwrites its lens) */
	if (!mlsd_gate(s->lens, num_litlen_syms) ||
	    !ADD_SUFFIX(mlsd_build_table)(s, s->lens + num_litlen_syms,
					  num_offset_syms, false) ||
	    !ADD_SUFFIX(mlsd_build_table)(s, s->lens, num_litlen_syms, true))
		return res;
	/* (a round's positions fit in 16 bits) */
	STATIC_ASSERT(8 + ((MLSD_LANES - 1) * MLSD_ROUND_STEPS + MLSD_NREC) *
		      MLSD_MAX_STEP_BITS <= 0x10000);
	STATIC_ASSERT(MLSD_MIN_IN_BYTES * 8 >= MLSD_IN_MARGIN);
	STATIC_ASSERT(MLSD_CATCHUP % MLSD_EXTEND == 0);
	STATIC_ASSERT(MLSD_LANES == 4);
	for (;;) {
		const unsigned nsteps = nrec < MLSD_GROW ? MLSD_MIN_STEPS :
					MLSD_ROUND_STEPS;
		/* (the jump records are executed too) */
		const size_t r = (total + MLSD_LANES) / nsteps + 1;
		u64 (* const recs)[MLSD_LANES] = s->u.recs[cur];
		const size_t dist = MAX(rate16 * nsteps / 16, margin + 64) -
				    margin;
		const size_t base = end_pos & ~(size_t)7, start = end_pos & 7;
		size_t p[MLSD_LANES], q;
		const u8 *rin;
		unsigned n, cnt;

		/*
		 * Start a round of lanes, unless the end of the input is near:
		 * then the last round's records are left unexecuted.
		 */
		if (end_pos + (MLSD_LANES - 1) * dist > in_limit)
			break;
		rin = in + base / 8;
		for (i = 0; i < MLSD_LANES; i++)
			p[i] = start + (size_t)i * dist;
		/* ... while executing the verified records of the last one. */
		for (n = 0, cnt = nsteps; ; cnt += MLSD_EXTEND) {
			u64 *rp = recs[n];
			unsigned lo = 0;

			/* (one copy of the steps serves the lanes, in pairs) */
			for (; n < cnt; n++) {
				i = MLSD_LANES / 2;
				NO_UNROLL_LOOP
				do {
					size_t pa = p[0], pb = p[1];

					MLSD_STEP(rin, pa, &rp[0]);
					MLSD_STEP(rin, pb, &rp[1]);
					rp += 2;
					p[0] = p[2];
					p[1] = p[3];
					p[2] = pa;
					p[3] = pb;
				} while (--i != 0);
				if (xp != NULL)
					MLSD_EXEC(r);
			}
			ASSERT(xp == NULL);
			MLSD_UNROLL_LOOP
			for (i = 0; i < MLSD_LANES; i++)
				recs[cnt][i] = ((u64)p[i] << 48) | MLSD_JUMP |
					       ((u64)MLSD_EOB_LEN << 18);
			/*
			 * Chain the verified records: lane i+1 takes over where
			 * it has a step at lane i's end.  If a lane has none,
			 * all the lanes get MLSD_EXTEND more steps (or that
			 * lane ends the round).  The margin settles where 1/7
			 * of the lanes need that.
			 */
			total = 0;
			NO_UNROLL_LOOP
			for (i = 0; ; i++) {
				u64 (* const lane)[MLSD_LANES] =
					(void *)&recs[0][i];
				const u64 (* const next)[MLSD_LANES] =
					(void *)&recs[0][i + 1];
				unsigned h = 0;

				q = MLSD_POS(lane[cnt][0]);
				total += cnt - lo;
				if (i + 1 == MLSD_LANES)
					break;
				while (h < cnt && MLSD_POS(next[h][0]) < q)
					h++;
				if (MLSD_POS(next[h][0]) != q) {
					if (cnt == nsteps)
						margin += 20;
					break;
				}
				if (cnt == nsteps && margin > 32)
					margin -= 4;
				lane[cnt][0] |= (u64)(u16)((const u8 *)next[h] -
						(const u8 *)lane[cnt]) << 32;
				lo = h;
			}
			if (i + 1 == MLSD_LANES ||
			    cnt == nsteps + MLSD_CATCHUP)
				break;
		}
		xp = recs[0];
		xbase = base;
		end_pos = base + q;
		rate16 = (q - start) * 16 / total;
		margin = MIN(margin, 1024);
		nrec += total;
		cur ^= 1;
	}
stopped:
	/* (at end-of-block, out of space or near the end of the input) */
	if (xp != NULL)
		end_pos = xbase + MLSD_POS(*xp);
	state->rate16 = rate16 - 12 * 16;
	state->margin = margin - 256;
	ctx->bitbuf = get_unaligned_leword(in + (end_pos >> 3)) >>
		      (end_pos & 7);
	ctx->in_next = in + (end_pos >> 3) + sizeof(bitbuf_t) - 1;
	ctx->bitsleft = (MAX_BITSLEFT & ~7) - (end_pos & 7);
	ctx->out_next = out;
	return res;
}
#undef MLSD_EXEC
#undef MLSD_COPY64
#undef MLSD_STEP
