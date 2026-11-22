/*
 * x86/decompress_impl.h - x86 decompression implementation
 *
 * Copyright 2026 Google LLC
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

#ifndef LIB_X86_DECOMPRESS_IMPL_H
#define LIB_X86_DECOMPRESS_IMPL_H

#include "cpu_features.h"

/*
 * BMI, BMI2, LZCNT, and SSSE3 optimized decompression function.  The main
 * benefit comes from BMI2; the others are included because they provide a
 * slight benefit too and all real CPUs with BMI2 also support them.
 *
 * With gcc and clang we just compile the whole function with the appropriate
 * target attribute, and we mostly (but not always) rely on the compiler
 * generating the instructions automatically rather than using intrinsics.
 *
 * With MSVC, there is no target function attribute, so we use _bzhi_u32() and
 * SSE2/SSSE3 intrinsics explicitly.  However, MSVC from VS2017 (toolset v141)
 * apparently miscompiles the _bzhi_*() intrinsics.  It seems to be fixed in
 * VS2022.  Hence, use MSVC_PREREQ(1930).
 */
#if defined(__GNUC__) || defined(__clang__) || MSVC_PREREQ(1930)
#  define deflate_decompress_bmi2	deflate_decompress_bmi2
#  define SUFFIX			_bmi2
#  define ATTRIBUTES			_target_attribute("bmi2,bmi,lzcnt,ssse3")
   /*
    * Use the bzhi intrinsic rather than rely on the compiler to lower 'word &
    * BITMASK(count)' to bzhi.  gcc doesn't generate bzhi reliably without it,
    * and MSVC doesn't generate bzhi at all without it.  clang generates bzhi
    * regardless, but using the intrinsic avoids relying on it to fold away
    * '(u8)' on the count and lets us use _bzhi_u32 cleanly (see below).
    *
    * For count < 32, EXTRACT_VARBITS() is equivalent to
    * 'word & BITMASK(count)'.  For (u8)count < 32, EXTRACT_VARBITS8() is
    * equivalent to 'word & BITMASK((u8)count)'.  Nevertheless, their
    * implementation using the bzhi intrinsic is identical, as the bzhi
    * instruction truncates the count to 8 bits implicitly.
    *
    * Since '(u8)count < 32', either _bzhi_u32 or _bzhi_u64 works.  Prefer
    * _bzhi_u32 because it returns u32 rather than u64, which lets both gcc and
    * clang use 32-bit arithmetic (e.g., 32-bit shrx and add instead of 64-bit
    * shrx and lea chains for table indexing), avoids shift-count masking on
    * clang, and works uniformly on both 32-bit and 64-bit x86.
    */
#  define EXTRACT_VARBITS(word, count)  _bzhi_u32((u32)(word), (count))
#  define EXTRACT_VARBITS8(word, count) _bzhi_u32((u32)(word), (count))

/*
 * SSSE3 implementation of match copying.
 *
 * For a long time libdeflate didn't have this, as the word-at-a-time scalar
 * copy already does incredibly well.  If it's implemented properly, though,
 * (including having only two main cases, utilizing the pshufb trick for any
 * offset <= 15), an SSSE3 implementation now seems to provide a slight speedup
 * when combined with other changes to the decompression code.
 *
 * This code is executed only when BMI2 is also available.  All real CPUs with
 * BMI2 also have AVX2, so we could use AVX2 here (if it were added to
 * ATTRIBUTES and the CPU feature checks also required it to be safe).  We
 * currently don't, as using AVX2 here still seems to be worse due to the
 * downsides of enabling the AVX2 target and using 256-bit vectors, such as:
 * needing to restrict the main case to offset >= 32, vpshufb operating
 * independently within each 128-bit lane, warm-up latency, extra instructions
 * being generated to align the stack and do vzeroupper, the compiler doing
 * unhelpful auto-vectorization elsewhere, etc.
 */
static forceinline ATTRIBUTES void
copy_match_ssse3(u8 *dst, const u8 *src, u32 offset, const u8 *out_next)
{
	/* Overrun up to 47 bytes is allowed; see FASTLOOP_MAX_MATCH_OVERRUN. */
	STATIC_ASSERT(FASTLOOP_MAX_MATCH_OVERRUN >= 47);

	/*
	 * offset >= 16: copy 16 bytes at a time.  Since the source and
	 * destination of each 16-byte copy don't overlap, this works even when
	 * the match overlaps itself (offset < length).  Unconditionally copying
	 * 48 bytes covers most matches without a loop.
	 */
	if (likely(offset >= 16)) {
		_mm_storeu_si128((__m128i *)(dst + 0),
				 _mm_loadu_si128((const __m128i *)(src + 0)));
		_mm_storeu_si128((__m128i *)(dst + 16),
				 _mm_loadu_si128((const __m128i *)(src + 16)));
		_mm_storeu_si128((__m128i *)(dst + 32),
				 _mm_loadu_si128((const __m128i *)(src + 32)));
		dst += 48;
		src += 48;
		if (unlikely(dst < out_next)) {
			NO_VECTORIZE_LOOP
			do {
				_mm_storeu_si128((__m128i *)(dst + 0),
						 _mm_loadu_si128((const __m128i *)(src + 0)));
				_mm_storeu_si128((__m128i *)(dst + 16),
						 _mm_loadu_si128((const __m128i *)(src + 16)));
				dst += 32;
				src += 32;
			} while (dst < out_next);
		}
	} else {
		/*
		 * 1 <= offset <= 15: use pshufb to repeat the 'offset'-byte
		 * pattern across a 16-byte vector, then store it at stride
		 * 'step = (16 / offset) * offset' (in 9..16) with no reloads.
		 *
		 * Loading 16 bytes from 'src' is safe even when offset < 16:
		 * fastloop output slack prevents overreading 'out_end', and
		 * lanes offset..15 (which overlap dst[0..15-offset] and may be
		 * uninitialized) are discarded by pshufb since all indices in
		 * repeat_table[offset] are in 0..offset-1.  The 2 initial
		 * stores write up to step + 16 <= 32 bytes (overrun <= 29 for
		 * length >= 3), and any loop stores start at dst < out_next
		 * (overrun <= 15), well within FASTLOOP_MAX_MATCH_OVERRUN.
		 *
		 * Row 0 is unused as a shuffle mask (since offset >= 1), so we
		 * store the stride table '(16 / offset) * offset' there to keep
		 * the entire table 256 bytes instead of 272 bytes and avoid a
		 * +256 displacement on memory operands.
		 */
		static const u8 _aligned_attribute(16) repeat_table[16][16] = {
			/* [0]: strides repeat_table[0][offset] for offset 1..15 */
			{ 0, 16, 16, 15, 16, 15, 12, 14, 16, 9, 10, 11, 12, 13, 14, 15 },
			/* [1..15]: shuffle masks for offset 1..15 */
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
		__m128i mask;
		__m128i v;
		size_t step;

		ASSERT(offset >= 1 && offset <= 15);
#if __has_feature(memory_sanitizer)
		/*
		 * LLVM's MemorySanitizer doesn't model _mm_shuffle_epi8() lane
		 * selection and instead propagates shadow lane-wise from src.
		 * Initialize dst[0..15] (which covers src[offset..15] without
		 * touching src[0..offset-1]) before loading from src.
		 */
		_mm_storeu_si128((__m128i *)dst, _mm_setzero_si128());
#endif
		mask = _mm_load_si128((const __m128i *)repeat_table[offset]);
		v = _mm_shuffle_epi8(_mm_loadu_si128((const __m128i *)src),
				     mask);
		step = repeat_table[0][offset];

		_mm_storeu_si128((__m128i *)dst, v);
		dst += step;
		_mm_storeu_si128((__m128i *)dst, v);
		dst += step;
		if (unlikely(dst < out_next)) {
			NO_VECTORIZE_LOOP
			do {
				_mm_storeu_si128((__m128i *)dst, v);
				dst += step;
			} while (dst < out_next);
		}
	}
}
#  define COPY_MATCH	copy_match_ssse3
#  include "../decompress_template.h"
#endif

#if defined(deflate_decompress_bmi2) && HAVE_BMI2_NATIVE
#define DEFAULT_IMPL	deflate_decompress_bmi2
#else
static inline decompress_func_t
arch_select_decompress_func(void)
{
#ifdef deflate_decompress_bmi2
	if (HAVE_BMI2(get_x86_cpu_features()))
		return deflate_decompress_bmi2;
#endif
	return NULL;
}
#define arch_select_decompress_func	arch_select_decompress_func
#endif

#endif /* LIB_X86_DECOMPRESS_IMPL_H */
