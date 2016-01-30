/*
 * compiler-msc.h - definitions for the Microsoft C Compiler
 */

#include <inttypes.h>
#include <stdlib.h>

#define BUILDING_LIBDEFLATE

/* MSC __restrict has nonstandard behavior.  Don't use it.  */
#define restrict

#define LIBEXPORT			__declspec(dllexport)
#define forceinline			__forceinline

/* Assume a little endian architecture with fast unaligned access.  */
#define CPU_IS_LITTLE_ENDIAN()		1
#define UNALIGNED_ACCESS_IS_FAST	1

#define compiler_bswap16		_byteswap_ushort
#define compiler_bswap32		_byteswap_ulong
#define compiler_bswap64		_byteswap_uint64

static forceinline unsigned
compiler_fls32(uint32_t n)
{
	_BitScanReverse(&n, n);
	return n;
}
#define compiler_fls32	compiler_fls32

static forceinline unsigned
compiler_ffs32(uint32_t n)
{
	_BitScanForward(&n, n);
	return n;
}
#define compiler_ffs32	compiler_ffs32

#ifdef _M_X64
static forceinline unsigned
compiler_fls64(uint64_t n)
{
	_BitScanReverse64(&n, n);
	return n;
}
#define compiler_fls64	compiler_fls64

static forceinline unsigned
compiler_ffs64(uint64_t n)
{
	_BitScanForward64(&n, n);
	return n;
}
#define compiler_ffs64	compiler_ffs64
#endif /* _M_X64 */
