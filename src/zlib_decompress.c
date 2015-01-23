/*
 * zlib_decompress.c
 *
 * Decompress DEFLATE-compressed data wrapped in the zlib format.
 *
 * This file has no copyright assigned and is placed in the Public Domain.
 */

#include "libdeflate.h"

#include "adler32.h"
#include "unaligned.h"
#include "zlib_constants.h"

LIBEXPORT bool
zlib_decompress(struct deflate_decompressor *d,
		const void *in, size_t in_nbytes, void *out, size_t out_nbytes)
{
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	u16 hdr;

	if (in_nbytes < ZLIB_MIN_OVERHEAD)
		return false;

	/* 2 byte header: CMF and FLG  */
	hdr = get_unaligned_u16_be(in_next);
	in_next += 2;

	/* FCHECK */
	if ((hdr % 31) != 0)
		return false;

	/* CM */
	if (((hdr >> 8) & 0xF) != ZLIB_CM_DEFLATE)
		return false;

	/* CINFO */
	if ((hdr >> 12) > ZLIB_CINFO_32K_WINDOW)
		return false;

	/* FDICT */
	if ((hdr >> 5) & 1)
		return false;

	/* Compressed data  */
	if (!deflate_decompress(d, in_next, in_end - ZLIB_FOOTER_SIZE - in_next,
				out, out_nbytes))
		return false;

	in_next = in_end - ZLIB_FOOTER_SIZE;

	/* ADLER32  */
	if (adler32(out, out_nbytes) != get_unaligned_u32_be(in_next))
		return false;

	return true;
}
