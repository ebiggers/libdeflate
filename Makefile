##############################################################################

# Build the static library libdeflate.a?
BUILD_STATIC_LIBRARY := yes

# Build the shared library libdeflate.so?
BUILD_SHARED_LIBRARY := no

# Build the benchmark program?  Note that to allow comparisons, the benchmark
# program will be linked with both zlib and libdeflate.
BUILD_BENCHMARK_PROGRAM := no

# Build the gzip program?  Note that this program is not intended to be a full
# gzip replacement.  Like the benchmark program, it is primary intended for
# benchmarking and testing.
BUILD_GZIP_PROGRAM := no

# Build all the programs?
BUILD_PROGRAMS := no

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

# Will the decompressor detect CPU features at runtime in order to run more
# optimized code?  This only affects some platforms and architectures.
RUNTIME_CPU_DETECTION := yes

# The compiler and archiver
CC := gcc
AR := ar

##############################################################################

# Detect compiling for Windows with MinGW
ifneq ($(findstring -mingw,$(CC)),)
    ifeq ($(AR),ar)
        AR := $(patsubst %-gcc,%-ar,$(CC))
    endif
    WINDOWS      := yes
    LIB_SUFFIX   := .a
    SHLIB_SUFFIX := .dll
    PROG_SUFFIX  := .exe
    PROG_CFLAGS  := -static
    GZIP_CFLAGS  := -municode
    SHLIB_IS_PIC := no
else
    WINDOWS      := no
    LIB_SUFFIX   := .a
    SHLIB_SUFFIX := .so
    PROG_SUFFIX  :=
    PROG_CFLAGS  :=
    GZIP_CFLAGS  :=
    SHLIB_IS_PIC := yes
endif

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

ifneq ($(SUPPORT_NEAR_OPTIMAL_PARSING),yes)
  override CFLAGS += -DSUPPORT_NEAR_OPTIMAL_PARSING=0
endif

ifeq ($(UNSAFE_DECOMPRESSION),yes)
  override CFLAGS += -DUNSAFE_DECOMPRESSION=1
endif

ifneq ($(RUNTIME_CPU_DETECTION),yes)
  override CFLAGS += -DRUNTIME_CPU_DETECTION=0
endif

SRC := src/aligned_malloc.c
ifeq ($(SUPPORT_COMPRESSION),yes)
    SRC += src/deflate_compress.c
endif
ifeq ($(SUPPORT_DECOMPRESSION),yes)
    SRC += src/deflate_decompress.c
    ifeq ($(RUNTIME_CPU_DETECTION),yes)
        SRC += src/x86_cpu_features.c
    endif
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

OBJ := $(SRC:.c=.o)
PIC_OBJ := $(SRC:.c=.pic.o)
ifeq ($(SHLIB_IS_PIC),yes)
    SHLIB_OBJ := $(PIC_OBJ)
else
    SHLIB_OBJ := $(OBJ)
endif

$(OBJ): %.o: %.c $(wildcard src/*.h)
	$(CC) -o $@ -c $(CFLAGS) $<

$(PIC_OBJ): %.pic.o: %.c $(wildcard src/*.h)
	$(CC) -o $@ -c $(CFLAGS) -fPIC $<

libdeflate$(SHLIB_SUFFIX):$(SHLIB_OBJ)
	$(CC) -o $@ -shared $(CFLAGS) $+

libdeflate$(LIB_SUFFIX):$(OBJ)
	$(AR) cr $@ $+

benchmark$(PROG_SUFFIX):tools/benchmark.c libdeflate$(LIB_SUFFIX)
	$(CC) -o $@ $(CFLAGS) -L. $+ libdeflate$(LIB_SUFFIX) -lz

gzip$(PROG_SUFFIX):tools/gzip.c libdeflate$(LIB_SUFFIX)
	$(CC) -o $@ $(CFLAGS) $(GZIP_CFLAGS) $(PROG_CFLAGS) -L. $+ libdeflate$(LIB_SUFFIX)

ifeq ($(WINDOWS),yes)
gunzip$(PROG_SUFFIX):tools/gzip.c libdeflate$(LIB_SUFFIX)
	$(CC) -o $@ $(CFLAGS) $(GZIP_CFLAGS) $(PROG_CFLAGS) -L. $+ libdeflate$(LIB_SUFFIX)
else
gunzip$(PROG_SUFFIX):gzip$(PROG_SUFFIX)
	ln -f gzip$(PROG_SUFFIX) $@
endif

ifeq ($(BUILD_PROGRAMS),yes)
    BUILD_BENCHMARK_PROGRAM := yes
    BUILD_GZIP_PROGRAM := yes
endif

TARGETS :=
ifeq ($(BUILD_STATIC_LIBRARY),yes)
    TARGETS += libdeflate$(LIB_SUFFIX)
endif
ifeq ($(BUILD_SHARED_LIBRARY),yes)
    TARGETS += libdeflate$(SHLIB_SUFFIX)
endif
ifeq ($(BUILD_BENCHMARK_PROGRAM),yes)
    TARGETS += benchmark$(PROG_SUFFIX)
endif
ifeq ($(BUILD_GZIP_PROGRAM),yes)
    TARGETS += gzip$(PROG_SUFFIX) gunzip$(PROG_SUFFIX)
endif

all:$(TARGETS)

clean:
	rm -f libdeflate.a libdeflate.so libdeflate.dll	src/*.o		\
		benchmark benchmark.exe					\
		gzip gzip.exe						\
		gunzip gunzip.exe

.PHONY: all clean

.DEFAULT_GOAL = all
