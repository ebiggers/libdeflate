# libdeflate release notes

## Version 1.12

This release focuses on improving the performance of the CRC-32 and Adler-32
checksum algorithms on x86 and ARM (both 32-bit and 64-bit).

* Build updates:

  * Fixed building libdeflate on Apple platforms.

  * For Visual Studio builds, Visual Studio 2015 or later is now required.

* CRC-32 algorithm updates:

  * Improved CRC-32 performance on short inputs on x86 and ARM.

  * Improved CRC-32 performance on Apple Silicon Macs by using a 12-way pmull
    implementation.   Performance on large inputs on M1 is now about 67 GB/s,
    compared to 8 GB/s before, or 31 GB/s with the Apple-provided zlib.

  * Improved CRC-32 performance on some other ARM CPUs by reworking the code so
    that multiple crc32 instructions can be issued in parallel.

  * Improved CRC-32 performance on some x86 CPUs by increasing the stride length
    of the pclmul implementation.

* Adler-32 algorithm updates:

  * Improved Adler-32 performance on some x86 CPUs by optimizing the AVX-2
    implementation.  E.g., performance on Zen 1 improved from 19 to 30 GB/s, and
    on Ice Lake from 35 to 41 GB/s (if the AVX-512 implementation is excluded).

  * Removed the AVX-512 implementation of Adler-32 to avoid CPU frequency
    downclocking, and because the AVX-2 implementation was made faster.

  * Improved Adler-32 performance on some ARM CPUs by optimizing the NEON
    implementation.  E.g., Apple M1 improved from about 36 to 52 GB/s.

## Version 1.11

* Library updates:

  * Improved compression performance slightly.

  * Detect arm64 CPU features on Apple platforms, which should improve
    performance in some areas such as CRC-32 computation.

* Program updates:

  * The included `gzip` and `gunzip` programs now support the `-q` option.

  * The included `gunzip` program now passes through non-gzip data when both
    the `-f` and `-c` options are used.

* Build updates:

  * Avoided a build error on arm32 with certain gcc versions, by disabling
    building `crc32_arm()` as dynamically-dispatched code when needed.

  * Support building with the LLVM toolchain on Windows.

  * Disabled the use of the "stdcall" ABI in static library builds on Windows.

  * Use the correct `install_name` in macOS builds.

  * Support Haiku builds.

## Version 1.10

* Added an additional check to the decompressor to make it quickly detect
  certain bad inputs and not try to generate an unbounded amount of output.

  Note: this was only a problem when decompressing with an unknown output size,
  which isn't the recommended use case of libdeflate.  However,
  `libdeflate-gunzip` has to do this, and it would run out of memory as it would
  keep trying to allocate a larger output buffer.

* Fixed a build error on Solaris.

* Cleaned up a few things in the compression code.

## Version 1.9

* Made many improvements to the compression algorithms, and rebalanced the
  compression levels:

  * Heuristics were implemented which significantly improve the compression
    ratio on data where short matches aren't useful, such as DNA sequencing
    data.  This applies to all compression levels, but primarily to levels 1-9.

  * Level 1 was made much faster, though it often compresses slightly worse than
    before (but still better than zlib).

  * Levels 8-9 were also made faster, though they often compress slightly worse
    than before (but still better than zlib).  On some data, levels 8-9 are much
    faster and compress much better than before; this change addressed an issue
    where levels 8-9 did poorly on certain files.  The algorithm used by levels
    8-9 is now more similar to that of levels 6-7 than to that of levels 10-12.

  * Levels 2-3, 7, and 10-12 were strengthened slightly.

  * Levels 4-6 were also strengthened slightly, but some of this improvement was
    traded off to speed them up slightly as well.

  * Levels 1-9 had their per-compressor memory usage greatly reduced.

  As always, compression ratios will vary depending on the input data, and
  compression speeds will vary depending on the input data and target platform.

* `make install` will now install a pkg-config file for libdeflate.

* The Makefile now supports the `DISABLE_SHARED` parameter to disable building
  the shared library.

* Improved the Android build support in the Makefile.

## Version 1.8

* Added `-t` (test) option to `libdeflate-gunzip`.

* Unaligned access optimizations are now enabled on WebAssembly builds.

* Fixed a build error when building with the Intel C Compiler (ICC).

* Fixed a build error when building with uClibc.

* libdeflate's CI system has switched from Travis CI to GitHub Actions.

* Made some improvements to test scripts.

## Version 1.7

* Added support for compression level 0, "no compression".

* Added an ARM CRC32 instruction accelerated implementation of CRC32.

* Added support for linking the programs to the shared library version of
  libdeflate rather than to the static library version.

* Made the compression level affect the minimum input size at which compression
  is attempted.

* Fixed undefined behavior in x86 Adler32 implementation.  (No miscompilations
  were observed in practice.)

* Fixed undefined behavior in x86 CPU feature code.  (No miscompilations were
  observed in practice.)

* Fixed installing shared lib symlink on macOS.

* Documented third-party bindings.

* Made a lot of improvements to the testing scripts and the CI configuration
  file.

* Lots of other small improvements and cleanups.

## Version 1.6

* Prevented gcc 10 from miscompiling libdeflate (workaround for
  https://gcc.gnu.org/bugzilla/show_bug.cgi?id=94994).

* Removed workaround for gcc 5 and earlier producing slow code on ARM32.  If
  this affects you, please upgrade your compiler.

* New API function: `libdeflate_zlib_decompress_ex()`.  It provides the actual
  size of the stream that was decompressed, like the gzip and DEFLATE
  equivalents.

* `libdeflate_zlib_decompress()` now accepts trailing bytes after the end of the
  stream, like the gzip and DEFLATE equivalents.

* Added support for custom memory allocators.  (New API function:
  `libdeflate_set_memory_allocator()`)

* Added support for building the library in freestanding mode.

* Building libdeflate no longer requires `CPPFLAGS=-Icommon`.

## Version 1.5

* Fixed up stdcall support on 32-bit Windows: the functions are now exported
  using both suffixed and non-suffixed names, and fixed `libdeflate.h` to be
  MSVC-compatible again.

## Version 1.4

* The 32-bit Windows build of libdeflate now uses the "stdcall" calling
  convention instead of "cdecl".  If you're calling `libdeflate.dll` directly
  from C or C++, you'll need to recompile your code.  If you're calling it from
  another language, or calling it indirectly using `LoadLibrary()`, you'll need
  to update your code to use the stdcall calling convention.

* The Makefile now supports building libdeflate as a shared
  library (`.dylib`) on macOS.

* Fixed a bug where support for certain optimizations and optional features
  (file access hints and more precise timestamps) was incorrectly omitted when
  libdeflate was compiled with `-Werror`.

* Added `make check` target to the Makefile.

* Added CI configuration files.

## Version 1.3

* `make install` now supports customizing the directories into which binaries,
  headers, and libraries are installed.

* `make install` now installs into `/usr/local` by default.  To change it, use
  e.g. `make install PREFIX=/usr`.

* `make install` now works on more platforms.

* The Makefile now supports overriding the optimization flags.

* The compression functions now correctly handle an output data buffer >= 4 GiB
  in size, and `gzip` and `gunzip` now correctly handle multi-gigabyte files (if
  enough memory is available).

## Version 1.2

* Slight improvements to decompression speed.

* Added an AVX-512BW implementation of Adler-32.

* The Makefile now supports a user-specified installation `PREFIX`.

* Fixed build error with some Visual Studio versions.

## Version 1.1

* Fixed crash in CRC-32 code when the prebuilt libdeflate for 32-bit Windows was
  called by a program built with Visual Studio.

* Improved the worst-case decompression speed of malicious data.

* Fixed build error when compiling for an ARM processor without hardware
  floating point support.

* Improved performance on the PowerPC64 architecture.

* Added soname to `libdeflate.so`, to make packaging easier.

* Added `make install` target to the Makefile.

* The Makefile now supports user-specified `CPPFLAGS`.

* The Windows binary releases now include the import library for
  `libdeflate.dll`.  `libdeflate.lib` is now the import library, and
  `libdeflatestatic.lib` is the static library.

## Version 1.0

* Added support for multi-member gzip files.

* Moved architecture-specific code into subdirectories.  If you aren't using the
  provided Makefile to build libdeflate, you now need to compile `lib/*.c` and
  `lib/*/*.c` instead of just `lib/*.c`.

* Added an ARM PMULL implementation of CRC-32, which speeds up gzip compression
  and decompression on 32-bit and 64-bit ARM processors that have the
  Cryptography Extensions.

* Improved detection of CPU features, resulting in accelerated functions being
  used in more cases.  This includes:

  * Detect CPU features on 32-bit x86, not just 64-bit as was done previously.

  * Detect CPU features on ARM, both 32 and 64-bit.  (Limited to Linux only
    currently.)

## Version 0.8

* Build fixes for certain platforms and compilers.

* libdeflate now produces the same output on all CPU architectures.

* Improved documentation for building libdeflate on Windows.

## Version 0.7

* Fixed a very rare bug that caused data to be compressed incorrectly.  The bug
  affected compression levels 7 and below since libdeflate v0.2.  Although there
  have been no user reports of the bug, and I believe it would have been highly
  unlikely to encounter on realistic data, it could occur on data specially
  crafted to reproduce it.

* Fixed a compilation error when building with clang 3.7.

## Version 0.6

* Various improvements to the gzip program's behavior.

* Faster CRC-32 on AVX-capable processors.

* Other minor changes.

## Version 0.5

* The CRC-32 checksum algorithm has been optimized with carryless multiplication
  instructions for `x86_64` (PCLMUL).  This speeds up gzip compression and
  decompression.

* Build fixes for certain platforms and compilers.

* Added more test programs and scripts.

* libdeflate is now entirely MIT-licensed.

## Version 0.4

* The Adler-32 checksum algorithm has been optimized with vector instructions
  for `x86_64` (SSE2 and AVX2) and ARM (NEON).  This speeds up zlib compression
  and decompression.

* To avoid naming collisions, functions and definitions in libdeflate's API have
  been renamed to be prefixed with `libdeflate_` or `LIBDEFLATE_`.  Programs
  using the old API will need to be updated.

* Various bug fixes and other improvements.

## Version 0.3

* Some bug fixes and other minor changes.

## Version 0.2

* Implemented a new block splitting algorithm which typically improves the
  compression ratio slightly at all compression levels.

* The compressor now outputs each block using the cheapest type (dynamic
  Huffman, static Huffman, or uncompressed).

* The gzip program has received an overhaul and now behaves more like the
  standard version.

* Build system updates, including: some build options were changed and some
  build options were removed, and the default 'make' target now includes the
  gzip program as well as the library.

## Version 0.1

* Initial official release.
