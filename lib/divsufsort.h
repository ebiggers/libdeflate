#ifndef LIB_DIVSUFSORT_H
#define LIB_DIVSUFSORT_H

#include "lib_common.h"

extern void
divsufsort(const u8 *T, u32 *SA, u32 n, u32 *tmp);

#define DIVSUFSORT_TMP_LEN (256 + (256 * 256))

#endif /* LIB_DIVSUFSORT_H */
