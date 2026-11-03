# burn - the CleanLinux package manager
# SPDX-License-Identifier: MIT
#
#   make                 build ./burn
#   make check           unit + command line tests
#   make install         DESTDIR= PREFIX=/usr
#   make WERROR=1        warnings are errors (CI)

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin

CC ?= cc
CFLAGS ?= -O2 -pipe
WARN := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 \
	-Wno-unused-parameter -Wpointer-arith -Wvla -Wundef
ifeq ($(WERROR),1)
WARN += -Werror
endif
BURN_CFLAGS := -std=gnu17 -D_GNU_SOURCE $(WARN)
LDLIBS += -lzstd

SRC := $(sort $(wildcard src/*.c))
OBJ := $(SRC:.c=.o)
LIBOBJ := $(filter-out src/main.o,$(OBJ))
HDR := $(wildcard src/*.h)

all: burn

burn: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(HDR)
	$(CC) $(CPPFLAGS) $(BURN_CFLAGS) $(CFLAGS) -c -o $@ $<

tests/%: tests/%.c $(LIBOBJ) $(HDR)
	$(CC) $(CPPFLAGS) $(BURN_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBOBJ) $(LDLIBS)

# RECIPES=../CleanLinux-Recipes/packages also compares every recipe with bash
check: burn tests/unit tests/shdump
	./tests/unit
	sh tests/shcompat.sh tests/shdump $(RECIPES)
	sh tests/cli.sh ./burn

install: burn
	install -Dm755 burn $(DESTDIR)$(BINDIR)/burn

clean:
	rm -f burn tests/unit tests/shdump src/*.o

.PHONY: all check install clean
