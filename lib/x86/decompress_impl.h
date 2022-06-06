#ifndef LIB_X86_DECOMPRESS_IMPL_H
#define LIB_X86_DECOMPRESS_IMPL_H

#include "cpu_features.h"

/* BMI2 optimized version */
#if HAVE_BMI2_TARGET && !HAVE_BMI2_NATIVE
#  define FUNCNAME		deflate_decompress_bmi2
#  define ATTRIBUTES		__attribute__((target("bmi2")))
#  include "../decompress_template.h"
static inline decompress_func_t
arch_select_decompress_func(void)
{
	if (HAVE_BMI2(get_x86_cpu_features()))
		return deflate_decompress_bmi2;
	return NULL;
}
#  define arch_select_decompress_func	arch_select_decompress_func
#endif

#endif /* LIB_X86_DECOMPRESS_IMPL_H */
