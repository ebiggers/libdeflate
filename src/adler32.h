/*
 * adler32.h
 *
 * Adler-32 checksum algorithm.
 */

#pragma once

#include "types.h"

extern u32
adler32(const void *buffer, size_t size);
