/*
 * aligned_malloc.c - aligned memory allocation
 */

#ifndef _LIB_ALIGNED_MALLOC_H
#define _LIB_ALIGNED_MALLOC_H

#include "common_defs.h"

extern void *aligned_malloc(size_t alignment, size_t size);
extern void aligned_free(void *ptr);

#endif /* _LIB_ALIGNED_MALLOC_H */
