##############################################################################

# Build the static library libdeflate.a?
BUILD_STATIC_LIBRARY := yes

# Build the shared library libdeflate.so?
BUILD_SHARED_LIBRARY := no

# Build the benchmark program?  Note that to allow comparisons, the benchmark
# program will be linked with both zlib and libdeflate.
BUILD_BENCHMARK_PROGRAM := no

# Will compression be supported?
SUPPORT_COMPRESSION := yes

# Will decompression be supported?
SUPPORT_DECOMPRESSION := yes

# Will the zlib wrapper format be supported?
SUPPORT_ZLIB := yes

# Will the gzip wrapper format be supported?
SUPPORT_GZIP := yes

# Will near optimal parsing (high compression) be supported?
SUPPORT_NEAR_OPTIMAL_PARSING := yes

# Will the decompressor assume that all compressed data is valid?
# This is faster but ***insecure***!  Default to secure.
UNSAFE_DECOMPRESSION := no

# The compiler and archiver
CC := gcc
AR := ar

##############################################################################

# Always compile with optimizations enabled!
# But don't change it to -O3 and expect it to be better.
override CFLAGS += -O2

# Use a sane default symbol visibility.
override CFLAGS += -fvisibility=hidden

# Set the C version.  We currently require at least C99.  Also, we actually need
# -std=gnu99 instead of -std=c99 for unnamed structs and unions, which are in
# C11 but not C99.  But we can't yet use -std=c11 because we want to support
# older versions of gcc.
override CFLAGS += -std=gnu99

# Allow including libdeflate.h.
override CFLAGS += -I.

# Hide non-standard functions from standard headers (e.g. heapsort() on *BSD).
override CFLAGS += -D_ANSI_SOURCE

ifeq ($(SUPPORT_NEAR_OPTIMAL_PARSING),yes)
  override CFLAGS += -DSUPPORT_NEAR_OPTIMAL_PARSING=1
endif

ifeq ($(UNSAFE_DECOMPRESSION),yes)
  override CFLAGS += -DUNSAFE_DECOMPRESSION=1
endif

SRC := src/aligned_malloc.c
ifeq ($(SUPPORT_COMPRESSION),yes)
    SRC += src/deflate_compress.c
endif
ifeq ($(SUPPORT_DECOMPRESSION),yes)
    SRC += src/deflate_decompress.c
endif
ifeq ($(SUPPORT_ZLIB),yes)
    ifeq ($(SUPPORT_COMPRESSION),yes)
        SRC += src/zlib_compress.c
    endif
    ifeq ($(SUPPORT_DECOMPRESSION),yes)
        SRC += src/zlib_decompress.c
    endif
    SRC += src/adler32.c
endif
ifeq ($(SUPPORT_GZIP),yes)
    ifeq ($(SUPPORT_COMPRESSION),yes)
        SRC += src/gzip_compress.c
    endif
    ifeq ($(SUPPORT_DECOMPRESSION),yes)
        SRC += src/gzip_decompress.c
    endif
    SRC += src/crc32.c
endif

override PIC_CFLAGS := $(CFLAGS) -fPIC

OBJ := $(SRC:.c=.o)
PIC_OBJ := $(SRC:.c=.pic.o)

$(OBJ): %.o: %.c $(wildcard src/*.h)
	$(CC) -o $@ -c $(CFLAGS) $<

$(PIC_OBJ): %.pic.o: %.c $(wildcard src/*.h)
	$(CC) -o $@ -c $(PIC_CFLAGS) $<

libdeflate.so:$(PIC_OBJ)
	$(CC) -o $@ -shared $(CFLAGS) $+

libdeflate.a:$(OBJ)
	$(AR) cr $@ $+

benchmark:tools/benchmark.c libdeflate.a
	$(CC) -o $@ $(CFLAGS) -L. $+ libdeflate.a -lz

TARGETS :=
ifeq ($(BUILD_STATIC_LIBRARY),yes)
    TARGETS += libdeflate.a
endif
ifeq ($(BUILD_SHARED_LIBRARY),yes)
    TARGETS += libdeflate.so
endif
ifeq ($(BUILD_BENCHMARK_PROGRAM),yes)
    TARGETS += benchmark
endif

all:$(TARGETS)

clean:
	rm -f benchmark libdeflate.a libdeflate.so src/*.o

.PHONY: all clean

.DEFAULT_GOAL = all
