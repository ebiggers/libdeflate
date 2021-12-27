SRC := $(wildcard */*.c)
EXE := $(SRC:.c=)

LDLIBS := -ldeflate
LDFLAGS := -L../..
CPPFLAGS := -I../..

all:$(EXE)

clean:
	rm -f $(EXE)
