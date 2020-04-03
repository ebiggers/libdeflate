/*
 * aligned_malloc.c - aligned memory allocation
 */

#ifndef LIB_ALIGNED_MALLOC_H
#define LIB_ALIGNED_MALLOC_H

#include "lib_common.h"

void *aligned_malloc(size_t alignment, size_t size);
void aligned_free(void *ptr);

#endif /* LIB_ALIGNED_MALLOC_H */
