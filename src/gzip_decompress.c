/*
 * gzip_decompress.c
 *
 * Decompress DEFLATE-compressed data wrapped in the gzip format.
 */

#include "libdeflate.h"

#include "crc32.h"
#include "gzip_constants.h"
#include "unaligned.h"

LIBEXPORT bool
gzip_decompress(struct deflate_decompressor *d,
		const void *in, size_t in_nbytes, void *out, size_t out_nbytes)
{
	const u8 *in_next = in;
	const u8 * const in_end = in_next + in_nbytes;
	u8 flg;

	if (in_nbytes < GZIP_MIN_OVERHEAD)
		return false;

	/* ID1 */
	if (*in_next++ != GZIP_ID1)
		return false;
	/* ID2 */
	if (*in_next++ != GZIP_ID2)
		return false;
	/* CM */
	if (*in_next++ != GZIP_CM_DEFLATE)
		return false;
	flg = *in_next++;
	/* MTIME */
	in_next += 4;
	/* XFL */
	in_next += 1;
	/* OS */
	in_next += 1;

	if (flg & GZIP_FRESERVED)
		return false;

	/* Extra field */
	if (flg & GZIP_FEXTRA) {
		u16 xlen = get_unaligned_u16_le(in_next);
		in_next += 2;

		if (in_end - in_next < (u32)xlen + GZIP_FOOTER_SIZE)
			return false;

		in_next += xlen;
	}

	/* Original file name (zero terminated) */
	if (flg & GZIP_FNAME) {
		while (*in_next != 0 && ++in_next != in_end)
			;
		if (in_next != in_end)
			in_next++;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return false;
	}

	/* File comment (zero terminated) */
	if (flg & GZIP_FCOMMENT) {
		while (*in_next != 0 && ++in_next != in_end)
			;
		if (in_next != in_end)
			in_next++;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return false;
	}

	/* CRC16 for gzip header */
	if (flg & GZIP_FHCRC) {
		in_next += 2;
		if (in_end - in_next < GZIP_FOOTER_SIZE)
			return false;
	}

	/* Compressed data  */
	if (!deflate_decompress(d, in_next, in_end - GZIP_FOOTER_SIZE - in_next,
				out, out_nbytes))
		return false;

	in_next = in_end - GZIP_FOOTER_SIZE;

	/* CRC32 */
	if (crc32(out, out_nbytes) != get_unaligned_u32_le(in_next))
		return false;
	in_next += 4;

	/* ISIZE */
	if ((u32)out_nbytes != get_unaligned_u32_le(in_next))
		return false;

	return true;
}
