/*
 * adler32.h
 *
 * Adler-32 checksum algorithm.
 */

#pragma once

#include "types.h"

extern u32
adler32(const u8 *buffer, size_t size);
