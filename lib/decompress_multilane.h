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
 * records and executed: with MLSD_INTERLEAVE, while the lanes decode the next
 * round (the lanes wait on their table lookups, the execution on its stores).
 *
 * The lanes' tables have 64-bit entries holding the record bits ready to be
 * added up.  They and the records live at the end of the output buffer, where
 * the lanes' output stops (struct mlsd_scratch).  Litlen entries, including
 * literal+literal and literal+length entries as in the normal table:
 *
 *	Bit 63-32:  bits 63-32 of the record
 *	Bit 14-9:   bits consumed
 *	Bit 8:      1 if no offset follows (MLSD_NOOFF)
 *	Bit 5-0:    bits consumed, or 62 or 63 (modulo 64) with MLSD_NOOFF
 *
 * A length's extra bits are part of the table index, and the entry holds the
 * length itself, if the codeword(s) and the extra bits fit in the index.
 * Otherwise the entry has MLSD_XLEN, the length's base, and where the literals
 * go the codeword length (bits 39-32) and the extra bits mask (bits 47-40): the
 * extra bits are added when the record is executed.  The end-of-block entry has
 * MLSD_SLOW (the lanes shouldn't go on past it), and so do the subtable
 * pointers, which have minus the subtable's offset in the table in bits 63-32
 * (so MLSD_PTR too) and the index mask in bits 31-0.  Offset entries:
 *
 *	Bit 47-32:  extra offset bits mask
 *	Bit 31-16:  offset base
 *	Bit 14-9:   codeword length + extra bits
 *	Bit 5-0:    codeword length
 *
 * A step without an offset ORs MLSD_NOOFF into the offset table index instead
 * of selecting, and shifts the input bits out of it (bits 5-0 above): offset
 * table entries 256 to 259 consume no bits and give MLSD_DUMMY_OFF.  Records:
 *
 *	Bit 63:     1 if end-of-block
 *	Bit 60:     1 in a jump record (MLSD_JUMP)
 *	Bit 59:     MLSD_XLEN
 *	Bit 58-50:  match length (0 if none), or its base with MLSD_XLEN
 *	Bit 49-48:  number of literals (0-2)
 *	Bit 47-32:  the literal(s)
 *	Bit 31-16:  match offset (MLSD_DUMMY_OFF if none)
 *	Bit 15-0:   bit position of the step in the input, modulo 2^16
 *
 * A jump record has the position where its lane ended; its offset is the
 * signed distance in records to the next one, or 0 if there is none.
 */

#ifndef MLSD_MIN_STEPS	/* (can be overridden for testing) */
#  define MLSD_MIN_STEPS	128	/* steps per lane per round at first */
#  define MLSD_GROW		4096	/* ... until this many records */
#endif
#define MLSD_LONG_LEN_SYM	273	/* the first symbol of a length over 34 */
#define MLSD_DUMMY_OFF		4096
#define MLSD_MAX_STEP_BITS	(LENGTH_MAXBITS + OFFSET_MAXBITS)
#define MLSD_MAX_REC_OUT	(2 + DEFLATE_MAX_MATCH_LEN)
/* The last lane starts at least this many bits before the end of the input */
#define MLSD_IN_MARGIN		(MLSD_NREC * MLSD_MAX_STEP_BITS + 64)
/* The output stops this far before the scratch */
#define MLSD_OUT_MARGIN		(MLSD_MAX_REC_OUT + 64)
/*
 * A block uses the lanes only if:
 * - the output buffer has MLSD_MIN_BUF_BYTES (with less, the lanes stop too
 *   soon to pay off on highly compressible data),
 * - MLSD_MIN_IN_BYTES of input and MLSD_MIN_OUT_BYTES of output space before
 *   the scratch remain,
 * - the previous block produced MLSD_MIN_PREV_OUT bytes, and
 * - the length codewords take at least MLSD_GATE32/32 of the codespace (the
 *   fastloop does better on runs of literals), weighted by MLSD_GATE_LEN.
 */
#ifndef MLSD_MIN_PREV_OUT
#  define MLSD_MIN_BUF_BYTES	524288
#  define MLSD_MIN_IN_BYTES	4096
#  define MLSD_MIN_OUT_BYTES	32768
#  define MLSD_MIN_PREV_OUT	4096
#  define MLSD_MIN_LEN_SPACE	(MLSD_GATE32 << (DEFLATE_MAX_CODEWORD_LEN - 5))
#endif

#ifndef MLSD_SHARED_DEFINED
#define MLSD_SHARED_DEFINED
#define MLSD_NOOFF		((u64)1 << 8)
#define MLSD_XLEN		((u64)1 << 59)
#define MLSD_JUMP		((u64)1 << 60)
#define MLSD_PTR		((u64)1 << 62)
#define MLSD_SLOW		((u64)1 << 63)
#define MLSD_POS(rec)		((u16)(rec))
/* The bits of a litlen entry that only an MLSD_XLEN entry has */
#define MLSD_XLEN_BITS		(MLSD_XLEN | ((u64)0xFFFF << 32))
#if defined(__clang__)
#  define MLSD_UNROLL_LOOP	_Pragma("clang loop unroll(full)")
#else
#  define MLSD_UNROLL_LOOP	_Pragma("GCC unroll 16")
#endif

#ifdef __APPLE__
#include <sys/qos.h>
#include <sys/resource.h>
#ifndef PRIO_DARWIN_THREAD	/* (hidden by a strict _POSIX_C_SOURCE) */
#  define PRIO_DARWIN_THREAD	3
#endif
#ifndef PRIO_DARWIN_PROCESS
#  define PRIO_DARWIN_PROCESS	4
#endif
#endif

/*
 * Whether the lanes help where this thread runs.  On x86, that depends on the
 * CPU.  On Apple arm64, they lose on the efficiency cores, where a thread may
 * run if it has a QoS class of utility or lower or if it or its process is
 * "darwinbg".
 */
static bool
mlsd_usable(void)
{
#ifdef MLSD_TEST_ALWAYS_USABLE
	return true;
#elif defined(ARCH_X86_64)
	return (get_x86_cpu_features() & X86_CPU_FEATURE_MLSD) != 0;
#else
	const qos_class_t q = qos_class_self();

	return (q == QOS_CLASS_UNSPECIFIED || q > QOS_CLASS_UTILITY) &&
	       getpriority(PRIO_DARWIN_PROCESS, 0) <= 0 &&
	       getpriority(PRIO_DARWIN_THREAD, 0) <= 0;
#endif
}

/* Reverse the low 16 bits of 'v' */
static forceinline u32
mlsd_rev16(u32 v)
{
#ifdef rbit32
	return rbit32(v) >> 16;
#else
	v = ((v & 0x5555) << 1) | ((v >> 1) & 0x5555);
	v = ((v & 0x3333) << 2) | ((v >> 2) & 0x3333);
	v = ((v & 0x0F0F) << 4) | ((v >> 4) & 0x0F0F);
	return ((v & 0xFF) << 8) | (v >> 8);
#endif
}

/* 16 bytes, for unaligned loads and stores */
typedef u8 mlsd_vec __attribute__((vector_size(16), aligned(1), may_alias));
#define MLSD_LOAD(p)		(*(const mlsd_vec *)(p))
#define MLSD_STORE(p, v)	(*(mlsd_vec *)(p) = (v))

/* Copy 32 or 64 bytes that may overlap, loading them all first */
static forceinline void
mlsd_move(u8 *dst, const u8 *src, bool all64)
{
	const mlsd_vec a = MLSD_LOAD(src), b = MLSD_LOAD(src + 16);

	if (all64) {
		const mlsd_vec c = MLSD_LOAD(src + 32), d = MLSD_LOAD(src + 48);

		MLSD_STORE(dst + 32, c);
		MLSD_STORE(dst + 48, d);
	}
	MLSD_STORE(dst, a);
	MLSD_STORE(dst + 16, b);
}

/*
 * The lane-format entry for symbol 'sym' with codeword length 'len';
 * 'results' is litlen_decode_results or offset_decode_results.  A length with
 * extra bits gets MLSD_XLEN and its base.
 */
static forceinline u64
mlsd_entry(unsigned sym, unsigned len, bool litlen, const u32 *results)
{
	u32 res, base, mask, total;

	if (litlen && sym < DEFLATE_NUM_LITERALS)
		return MLSD_NOOFF | ((u64)1 << 48) | ((u64)sym << 32) |
		       (len << 9) | 63;
	if (litlen && sym == DEFLATE_END_OF_BLOCK)
		return MLSD_SLOW | MLSD_NOOFF | (len << 9) | 63;
	res = results[sym];
	base = res >> 16;
	mask = (1U << (res & 0x1F)) - 1;
	total = len + (res & 0x1F);
	if (!litlen)
		return ((u64)mask << 32) | (base << 16) | (total << 9) | len;
	return ((u64)base << 50) | (total << 9) | total |
	       (mask != 0 ? MLSD_XLEN | ((u64)mask << 40) | ((u64)len << 32) :
			    0);
}
/* The entry that a subtable pointer leads to */
#define MLSD_SUB_ENTRY(table, entry, bits, tb)				\
	(((const u64 *)((const u8 *)(table) - ((s64)(entry) >> 32)))	\
		[(u32)(entry) & ((u32)(bits) >> (tb))])

#endif /* MLSD_SHARED_DEFINED */

/*
 * Build the lanes' litlen or offset table from the 'num_syms' codeword lengths
 * at 'lens', which get 8 bytes of zero padding.  Like build_decode_table(),
 * fill the main table one codeword length at a time, doubling it in between;
 * once it has 2^len entries, add the other entries that take 'len' index bits
 * (litlen only): a length with its extra bits, and a literal followed by one
 * of the entries so far.  Return false unless the code is complete.
 */
static NOINLINE ATTRIBUTES bool
ADD_SUFFIX(mlsd_build_table)(struct mlsd_scratch * restrict s,
			     u8 * restrict lens, unsigned num_syms, bool litlen)
{
	struct mlsd_build_tmp * const tmp = &s->u.tmp;
	u64 * const t = litlen ? s->lt : &MLSD_OT(s, 0);
	const unsigned tb = litlen ? MLSD_TABLEBITS : OFFSET_TABLEBITS;
	/* (the subtables start after the main or dummy entries) */
	unsigned sub_end = litlen ? 1U << MLSD_TABLEBITS :
				    (1U << OFFSET_TABLEBITS) + MLSD_NDUMMY;
	const u32 * const results = litlen ? litlen_decode_results :
					     offset_decode_results;
	unsigned sym, len, n, i = 0, code = 0, sub = 0, prefix = ~0U, nxlen = 0;

#if defined(__APPLE__) && defined(ARCH_ARM64) && defined(__clang__)
	/* A frame pointer, so that the function needs no DWARF unwind info */
	__asm__ volatile("" : : "r" (__builtin_frame_address(0)));
#endif
	memset(&lens[num_syms], 0, 8);
	if (!litlen) {
		STATIC_ASSERT(MLSD_NDUMMY == 4);
		t[256] = t[257] = t[258] = t[259] = (u64)MLSD_DUMMY_OFF << 16;
	}
	tmp->end[0] = 0;
	for (len = 1; len <= DEFLATE_MAX_CODEWORD_LEN; len++) {
		unsigned le = i;	/* past the literals of this length */
		unsigned a, b;

		code <<= 1;
		if (len <= tb) {
			/* (may copy too much) */
			const u64 *src = t;
			u64 *dst = &t[1U << (len - 1)];

			do {
				mlsd_move((u8 *)dst, (const u8 *)src, true);
				src += 8;
				dst += 8;
			} while (src < &t[1U << (len - 1)]);
		}
		NO_UNROLL_LOOP
		for (sym = 0; sym < num_syms; sym += 8) {
			/* a bit per symbol with this codeword length */
			u64 m = ~((get_unaligned_le64(&lens[sym]) ^
				   (len * 0x0101010101010101)) +
				  0x7F7F7F7F7F7F7F7F) & 0x8080808080808080;

			for (; m != 0; m &= m - 1, i++, code++) {
				const unsigned v = sym + (bsf64(m) >> 3);

				tmp->rc[i] = mlsd_rev16(code) >> (16 - len);
				tmp->ent[i] = mlsd_entry(v, len, litlen,
							 results);
				if (len <= tb)
					t[tmp->rc[i]] = tmp->ent[i];
				if (v < DEFLATE_NUM_LITERALS)
					le = i + 1;
				if (tmp->ent[i] & MLSD_XLEN)
					tmp->xlen[nxlen++] = i;
			}
		}
		tmp->lit_end[len] = le;
		/* The MLSD_XLEN lengths that take 'len' bits with extra bits */
		for (n = 0; len <= tb && n < nxlen; n++) {
			const u64 e = tmp->ent[tmp->xlen[n]];
			u64 x;

			if ((e & 63) != len)
				continue;
			for (x = 0; x <= ((e >> 40) & 31); x++, i++) {
				tmp->rc[i] = tmp->rc[tmp->xlen[n]] |
					     (x << ((e >> 32) & 63));
				tmp->ent[i] = (e & ~MLSD_XLEN_BITS) + (x << 50);
				t[tmp->rc[i]] = tmp->ent[i];
			}
		}
		tmp->end[len] = i;
		/* A literal of length n, then an entry of 'len - n' bits */
		for (n = 1; litlen && len <= tb && n < len; n++) {
			if (tmp->end[n - 1] == tmp->lit_end[n])
				continue;
			for (b = tmp->end[len - n - 1]; b < tmp->end[len - n];
			     b++) {
				u64 * const dst = &t[(unsigned)tmp->rc[b] << n];
				const u64 eb = tmp->ent[b];
				const u64 lit2 = eb & ((u64)0xFF << 32);
				/* (bits 5-0: 63 + 63 = 62, 63 + n + 1 = n) */
				const u64 e = eb + (lit2 << 8) - lit2 -
					      MLSD_NOOFF +
					      (eb & MLSD_NOOFF ? 0 : n + 1);

				if (eb & (MLSD_SLOW | MLSD_XLEN))
					continue;
				NO_UNROLL_LOOP
				for (a = tmp->end[n - 1]; a < tmp->lit_end[n];
				     a++)
					dst[tmp->rc[a]] = e + tmp->ent[a];
			}
		}
	}
	/* (an overfull code overshoots too) */
	if (code != 1U << DEFLATE_MAX_CODEWORD_LEN)
		return false;
	/* Longer codewords, in subtables (as in build_decode_table()) */
	for (i = tmp->end[tb], len = tb + 1; len <= DEFLATE_MAX_CODEWORD_LEN;
	     len++) {
		NO_UNROLL_LOOP
		for (; i < tmp->end[len]; i++) {
			if ((tmp->rc[i] & ((1U << tb) - 1)) != prefix) {
				unsigned bits = len - tb;
				u32 used = tmp->end[len] - i;

				while (used < 1U << bits) {
					bits++;
					used = (used << 1) +
					       tmp->end[tb + bits] -
					       tmp->end[tb + bits - 1];
				}
				prefix = tmp->rc[i] & ((1U << tb) - 1);
				sub = sub_end;
				sub_end += 1U << bits;
				/* (minus the subtable's offset; the mask) */
				t[prefix] = (0 - (u64)sub * 8) << 32 |
					    ((1U << bits) - 1);
			}
			NO_UNROLL_LOOP
			for (n = sub + (tmp->rc[i] >> tb); n < sub_end;
			     n += 1U << (len - tb))
				t[n] = tmp->ent[i];
		}
	}
	return true;
}

/* p += n, in place: x86_64 clang moves the update otherwise, or copies p */
#if defined(__clang__) && defined(ARCH_X86_64)
#  define MLSD_ADVANCE(p, n)	\
	__asm__ volatile("add {%1, %0|%0, %1}" : "+r" (p) : "r" (n))
#else
#  define MLSD_ADVANCE(p, n)	((p) += (n))
#endif

/*
 * Decode the litlen entry (and any offset) at bit position 'p' into the record
 * *rp, branch-free except for the rare MLSD_SLOW entries; advance 'p'.
 */
#define MLSD_STEP(in, p, rp)						\
do {									\
	const u64 w_ = get_unaligned_le64(MLSD_ADDR(in, p)) >> ((p) & 7);\
	u64 e1_ = s->lt[w_ & ((1U << MLSD_TABLEBITS) - 1)];		\
	u64 w1_, e2_;							\
									\
	if (unlikely(e1_ & MLSD_SLOW)) {				\
		if (e1_ & MLSD_PTR)					\
			e1_ = MLSD_SUB_ENTRY(s->lt, e1_, w_,		\
					     MLSD_TABLEBITS);		\
		eob |= e1_ >> 63;					\
	}								\
	w1_ = w_ >> (e1_ & 63);						\
	e2_ = MLSD_OT(s, (w1_ & 0xFF) | (e1_ & MLSD_NOOFF));		\
	if (unlikely(e2_ & MLSD_SLOW))					\
		e2_ = MLSD_SUB_ENTRY(&MLSD_OT(s, 0), e2_, w1_,		\
				     OFFSET_TABLEBITS);			\
	e1_ += (u32)e2_;						\
	*(rp) = e1_ + (((w1_ >> (e2_ & 63)) & (e2_ >> 32)) << 16);	\
	put_unaligned_le16((u16)(p), (u8 *)(rp));			\
	MLSD_ADVANCE(p, (e1_ >> 9) & 63);				\
} while (0)

/* The bit position of the step of record *xp */
#define MLSD_XPOS()	(xstart + (u16)(MLSD_POS(*xp) - (u16)xstart))

/*
 * Execute the chained records at 'xp' while 'more' holds; xp gets NULL after
 * the last one.  A match of at most 64 bytes that doesn't overlap itself takes
 * one copy of 32 or 64 bytes.
 */
#define MLSD_EXEC(more)							\
do {									\
	/* (the record's fields, loaded as they are needed) */		\
	const u32 hi = get_unaligned_le32((const u8 *)xp + 4);		\
	const u32 off = get_unaligned_le16((const u8 *)xp + 2);		\
	u32 len = hi >> 18;						\
	const u8 *src;							\
									\
	if (unlikely(out > out_limit))					\
		goto stopped;						\
	/* (not literals in an MLSD_XLEN record: the match covers them) */\
	put_unaligned_le16((u16)hi, out);				\
	out += (hi >> 16) & 3;						\
	src = out - off;						\
copy:									\
	if (likely(len <= 64 && len <= off)) {				\
		mlsd_move(out, src, unlikely(len >= copy64_len));	\
	} else if (len >= 512) {					\
		if (hi & (u32)(MLSD_XLEN >> 32)) {			\
			const u64 pos = MLSD_XPOS();			\
									\
			len = (len & 511) +				\
			      ((get_unaligned_le64(MLSD_ADDR(in, pos)) >>\
				((pos & 7) + (u8)hi)) & (u8)(hi >> 8));	\
			goto copy;					\
		}							\
		if (!(hi & (u32)(MLSD_JUMP >> 32))) {			\
			res = FASTLOOP_BLOCK_DONE;			\
			xp += MLSD_LANES;				\
			goto stopped;					\
		}							\
		xp += (s16)off;						\
		xstart += xdist;					\
		if (off != 0)						\
			continue;					\
		xp = NULL;						\
		break;							\
	} else if (off >= 16) {						\
		u8 *dst = out;						\
									\
		/* (16 at a time, as the match may overlap itself) */	\
		do {							\
			MLSD_STORE(dst, MLSD_LOAD(src));		\
			MLSD_STORE(dst + 16, MLSD_LOAD(src + 16));	\
			MLSD_STORE(dst + 32, MLSD_LOAD(src + 32));	\
			MLSD_STORE(dst + 48, MLSD_LOAD(src + 48));	\
			dst += 64;					\
			src += 64;					\
		} while (dst < out + len);				\
	} else {							\
		COPY_MATCH(out, src, off, out + len);			\
	}								\
	out += len;							\
	xp += MLSD_LANES;						\
} while (more)

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
				 const u8 * const in, const u8 * const in_end)
{
	struct mlsd_scratch * const s = (struct mlsd_scratch *)state->scratch;
	const unsigned num_litlen_syms = state->num_litlen_syms;
	const u8 * const out_limit = (const u8 *)s - MLSD_OUT_MARGIN;
	/* (bit positions are 64-bit, from address 0 on x86_64: MLSD_ADDR()) */
	const u64 in_limit = MLSD_BITPOS(in, in_end) - MLSD_IN_MARGIN;
	u8 *out = ctx->out_next;
	/* Where the round starts, or the next one if none is under way */
	u64 end_pos = MLSD_BITPOS(in, ctx->in_next) - (u8)ctx->bitsleft;
	size_t rate16 = state->rate16 + 12 * 16, margin = state->margin + 256;
	size_t nrec = 0, total = 0, dist = 0;
	/* The round's steps go to the rows [recs, rend), of which rp is next */
	u64 *recs = NULL, *rp = NULL, *rend = NULL;
	unsigned nsteps = 0, cnt = 0, cur = 0;
	u64 p[MLSD_LANES];
	/*
	 * The next record to execute if any, where its lane started, and the
	 * distance between the lanes of its round
	 */
	const u64 *xp = NULL;
	u64 xstart = 0;
	size_t xdist = 0;
	/* Records to execute per row of steps (the jump records too), or all */
	MAYBE_UNUSED size_t r = 0;
	/* Whether a lane has decoded an end-of-block symbol in this round */
	MAYBE_UNUSED u64 eob = 0;
	enum fastloop_result res = FASTLOOP_BLOCK_NOT_DONE;
	/*
	 * The gate: the part of the codespace (in 2^-DEFLATE_MAX_CODEWORD_LEN)
	 * that the length codewords take, each counted 1 - (its mean length) /
	 * MLSD_GATE_LEN times if that isn't 0; and the part of the long ones
	 */
	s32 len_space = 0, long_space = 0;
	u32 copy64_len;
	unsigned sym;
	int i;

	NO_VECTORIZE_LOOP
	for (sym = DEFLATE_FIRST_LEN_SYM; sym < num_litlen_syms; sym++) {
		const u32 res_ = litlen_decode_results[sym];
		const s32 space =
			(s32)(((1U << DEFLATE_MAX_CODEWORD_LEN) >>
			       s->lens[sym]) &
			      ((1U << DEFLATE_MAX_CODEWORD_LEN) - 1)) *
			(MLSD_GATE_LEN == 0 ? 1 : 2 * MLSD_GATE_LEN + 1 -
			 2 * (s32)(res_ >> 16) - (1 << (res_ & 0x1F)));

		len_space += space;
		if (sym >= MLSD_LONG_LEN_SYM)
			long_space += space;
	}
	len_space /= MLSD_GATE_LEN != 0 ? 2 * MLSD_GATE_LEN : 1;
	/*
	 * Matches of at least this length are copied 64 bytes at once instead
	 * of 32: all are if many of the block's matches are long, so that the
	 * branch on it is predicted.  (Few such blocks pass a MLSD_GATE_LEN
	 * gate, and x86_64 clang needs the register.)
	 */
	copy64_len = 33;
	if (!MLSD_GATE_LEN) {
		/* (volatile: as a condition it would cost a second branch) */
		volatile u32 v = long_space * 8 >= len_space ? 0 : 33;

		copy64_len = v;
	}

	/* (s->lt[] has LITLEN_ENOUGH entries) */
	STATIC_ASSERT(MLSD_TABLEBITS <= LITLEN_TABLEBITS);
	/* (the offset table first: the litlen padding overwrites its lens) */
	if (len_space < MLSD_MIN_LEN_SPACE ||
	    !ADD_SUFFIX(mlsd_build_table)(s, s->lens + num_litlen_syms,
					  state->num_offset_syms, false) ||
	    !ADD_SUFFIX(mlsd_build_table)(s, s->lens, num_litlen_syms, true))
		return res;
	/* (a lane's positions are told apart modulo 2^16) */
	STATIC_ASSERT(MLSD_NREC * MLSD_MAX_STEP_BITS < 0x10000);
	STATIC_ASSERT(MLSD_MIN_IN_BYTES * 8 >= MLSD_IN_MARGIN);
	/* (a jump record's 16-bit distance) */
	STATIC_ASSERT(ARRAY_LEN(s->u.recs[0]) * MLSD_LANES <= 0x8000);
	for (;;) {
		/* Start a round, or near the end of the input only execute. */
		nsteps = nrec < MLSD_GROW ? MLSD_MIN_STEPS : MLSD_ROUND_STEPS;
		dist = MAX(rate16 * nsteps / 16, margin + 64) - margin;
		if (end_pos + (MLSD_LANES - 1) * dist > in_limit) {
			if (!MLSD_INTERLEAVE || xp == NULL)
				break;
			rp = rend = NULL;
		} else {
			recs = s->u.recs[cur][0];
			rp = recs;
			cnt = nsteps;
			rend = rp + cnt * MLSD_LANES;
			MLSD_UNROLL_LOOP
			for (i = 0; i < MLSD_LANES; i++)
				p[i] = end_pos + (u64)i * dist;
		}
		r = rp != NULL ? (total + MLSD_LANES) / nsteps + 1 : (size_t)-1;
		for (;;) {
			/* Where lane i started, and its bits */
			u64 start = end_pos;
			size_t adv, lo = 0;

			/*
			 * The rows of steps.  With MLSD_INTERLEAVE, each comes
			 * after some verified records of the round before.
			 */
			for (;;) {
#if MLSD_INTERLEAVE
				if (xp != NULL) {
					size_t k = r;

					MLSD_EXEC(--k != 0);
				}
#endif
				if (rp == rend)
					break;
				MLSD_UNROLL_LOOP
				for (i = 0; i < MLSD_LANES; i++)
					MLSD_STEP(in, p[i], &rp[i]);
				rp += MLSD_LANES;
			}
			if (rp == NULL)
				break;
			ASSERT(xp == NULL);
			MLSD_UNROLL_LOOP
			for (i = 0; i < MLSD_LANES; i++)
				rend[i] = p[i];
			/*
			 * Chain the verified records: lane i+1 takes over where
			 * it has a step at lane i's end, and lane i's entry in
			 * the row at rend becomes the jump record.  If a lane
			 * has no such step, all the lanes get MLSD_EXTEND more
			 * steps (or that lane ends the round).  The margin
			 * settles where 1/7 of the lanes need that.
			 */
			total = 0;
			NO_UNROLL_LOOP
			for (i = 0; i < MLSD_LANES; i++, start += dist) {
				u64 * const jump = &rend[i];
				const u64 * const next = &recs[i + 1];
				size_t h = 0;

				*jump = MLSD_POS(*jump) | MLSD_JUMP;
				adv = (u16)(MLSD_POS(*jump) - (u16)start);
				total += cnt - lo;
				if (i + 1 == MLSD_LANES)
					break;
#ifdef __clang__	/* (gcc 11 ignores it here, with a warning) */
				NO_UNROLL_LOOP
#endif
				while (adv >= dist && h < cnt &&
				       (u16)(MLSD_POS(next[h * MLSD_LANES]) -
					     (u16)(start + dist)) < adv - dist)
					h++;
				if (adv < dist ||
				    MLSD_POS(next[h * MLSD_LANES]) !=
				    MLSD_POS(*jump)) {
					if (cnt == nsteps)
						margin += 20;
					break;
				}
				if (cnt == nsteps && margin > 32)
					margin -= 4;
				*jump |= (u64)(u16)(&next[h * MLSD_LANES] -
						    jump) << 16;
				lo = h;
			}
			if (i + 1 != MLSD_LANES &&
			    cnt != nsteps + MLSD_CATCHUP) {
				cnt += MLSD_EXTEND;
				rend += MLSD_EXTEND * MLSD_LANES;
				continue;
			}
			xp = recs;
			xstart = end_pos;
			xdist = dist;
			rate16 = (size_t)(start + adv - end_pos) * 16 / total;
			end_pos = start + adv;
			margin = MIN(margin, 1024);
			nrec += total;
			cur ^= MLSD_INTERLEAVE;
			/*
			 * Go on to the next round, unless the block probably
			 * ends in this one: then don't decode past its end.
			 */
#if MLSD_INTERLEAVE
			if (!eob)
				break;
			eob = 0;
			r = (size_t)-1;
			rp = rend = NULL;
#else
			MLSD_EXEC(true);
			break;
#endif
		}
	}
	if (0) {
stopped:
		/* (at end-of-block or out of space) */
		end_pos = MLSD_XPOS();
	}
	state->rate16 = rate16 - 12 * 16;
	state->margin = margin - 256;
	ctx->bitbuf = get_unaligned_leword(MLSD_ADDR(in, end_pos)) >>
		      (end_pos & 7);
	ctx->in_next = MLSD_ADDR(in, end_pos) + sizeof(bitbuf_t) - 1;
	ctx->bitsleft = (MAX_BITSLEFT & ~7) - (end_pos & 7);
	ctx->out_next = out;
	return res;
}
#undef MLSD_EXEC
#undef MLSD_XPOS
#undef MLSD_STEP
