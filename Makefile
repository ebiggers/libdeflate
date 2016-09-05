#
# Use 'make help' to list available targets.
#
# Define V=1 to enable "verbose" mode, showing all executed commands.
#
# Define DECOMPRESSION_ONLY to omit all compression code, building a
# decompression-only library.  If doing this, you must also build a specific
# library target such as 'libdeflate.a', as the programs will no longer compile.
#
# Define DISABLE_GZIP to disable support for the gzip wrapper format.
#
# Define DISABLE_ZLIB to disable support for the zlib wrapper format.
#
##############################################################################

#### Common compiler flags.
#### Flags given here are not intended to be overridden, but you can add more
#### by defining CFLAGS in the environment or on the 'make' command line.

cc-option = $(shell if $(CC) $(1) -c -x c /dev/null -o /dev/null \
	      1>&2 2>/dev/null; then echo $(1); fi)

override CFLAGS :=							\
	$(CFLAGS) -O2 -fomit-frame-pointer -std=c99 -I. -Icommon	\
	-Wall -Wundef							\
	$(call cc-option,-Wpedantic)					\
	$(call cc-option,-Wdeclaration-after-statement)			\
	$(call cc-option,-Wmissing-prototypes)				\
	$(call cc-option,-Wstrict-prototypes)				\
	$(call cc-option,-Wvla)

##############################################################################

STATIC_LIB_SUFFIX := .a
SHARED_LIB_SUFFIX := .so
SHARED_LIB_CFLAGS := -fPIC
PROG_SUFFIX       :=
PROG_CFLAGS       :=
HARD_LINKS        := 1

# Compiling for Windows with MinGW?
ifneq ($(findstring -mingw,$(shell $(CC) -dumpmachine 2>/dev/null)),)
    ifeq ($(AR),ar)
        ifneq ($(findstring -mingw,$(CC)),)
            AR := $(patsubst %-gcc,%-ar,$(CC))
        endif
    endif
    STATIC_LIB_SUFFIX := .lib
    SHARED_LIB_SUFFIX := .dll
    SHARED_LIB_CFLAGS :=
    PROG_SUFFIX       := .exe
    PROG_CFLAGS       := -static -municode
    HARD_LINKS        :=
    override CFLAGS   := $(CFLAGS) $(call cc-option,-Wno-pedantic-ms-format)
endif

##############################################################################

#### Quiet make is enabled by default.  Define V=1 to disable.

ifneq ($(findstring s,$(MAKEFLAGS)),s)
ifneq ($(V),1)
        QUIET_CC       = @echo '  CC      ' $@;
        QUIET_CCLD     = @echo '  CCLD    ' $@;
        QUIET_AR       = @echo '  AR      ' $@;
        QUIET_LN       = @echo '  LN      ' $@;
        QUIET_CP       = @echo '  CP      ' $@;
        QUIET_GEN      = @echo '  GEN     ' $@;
endif
endif

##############################################################################

COMMON_HEADERS := $(wildcard common/*.h)
ALL_TARGETS :=

#### Library

STATIC_LIB := libdeflate$(STATIC_LIB_SUFFIX)
SHARED_LIB := libdeflate$(SHARED_LIB_SUFFIX)

LIB_CFLAGS += $(CFLAGS) -fvisibility=hidden -D_ANSI_SOURCE

LIB_HEADERS := $(wildcard lib/*.h)

LIB_SRC := lib/aligned_malloc.c lib/deflate_decompress.c lib/x86_cpu_features.c

DECOMPRESSION_ONLY :=
ifndef DECOMPRESSION_ONLY
    LIB_SRC += lib/deflate_compress.c
endif

DISABLE_ZLIB :=
ifndef DISABLE_ZLIB
    LIB_SRC += lib/adler32.c lib/zlib_decompress.c
    ifndef DECOMPRESSION_ONLY
        LIB_SRC += lib/zlib_compress.c
    endif
endif

DISABLE_GZIP :=
ifndef DISABLE_GZIP
    LIB_SRC += lib/crc32.c lib/gzip_decompress.c
    ifndef DECOMPRESSION_ONLY
        LIB_SRC += lib/gzip_compress.c
    endif
endif

STATIC_LIB_OBJ := $(LIB_SRC:.c=.o)
SHARED_LIB_OBJ := $(LIB_SRC:.c=.shlib.o)

# Compile static library object files
$(STATIC_LIB_OBJ): %.o: %.c $(LIB_HEADERS) $(COMMON_HEADERS) .lib-cflags
	$(QUIET_CC) $(CC) -o $@ -c $(LIB_CFLAGS) $<

# Compile shared library object files
$(SHARED_LIB_OBJ): %.shlib.o: %.c $(LIB_HEADERS) $(COMMON_HEADERS) .lib-cflags
	$(QUIET_CC) $(CC) -o $@ -c $(LIB_CFLAGS) $(SHARED_LIB_CFLAGS) -DLIBDEFLATE_DLL $<

# Create static library
$(STATIC_LIB):$(STATIC_LIB_OBJ)
	$(QUIET_AR) $(AR) cr $@ $+

ALL_TARGETS += $(STATIC_LIB)

# Create shared library
$(SHARED_LIB):$(SHARED_LIB_OBJ)
	$(QUIET_CCLD) $(CC) -o $@ $(LDFLAGS) $(LIB_CFLAGS) -shared $+

ALL_TARGETS += $(SHARED_LIB)

# Rebuild if CC or LIB_CFLAGS changed
.lib-cflags: FORCE
	@flags='$(CC):$(LIB_CFLAGS)'; \
	if [ "$$flags" != "`cat $@ 2>/dev/null`" ]; then \
		[ -e $@ ] && echo "Rebuilding library due to new compiler flags"; \
		echo "$$flags" > $@; \
	fi

##############################################################################

#### Programs

PROG_CFLAGS += $(CFLAGS)		\
	       -D_DEFAULT_SOURCE	\
	       -D_FILE_OFFSET_BITS=64	\
	       -DHAVE_CONFIG_H

PROG_COMMON_HEADERS := programs/prog_util.h programs/config.h
PROG_COMMON_SRC := programs/prog_util.c programs/tgetopt.c
PROG_SPECIFIC_SRC := programs/gzip.c programs/benchmark.c

PROG_COMMON_OBJ := $(PROG_COMMON_SRC:.c=.o)
PROG_SPECIFIC_OBJ := $(PROG_SPECIFIC_SRC:.c=.o)
PROG_OBJ := $(PROG_COMMON_OBJ) $(PROG_SPECIFIC_OBJ)

# Generate autodetected configuration header
programs/config.h:programs/detect.sh .prog-cflags
	$(QUIET_GEN) CC="$(CC)" CFLAGS="$(CFLAGS)" $< > $@

# Compile program object files
$(PROG_OBJ): %.o: %.c $(PROG_COMMON_HEADERS) $(COMMON_HEADERS) .prog-cflags
	$(QUIET_CC) $(CC) -o $@ -c $(PROG_CFLAGS) $<

# Link benchmark program
benchmark$(PROG_SUFFIX):programs/benchmark.o $(PROG_COMMON_OBJ) $(STATIC_LIB)
	$(QUIET_CCLD) $(CC) -o $@ $(LDFLAGS) $(PROG_CFLAGS) $+ -lz

# 'benchmark' is not compiled by default because of the zlib link requirement.
#ALL_TARGETS += benchmark$(PROG_SUFFIX)

# Link gzip program
gzip$(PROG_SUFFIX):programs/gzip.o $(PROG_COMMON_OBJ) $(STATIC_LIB)
	$(QUIET_CCLD) $(CC) -o $@ $(LDFLAGS) $(PROG_CFLAGS) $+

ALL_TARGETS += gzip$(PROG_SUFFIX)

ifdef HARD_LINKS
# Hard link gunzip to gzip
gunzip$(PROG_SUFFIX):gzip$(PROG_SUFFIX)
	$(QUIET_LN) ln -f $< $@
else
# No hard links; copy gzip to gunzip
gunzip$(PROG_SUFFIX):gzip$(PROG_SUFFIX)
	$(QUIET_CP) cp -f $< $@
endif

ALL_TARGETS += gunzip$(PROG_SUFFIX)

# Rebuild if CC or PROG_CFLAGS changed
.prog-cflags: FORCE
	@flags='$(CC):$(PROG_CFLAGS)'; \
	if [ "$$flags" != "`cat $@ 2>/dev/null`" ]; then \
		[ -e $@ ] && echo "Rebuilding programs due to new compiler flags"; \
		echo "$$flags" > $@; \
	fi

##############################################################################

all:$(ALL_TARGETS)

help:
	@echo "Available targets:"
	@echo "------------------"
	@for target in $(ALL_TARGETS) benchmark$(PROG_SUFFIX); do \
		echo -e "$$target";		\
	done

clean:
	rm -f *.a *.dll *.exe *.exp *.so \
		lib/*.o lib/*.obj lib/*.dllobj \
		programs/*.o programs/*.obj \
		benchmark gzip gunzip programs/config.h \
		libdeflate.lib libdeflatestatic.lib \
		.lib-cflags .prog-cflags

realclean: clean
	rm -f tags cscope*

FORCE:

.PHONY: all help clean realclean

.DEFAULT_GOAL = all
