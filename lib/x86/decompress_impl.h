#ifndef LIB_X86_DECOMPRESS_IMPL_H
#define LIB_X86_DECOMPRESS_IMPL_H

#include "cpu_features.h"

/* BMI2 optimized version */
#if HAVE_BMI2_TARGET
#  define FUNCNAME		deflate_decompress_bmi2
#  if HAVE_BMI2_NATIVE
#    define ATTRIBUTES
#  else
#    define ATTRIBUTES		__attribute__((target("bmi2")))
#  endif
#  include <immintrin.h>
#  ifdef __x86_64__
#    define EXTRACT_VARBITS(word, count)  _bzhi_u64((word), (count))
#    define EXTRACT_VARBITS8(word, count) _bzhi_u64((word), (count))
#  else
#    define EXTRACT_VARBITS(word, count)  _bzhi_u32((word), (count))
#    define EXTRACT_VARBITS8(word, count) _bzhi_u32((word), (count))
#  endif
#  include "../decompress_template.h"
#  if HAVE_BMI2_NATIVE
#    define DEFAULT_IMPL deflate_decompress_bmi2
#  else
static inline decompress_func_t
arch_select_decompress_func(void)
{
	if (HAVE_BMI2(get_x86_cpu_features()))
		return deflate_decompress_bmi2;
	return NULL;
}
#    define arch_select_decompress_func	arch_select_decompress_func
#  endif
#endif

#endif /* LIB_X86_DECOMPRESS_IMPL_H */
