/*
 * x86/crc32_pclmul_template.h - gzip CRC-32 with PCLMULQDQ instructions
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

/*
 * This file is a "template" for instantiating PCLMULQDQ-based crc32_x86
 * functions.  The "parameters" are:
 *
 * SUFFIX:
 *	Name suffix to append to all instantiated functions.
 * ATTRIBUTES:
 *	Target function attributes to use.  Must satisfy the dependencies of the
 *	other parameters as follows:
 *	   VL=16 && FOLD_LESSTHAN16BYTES=0: at least pclmul
 *	   VL=16 && FOLD_LESSTHAN16BYTES=1: at least pclmul,sse4.1
 *	   VL=32 && USE_TERNARYLOGIC=0: at least vpclmulqdq,pclmul,avx2
 *	   VL=32 && USE_TERNARYLOGIC=1: at least vpclmulqdq,pclmul,avx512vl
 *	   VL=64: at least vpclmulqdq,pclmul,avx512vl
 * VL:
 *	Vector length in bytes.  Supported values are 16, 32, and 64.
 * FOLD_LESSTHAN16BYTES:
 *	Use vector instructions to handle any partial blocks at the beginning
 *	and end, instead of falling back to scalar instructions for those parts.
 * USE_TERNARYLOGIC:
 *	Use the vpternlog instruction to do three-argument XORs.
 *
 * The overall algorithm used is CRC folding with carryless multiplication
 * instructions.  Note that the x86 crc32 instruction cannot be used, as it is
 * for a different polynomial, not the gzip one.  For an explanation of CRC
 * folding with carryless multiplication instructions, see
 * scripts/gen_crc32_multipliers.c and the following paper:
 *
 *	"Fast CRC Computation for Generic Polynomials Using PCLMULQDQ Instruction"
 *	https://www.intel.com/content/dam/www/public/us/en/documents/white-papers/fast-crc-computation-generic-polynomials-pclmulqdq-paper.pdf
 *
 * The original pclmulqdq instruction does one 64x64 to 128-bit carryless
 * multiplication.  The VPCLMULQDQ feature added instructions that do two
 * parallel 64x64 to 128-bit carryless multiplications in combination with AVX
 * or AVX512VL, or four in combination with AVX512F.
 */

#undef fold_vec128
static forceinline ATTRIBUTES __m128i
ADD_SUFFIX(fold_vec128)(__m128i src, __m128i dst, __m128i multipliers)
{
	dst = _mm_xor_si128(dst, _mm_clmulepi64_si128(src, multipliers, 0x00));
	dst = _mm_xor_si128(dst, _mm_clmulepi64_si128(src, multipliers, 0x11));
	return dst;
}
#define fold_vec128	ADD_SUFFIX(fold_vec128)

#if VL >= 32
#undef fold_vec256
static forceinline ATTRIBUTES __m256i
ADD_SUFFIX(fold_vec256)(__m256i src, __m256i dst, __m256i multipliers)
{
#if USE_TERNARYLOGIC
	return _mm256_ternarylogic_epi32(
			_mm256_clmulepi64_epi128(src, multipliers, 0x00),
			_mm256_clmulepi64_epi128(src, multipliers, 0x11),
			dst,
			0x96);
#else
	return _mm256_xor_si256(
			_mm256_xor_si256(dst,
					 _mm256_clmulepi64_epi128(src, multipliers, 0x00)),
			_mm256_clmulepi64_epi128(src, multipliers, 0x11));
#endif
}
#define fold_vec256	ADD_SUFFIX(fold_vec256)
#endif /* VL >= 32 */

#if VL >= 64
#undef fold_vec512
static forceinline ATTRIBUTES __m512i
ADD_SUFFIX(fold_vec512)(__m512i src, __m512i dst, __m512i multipliers)
{
	return _mm512_ternarylogic_epi32(
			_mm512_clmulepi64_epi128(src, multipliers, 0x00),
			_mm512_clmulepi64_epi128(src, multipliers, 0x11),
			dst,
			0x96);
}
#define fold_vec512	ADD_SUFFIX(fold_vec512)
#endif /* VL >= 64 */

#if VL == 16
#  define vec_t			__m128i
#  define fold_vec		fold_vec128
#  define VLOAD_UNALIGNED(p)	_mm_loadu_si128((const void *)(p))
#  define VXOR(a, b)		_mm_xor_si128((a), (b))
#  define M128I_TO_VEC(a)	a
#  define MULTS_8V		_mm_set_epi64x(CRC32_X991_MODG, CRC32_X1055_MODG)
#  define MULTS_4V		_mm_set_epi64x(CRC32_X479_MODG, CRC32_X543_MODG)
#  define MULTS_2V		_mm_set_epi64x(CRC32_X223_MODG, CRC32_X287_MODG)
#  define MULTS_1V		_mm_set_epi64x(CRC32_X95_MODG, CRC32_X159_MODG)
#elif VL == 32
#  define vec_t			__m256i
#  define fold_vec		fold_vec256
#  define VLOAD_UNALIGNED(p)	_mm256_loadu_si256((const void *)(p))
#  define VXOR(a, b)		_mm256_xor_si256((a), (b))
#  define M128I_TO_VEC(a)	_mm256_castsi128_si256(a)
#  define MULTS(a, b)		_mm256_set_epi64x(a, b, a, b)
#  define MULTS_8V		MULTS(CRC32_X2015_MODG, CRC32_X2079_MODG)
#  define MULTS_4V		MULTS(CRC32_X991_MODG, CRC32_X1055_MODG)
#  define MULTS_2V		MULTS(CRC32_X479_MODG, CRC32_X543_MODG)
#  define MULTS_1V		MULTS(CRC32_X223_MODG, CRC32_X287_MODG)
#elif VL == 64
#  define vec_t			__m512i
#  define fold_vec		fold_vec512
#  define VLOAD_UNALIGNED(p)	_mm512_loadu_si512((const void *)(p))
#  define VXOR(a, b)		_mm512_xor_si512((a), (b))
#  define M128I_TO_VEC(a)	_mm512_castsi128_si512(a)
#  define MULTS(a, b)		_mm512_set_epi64(a, b, a, b, a, b, a, b)
#  define MULTS_8V		MULTS(CRC32_X4063_MODG, CRC32_X4127_MODG)
#  define MULTS_4V		MULTS(CRC32_X2015_MODG, CRC32_X2079_MODG)
#  define MULTS_2V		MULTS(CRC32_X991_MODG, CRC32_X1055_MODG)
#  define MULTS_1V		MULTS(CRC32_X479_MODG, CRC32_X543_MODG)
#else
#  error "unsupported vector length"
#endif

#if FOLD_LESSTHAN16BYTES
/*
 * Given 'x' containing a 16-byte polynomial, and a pointer 'p' that points to
 * the next '1 <= len <= 15' data bytes, rearrange the concatenation of 'x' and
 * the data into vectors x0 and x1 that contain 'len' bytes and 16 bytes,
 * respectively.  Then fold x0 into x1 and return the result.
 * Assumes that 'p + len - 16' is in-bounds.
 */
#undef fold_lessthan16bytes
static forceinline ATTRIBUTES __m128i
ADD_SUFFIX(fold_lessthan16bytes)(__m128i x, const u8 *p, size_t len,
				 __m128i /* __v2du */ multipliers_128b)
{
	/*
	 * pshufb(x, shift_tab[len..len+15]) left shifts x by 16-len bytes.
	 * pshufb(x, shift_tab[len+16..len+31]) right shifts x by len bytes.
	 */
	static const u8 shift_tab[48] = {
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
		0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	};
	__m128i lshift = _mm_loadu_si128((const void *)&shift_tab[len]);
	__m128i rshift = _mm_loadu_si128((const void *)&shift_tab[len + 16]);
	__m128i x0, x1;

	/* x0 = x left-shifted by '16 - len' bytes */
	x0 = _mm_shuffle_epi8(x, lshift);

	/*
	 * x1 = the last '16 - len' bytes from x (i.e. x right-shifted by 'len'
	 * bytes) followed by the remaining data.
	 */
	x1 = _mm_blendv_epi8(_mm_shuffle_epi8(x, rshift),
			     _mm_loadu_si128((const void *)(p + len - 16)),
			     /* msb 0/1 of each byte selects byte from arg1/2 */
			     rshift);

	return fold_vec128(x0, x1, multipliers_128b);
}
#define fold_lessthan16bytes	ADD_SUFFIX(fold_lessthan16bytes)
#endif /* FOLD_LESSTHAN16BYTES */

static u32 ATTRIBUTES
ADD_SUFFIX(crc32_x86)(u32 crc, const u8 *p, size_t len)
{
	const vec_t multipliers_8v = MULTS_8V; /* 8 vecs */
	const vec_t multipliers_4v = MULTS_4V; /* 4 vecs */
	const vec_t multipliers_2v = MULTS_2V; /* 2 vecs */
	const vec_t multipliers_1v = MULTS_1V; /* 1 vecs */
	const __m128i /* __v2du */ multipliers_128b =
		_mm_set_epi64x(CRC32_X95_MODG, CRC32_X159_MODG);
	const __m128i /* __v2du */ final_multiplier =
		_mm_set_epi64x(0, CRC32_X63_MODG);
	const __m128i mask32 = _mm_set_epi32(0, 0, 0, 0xFFFFFFFF);
	const __m128i /* __v2du */ barrett_reduction_constants =
		_mm_set_epi64x(CRC32_BARRETT_CONSTANT_2,
			       CRC32_BARRETT_CONSTANT_1);
	vec_t v0, v1, v2, v3, v4, v5, v6, v7;
	__m128i x0, x1;

	/*
	 * There are two overall code paths.  The first path supports all
	 * lengths, but is intended for short lengths; it uses unaligned loads
	 * and does at most 4-way folds.  The second path only supports longer
	 * lengths, aligns the pointer in order to do aligned loads, and does up
	 * to 8-way folds.  The length check below decides which path to take.
	 */
	if (len < 64*VL) {
		if (len < VL)
			return crc32_slice1(crc, p, len);

		v0 = VXOR(VLOAD_UNALIGNED(p),
			  M128I_TO_VEC(_mm_cvtsi32_si128(crc)));
		p += VL;

		if (len >= 4*VL) {
			v1 = VLOAD_UNALIGNED(p + 0*VL);
			v2 = VLOAD_UNALIGNED(p + 1*VL);
			v3 = VLOAD_UNALIGNED(p + 2*VL);
			p += 3*VL;
			while (len >= 8*VL) {
				v0 = fold_vec(v0, VLOAD_UNALIGNED(p + 0*VL),
					      multipliers_4v);
				v1 = fold_vec(v1, VLOAD_UNALIGNED(p + 1*VL),
					      multipliers_4v);
				v2 = fold_vec(v2, VLOAD_UNALIGNED(p + 2*VL),
					      multipliers_4v);
				v3 = fold_vec(v3, VLOAD_UNALIGNED(p + 3*VL),
					      multipliers_4v);
				p += 4*VL;
				len -= 4*VL;
			}
			v0 = fold_vec(v0, v2, multipliers_2v);
			v1 = fold_vec(v1, v3, multipliers_2v);
			if (len & (2*VL)) {
				v0 = fold_vec(v0, VLOAD_UNALIGNED(p + 0*VL),
					      multipliers_2v);
				v1 = fold_vec(v1, VLOAD_UNALIGNED(p + 1*VL),
					      multipliers_2v);
				p += 2*VL;
			}
			v0 = fold_vec(v0, v1, multipliers_1v);
			if (len & VL) {
				v0 = fold_vec(v0, VLOAD_UNALIGNED(p),
					      multipliers_1v);
				p += VL;
			}
		} else {
			if (len >= 2*VL) {
				v0 = fold_vec(v0, VLOAD_UNALIGNED(p),
					      multipliers_1v);
				p += VL;
				if (len >= 3*VL) {
					v0 = fold_vec(v0, VLOAD_UNALIGNED(p),
						      multipliers_1v);
					p += VL;
				}
			}
		}
	} else {
		size_t align = -(uintptr_t)p & (VL-1);
		const vec_t *vp;

		/* Align p to the next VL-byte boundary. */
		if (align == 0) {
			vp = (const vec_t *)p;
			v0 = VXOR(*vp++, M128I_TO_VEC(_mm_cvtsi32_si128(crc)));
		} else {
			len -= align;
		#if FOLD_LESSTHAN16BYTES
			x0 = _mm_xor_si128(_mm_loadu_si128((const void *)p),
					   _mm_cvtsi32_si128(crc));
			p += 16;
			if (align & 15) {
				x0 = fold_lessthan16bytes(x0, p, align & 15,
							  multipliers_128b);
				p += align & 15;
				align &= ~15;
			}
			while (align >= 16) {
				x0 = fold_vec128(x0, *(const __m128i *)p,
						 multipliers_128b);
				p += 16;
				align -= 16;
			}
			v0 = M128I_TO_VEC(x0);
		#  if VL == 32
			v0 = _mm256_inserti128_si256(v0, *(const __m128i *)p, 1);
			p += 16;
		#  elif VL == 64
			v0 = _mm512_inserti32x4(v0, *(const __m128i *)p, 1);
			p += 16;
			v0 = _mm512_inserti64x4(v0, *(const __m256i *)p, 1);
			p += 32;
		#  endif
			vp = (const vec_t *)p;
		#else
			crc = crc32_slice1(crc, p, align);
			p += align;
			vp = (const vec_t *)p;
			v0 = VXOR(*vp++, M128I_TO_VEC(_mm_cvtsi32_si128(crc)));
		#endif
		}
		v1 = *vp++;
		v2 = *vp++;
		v3 = *vp++;
		v4 = *vp++;
		v5 = *vp++;
		v6 = *vp++;
		v7 = *vp++;
		do {
			v0 = fold_vec(v0, *vp++, multipliers_8v);
			v1 = fold_vec(v1, *vp++, multipliers_8v);
			v2 = fold_vec(v2, *vp++, multipliers_8v);
			v3 = fold_vec(v3, *vp++, multipliers_8v);
			v4 = fold_vec(v4, *vp++, multipliers_8v);
			v5 = fold_vec(v5, *vp++, multipliers_8v);
			v6 = fold_vec(v6, *vp++, multipliers_8v);
			v7 = fold_vec(v7, *vp++, multipliers_8v);
			len -= 8*VL;
		} while (len >= 16*VL);

		/*
		 * Reduce v0-v7 (length 8*VL bytes) to v0 (length VL bytes)
		 * and fold in any VL-byte data segments that remain.
		 */
		v0 = fold_vec(v0, v4, multipliers_4v);
		v1 = fold_vec(v1, v5, multipliers_4v);
		v2 = fold_vec(v2, v6, multipliers_4v);
		v3 = fold_vec(v3, v7, multipliers_4v);
		if (len & (4*VL)) {
			v0 = fold_vec(v0, *vp++, multipliers_4v);
			v1 = fold_vec(v1, *vp++, multipliers_4v);
			v2 = fold_vec(v2, *vp++, multipliers_4v);
			v3 = fold_vec(v3, *vp++, multipliers_4v);
		}
		v0 = fold_vec(v0, v2, multipliers_2v);
		v1 = fold_vec(v1, v3, multipliers_2v);
		if (len & (2*VL)) {
			v0 = fold_vec(v0, *vp++, multipliers_2v);
			v1 = fold_vec(v1, *vp++, multipliers_2v);
		}
		v0 = fold_vec(v0, v1, multipliers_1v);
		if (len & VL)
			v0 = fold_vec(v0, *vp++, multipliers_1v);
		p = (const u8 *)vp;
	}

	/*
	 * Reduce v0 (length VL bytes) to x0 (length 16 bytes)
	 * and fold in any 16-byte data segments that remain.
	 */
#if VL == 16
	x0 = v0;
#else
	{
#  if VL == 32
		__m256i y0 = v0;
#  else
		const __m256i multipliers_256b =
			_mm256_set_epi64x(CRC32_X223_MODG, CRC32_X287_MODG,
					  CRC32_X223_MODG, CRC32_X287_MODG);
		__m256i y0 = fold_vec256(_mm512_extracti64x4_epi64(v0, 0),
					 _mm512_extracti64x4_epi64(v0, 1),
					 multipliers_256b);
		if (len & 32) {
			y0 = fold_vec256(y0, _mm256_loadu_si256((const void *)p),
					 multipliers_256b);
			p += 32;
		}
#  endif
		x0 = fold_vec128(_mm256_extracti128_si256(y0, 0),
				 _mm256_extracti128_si256(y0, 1),
				 multipliers_128b);
	}
	if (len & 16) {
		x0 = fold_vec128(x0, _mm_loadu_si128((const void *)p),
				 multipliers_128b);
		p += 16;
	}
#endif
	len &= 15;

	/*
	 * If fold_lessthan16bytes() is available, handle any remainder
	 * of 1 to 15 bytes now, before reducing to 32 bits.
	 */
#if FOLD_LESSTHAN16BYTES
	if (len)
		x0 = fold_lessthan16bytes(x0, p, len, multipliers_128b);
#endif

	/*
	 * Fold 128 => 96 bits.  This also implicitly appends 32 zero bits,
	 * which is equivalent to multiplying by x^32.  This is needed because
	 * the CRC is defined as M(x)*x^32 mod G(x), not just M(x) mod G(x).
	 */
	x0 = _mm_xor_si128(_mm_srli_si128(x0, 8),
			   _mm_clmulepi64_si128(x0, multipliers_128b, 0x10));

	/* Fold 96 => 64 bits. */
	x0 = _mm_xor_si128(_mm_srli_si128(x0, 4),
			   _mm_clmulepi64_si128(_mm_and_si128(x0, mask32),
						final_multiplier, 0x00));

	/*
	 * Reduce 64 => 32 bits using Barrett reduction.
	 *
	 * Let M(x) = A(x)*x^32 + B(x) be the remaining message.  The goal is to
	 * compute R(x) = M(x) mod G(x).  Since degree(B(x)) < degree(G(x)):
	 *
	 *	R(x) = (A(x)*x^32 + B(x)) mod G(x)
	 *	     = (A(x)*x^32) mod G(x) + B(x)
	 *
	 * Then, by the Division Algorithm there exists a unique q(x) such that:
	 *
	 *	A(x)*x^32 mod G(x) = A(x)*x^32 - q(x)*G(x)
	 *
	 * Since the left-hand side is of maximum degree 31, the right-hand side
	 * must be too.  This implies that we can apply 'mod x^32' to the
	 * right-hand side without changing its value:
	 *
	 *	(A(x)*x^32 - q(x)*G(x)) mod x^32 = q(x)*G(x) mod x^32
	 *
	 * Note that '+' is equivalent to '-' in polynomials over GF(2).
	 *
	 * We also know that:
	 *
	 *	              / A(x)*x^32 \
	 *	q(x) = floor (  ---------  )
	 *	              \    G(x)   /
	 *
	 * To compute this efficiently, we can multiply the top and bottom by
	 * x^32 and move the division by G(x) to the top:
	 *
	 *	              / A(x) * floor(x^64 / G(x)) \
	 *	q(x) = floor (  -------------------------  )
	 *	              \           x^32            /
	 *
	 * Note that floor(x^64 / G(x)) is a constant.
	 *
	 * So finally we have:
	 *
	 *	                          / A(x) * floor(x^64 / G(x)) \
	 *	R(x) = B(x) + G(x)*floor (  -------------------------  )
	 *	                          \           x^32            /
	 */
	x1 = _mm_clmulepi64_si128(_mm_and_si128(x0, mask32),
				  barrett_reduction_constants, 0x00);
	x1 = _mm_clmulepi64_si128(_mm_and_si128(x1, mask32),
				  barrett_reduction_constants, 0x10);
	x0 = _mm_xor_si128(x0, x1);
#if FOLD_LESSTHAN16BYTES
	crc = _mm_extract_epi32(x0, 1);
#else
	crc = _mm_cvtsi128_si32(_mm_shuffle_epi32(x0, 0x01));
	/* Process up to 15 bytes left over at the end. */
	crc = crc32_slice1(crc, p, len);
#endif
	return crc;
}

#undef vec_t
#undef fold_vec
#undef VLOAD_UNALIGNED
#undef VXOR
#undef M128I_TO_VEC
#undef MULTS
#undef MULTS_8V
#undef MULTS_4V
#undef MULTS_2V
#undef MULTS_1V

#undef SUFFIX
#undef ATTRIBUTES
#undef VL
#undef FOLD_LESSTHAN16BYTES
#undef USE_TERNARYLOGIC
