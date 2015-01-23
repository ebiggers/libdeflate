/*
 * adler32.h
 *
 * Adler-32 checksum algorithm.
 *
 * This file has no copyright assigned and is placed in the Public Domain.
 */

#pragma once

#include "types.h"

extern u32
adler32(const void *buffer, size_t size);
