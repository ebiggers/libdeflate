/*
 * aligned_malloc.c
 *
 * Aligned memory allocation.
 *
 * This file has no copyright assigned and is placed in the Public Domain.
 */

#pragma once

#include <stddef.h>

extern void *aligned_malloc(size_t alignment, size_t size);
extern void aligned_free(void *ptr);
