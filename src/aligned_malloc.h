/*
 * aligned_malloc.c - aligned memory allocation
 */

#pragma once

#include <stddef.h>

extern void *aligned_malloc(size_t alignment, size_t size);
extern void aligned_free(void *ptr);
