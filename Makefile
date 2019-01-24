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

# We don't define any CPPFLAGS, but support the user specifying it.

##############################################################################

PREFIX ?= /usr

SOVERSION          := 0
STATIC_LIB_SUFFIX  := .a
SHARED_LIB_SUFFIX  := .so.$(SOVERSION)
SHARED_LIB_CFLAGS  := -fPIC
SHARED_LIB_LDFLAGS := -Wl,-soname=libdeflate$(SHARED_LIB_SUFFIX)
PROG_SUFFIX        :=
PROG_CFLAGS        :=
HARD_LINKS         := 1

# Compiling for Windows with MinGW?
ifneq ($(findstring -mingw,$(shell $(CC) -dumpmachine 2>/dev/null)),)
    SOVERSION          :=
    STATIC_LIB_SUFFIX  := static.lib
    SHARED_LIB_SUFFIX  := .dll
    SHARED_LIB_CFLAGS  :=
    SHARED_LIB_LDFLAGS := -Wl,--out-implib,libdeflate.lib
    PROG_SUFFIX        := .exe
    PROG_CFLAGS        := -static -municode
    HARD_LINKS         :=
    override CFLAGS    := $(CFLAGS) $(call cc-option,-Wno-pedantic-ms-format)

    # If AR was not already overridden, then derive it from $(CC).
    # Note that CC may take different forms, e.g. "cc", "gcc",
    # "x86_64-w64-mingw32-gcc", or "x86_64-w64-mingw32-gcc-6.3.1".
    # On Windows it may also have a .exe extension.
    ifeq ($(AR),ar)
        AR := $(shell echo $(CC) | \
                sed -E 's/g?cc(-?[0-9]+(\.[0-9]+)*)?(\.exe)?$$/ar\3/')
    endif
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

COMMON_HEADERS := $(wildcard common/*.h) libdeflate.h
DEFAULT_TARGETS :=

#### Library

STATIC_LIB := libdeflate$(STATIC_LIB_SUFFIX)
SHARED_LIB := libdeflate$(SHARED_LIB_SUFFIX)

LIB_CFLAGS += $(CFLAGS) -fvisibility=hidden -D_ANSI_SOURCE

LIB_HEADERS := $(wildcard lib/*.h) $(wildcard lib/*/*.h)

LIB_SRC := lib/aligned_malloc.c lib/deflate_decompress.c \
	   $(wildcard lib/*/cpu_features.c)

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
	$(QUIET_CC) $(CC) -o $@ -c $(CPPFLAGS) $(LIB_CFLAGS) $<

# Compile shared library object files
$(SHARED_LIB_OBJ): %.shlib.o: %.c $(LIB_HEADERS) $(COMMON_HEADERS) .lib-cflags
	$(QUIET_CC) $(CC) -o $@ -c $(CPPFLAGS) $(LIB_CFLAGS) \
		$(SHARED_LIB_CFLAGS) -DLIBDEFLATE_DLL $<

# Create static library
$(STATIC_LIB):$(STATIC_LIB_OBJ)
	$(QUIET_AR) $(AR) cr $@ $+

DEFAULT_TARGETS += $(STATIC_LIB)

# Create shared library
$(SHARED_LIB):$(SHARED_LIB_OBJ)
	$(QUIET_CCLD) $(CC) -o $@ $(LDFLAGS) $(LIB_CFLAGS) \
		$(SHARED_LIB_LDFLAGS) -shared $+

DEFAULT_TARGETS += $(SHARED_LIB)

ifdef SOVERSION
# Create the symlink libdeflate.so => libdeflate.so.$SOVERSION
libdeflate.so:$(SHARED_LIB)
	$(QUIET_LN) ln -sf $+ $@
DEFAULT_TARGETS += libdeflate.so
endif

# Rebuild if CC, LIB_CFLAGS, or CPPFLAGS changed
.lib-cflags: FORCE
	@flags='$(CC):$(LIB_CFLAGS):$(CPPFLAGS)'; \
	if [ "$$flags" != "`cat $@ 2>/dev/null`" ]; then \
		[ -e $@ ] && echo "Rebuilding library due to new compiler flags"; \
		echo "$$flags" > $@; \
	fi

##############################################################################

#### Programs

PROG_CFLAGS += $(CFLAGS)		 \
	       -D_POSIX_C_SOURCE=200809L \
	       -D_FILE_OFFSET_BITS=64	 \
	       -DHAVE_CONFIG_H

ALL_PROG_COMMON_HEADERS := programs/config.h \
			   programs/prog_util.h \
			   programs/test_util.h
PROG_COMMON_SRC      := programs/prog_util.c \
			programs/tgetopt.c
NONTEST_PROG_SRC     := programs/gzip.c
TEST_PROG_COMMON_SRC := programs/test_util.c
TEST_PROG_SRC        := programs/benchmark.c \
			programs/checksum.c \
			programs/test_checksums.c \
			programs/test_incomplete_codes.c \
			programs/test_slow_decompression.c

NONTEST_PROGRAMS := $(NONTEST_PROG_SRC:programs/%.c=%$(PROG_SUFFIX))
DEFAULT_TARGETS  += $(NONTEST_PROGRAMS)
TEST_PROGRAMS    := $(TEST_PROG_SRC:programs/%.c=%$(PROG_SUFFIX))

PROG_COMMON_OBJ      := $(PROG_COMMON_SRC:%.c=%.o)
NONTEST_PROG_OBJ     := $(NONTEST_PROG_SRC:%.c=%.o)
TEST_PROG_COMMON_OBJ := $(TEST_PROG_COMMON_SRC:%.c=%.o)
TEST_PROG_OBJ        := $(TEST_PROG_SRC:%.c=%.o)

ALL_PROG_OBJ	     := $(PROG_COMMON_OBJ) $(NONTEST_PROG_OBJ) \
			$(TEST_PROG_COMMON_OBJ) $(TEST_PROG_OBJ)

# Generate autodetected configuration header
programs/config.h:programs/detect.sh .prog-cflags
	$(QUIET_GEN) CC="$(CC)" CFLAGS="$(PROG_CFLAGS)" $< > $@

# Compile program object files
$(ALL_PROG_OBJ): %.o: %.c $(ALL_PROG_COMMON_HEADERS) $(COMMON_HEADERS) \
			.prog-cflags
	$(QUIET_CC) $(CC) -o $@ -c $(CPPFLAGS) $(PROG_CFLAGS) $<

# Link the programs.
#
# Note: the test programs are not compiled by default.  One reason is that the
# test programs must be linked with zlib for doing comparisons.

$(NONTEST_PROGRAMS): %$(PROG_SUFFIX): programs/%.o $(PROG_COMMON_OBJ) \
			$(STATIC_LIB)
	$(QUIET_CCLD) $(CC) -o $@ $(LDFLAGS) $(PROG_CFLAGS) $+

$(TEST_PROGRAMS): %$(PROG_SUFFIX): programs/%.o $(PROG_COMMON_OBJ) \
			$(TEST_PROG_COMMON_OBJ) $(STATIC_LIB)
	$(QUIET_CCLD) $(CC) -o $@ $(LDFLAGS) $(PROG_CFLAGS) $+ -lz

ifdef HARD_LINKS
# Hard link gunzip to gzip
gunzip$(PROG_SUFFIX):gzip$(PROG_SUFFIX)
	$(QUIET_LN) ln -f $< $@
else
# No hard links; copy gzip to gunzip
gunzip$(PROG_SUFFIX):gzip$(PROG_SUFFIX)
	$(QUIET_CP) cp -f $< $@
endif

DEFAULT_TARGETS += gunzip$(PROG_SUFFIX)

# Rebuild if CC, PROG_CFLAGS, or CPPFLAGS changed
.prog-cflags: FORCE
	@flags='$(CC):$(PROG_CFLAGS):$(CPPFLAGS)'; \
	if [ "$$flags" != "`cat $@ 2>/dev/null`" ]; then \
		[ -e $@ ] && echo "Rebuilding programs due to new compiler flags"; \
		echo "$$flags" > $@; \
	fi

##############################################################################

all:$(DEFAULT_TARGETS)

install:all
	install -Dm644 -t $(DESTDIR)$(PREFIX)/lib $(STATIC_LIB)
	install -Dm755 -t $(DESTDIR)$(PREFIX)/lib $(SHARED_LIB)
	ln -sf $(SHARED_LIB) $(DESTDIR)$(PREFIX)/lib/libdeflate.so
	install -Dm644 -t $(DESTDIR)$(PREFIX)/include libdeflate.h
	install -Dm755 gzip $(DESTDIR)$(PREFIX)/bin/libdeflate-gzip
	ln -f $(DESTDIR)$(PREFIX)/bin/libdeflate-gzip $(DESTDIR)$(PREFIX)/bin/libdeflate-gunzip

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/$(STATIC_LIB) \
		$(DESTDIR)$(PREFIX)/lib/$(SHARED_LIB) \
		$(DESTDIR)$(PREFIX)/lib/libdeflate.so \
		$(DESTDIR)$(PREFIX)/include/libdeflate.h \
		$(DESTDIR)$(PREFIX)/bin/libdeflate-gzip \
		$(DESTDIR)$(PREFIX)/bin/libdeflate-gunzip

test_programs:$(TEST_PROGRAMS)

help:
	@echo "Available targets:"
	@echo "------------------"
	@for target in $(DEFAULT_TARGETS) $(TEST_PROGRAMS); do \
		echo -e "$$target";		\
	done

clean:
	rm -f *.a *.dll *.exe *.exp *.so \
		lib/*.o lib/*/*.o \
		lib/*.obj lib/*/*.obj \
		lib/*.dllobj lib/*/*.dllobj \
		programs/*.o programs/*.obj \
		$(DEFAULT_TARGETS) $(TEST_PROGRAMS) programs/config.h \
		libdeflate.lib libdeflatestatic.lib \
		.lib-cflags .prog-cflags

realclean: clean
	rm -f tags cscope* run_tests.log

FORCE:

.PHONY: all install uninstall test_programs help clean realclean

.DEFAULT_GOAL = all
