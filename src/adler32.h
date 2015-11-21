/*
 * adler32.h - Adler-32 checksum algorithm
 */

#pragma once

#include "util.h"

extern u32
adler32(const void *buffer, size_t size);
