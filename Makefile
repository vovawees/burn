# burn - the CleanLinux package manager
# SPDX-License-Identifier: MIT
#
#   make                 build ./burn
#   make check           unit + command line tests
#   make proto           compare with the bash prototype: PROTO= RECIPES= PROFILES=
#   make check-btrfs     snapshots on a real btrfs (loop device; KERNEL=bzImage: in qemu)
#   make install         DESTDIR= PREFIX=/usr
#   make dist            burn-VERSION.tar.gz from git HEAD
#   make WERROR=1        warnings are errors (tools/check.sh)
#   make CURL=1          downloads through libcurl inside burn (HTTP/2, multiplexed), not curl/wget

PREFIX ?= /usr
BINDIR ?= $(PREFIX)/bin
DATADIR ?= $(PREFIX)/share
VERSION := $(shell sed -n 's/^\#define BURN_VERSION "\(.*\)"/\1/p' src/util.h)

CC ?= cc
CFLAGS ?= -O2 -pipe
WARN := -Wall -Wextra -Wshadow -Wstrict-prototypes -Wmissing-prototypes -Wformat=2 \
	-Wno-unused-parameter -Wpointer-arith -Wvla -Wundef
ifeq ($(WERROR),1)
WARN += -Werror
endif
BURN_CFLAGS := -std=gnu17 -D_GNU_SOURCE $(WARN)
LDLIBS += -lzstd
ifeq ($(CURL),1)
BURN_CFLAGS += -DBURN_CURL $(shell pkg-config --cflags libcurl 2>/dev/null)
LDLIBS += $(shell pkg-config --libs libcurl 2>/dev/null || echo -lcurl)
endif

SRC := $(sort $(wildcard src/*.c))
OBJ := $(SRC:.c=.o)
LIBOBJ := $(filter-out src/main.o,$(OBJ))
HDR := $(wildcard src/*.h)

all: burn

burn: $(OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(OBJ) $(LDLIBS)

src/%.o: src/%.c $(HDR)
	$(CC) $(CPPFLAGS) $(BURN_CFLAGS) $(CFLAGS) -c -o $@ $<

# the shell helpers of recipe functions, compiled in as a C string
src/prelude.inc: src/prelude.sh
	sed -e 's/\\/\\\\/g' -e 's/"/\\"/g' -e 's/^/"/' -e 's/$$/\\n"/' $< >$@
src/shrun.o: src/prelude.inc

tests/%: tests/%.c $(LIBOBJ) $(HDR)
	$(CC) $(CPPFLAGS) $(BURN_CFLAGS) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBOBJ) $(LDLIBS)

# RECIPES=../CleanLinux-Recipes/packages also compares every recipe with bash
check: burn tests/unit tests/shdump tests/tarls tests/tarw tests/elfscan
	./tests/unit
	sh tests/tar.sh tests/tarls tests/tarw
	sh tests/elf.sh tests/elfscan
	sh tests/shcompat.sh tests/shdump $(RECIPES)
	sh tests/cli.sh ./burn
	if command -v shellcheck >/dev/null; then shellcheck src/prelude.sh; fi

# the C version against the bash prototype, command by command, on the same staging root
PROTO ?= ../CleanLinux-prototype/burn/burn
RECIPES_DIR = $(or $(RECIPES),../CleanLinux-Recipes/packages)
PROFILES ?= ../CleanLinux-prototype/profiles
proto: burn
	sh tests/proto.sh ./burn $(PROTO) $(RECIPES_DIR) $(PROFILES)

# snapshots on a real btrfs: a loop device (root or sudo), or in qemu with KERNEL=bzImage
check-btrfs: burn
	if [ -n "$(KERNEL)" ]; then sh tests/qemu-btrfs.sh $(KERNEL) ./burn; else sh tests/btrfs.sh ./burn; fi

install: burn
	install -Dm755 burn $(DESTDIR)$(BINDIR)/burn
	install -Dm644 burn.conf $(DESTDIR)$(DATADIR)/burn/burn.conf.example

# the release tarball: the same bytes for the same commit
dist:
	git archive --format=tar --prefix=burn-$(VERSION)/ HEAD | gzip -n -9 >burn-$(VERSION).tar.gz

clean:
	rm -f burn tests/unit tests/shdump tests/tarls tests/tarw tests/elfscan src/*.o src/prelude.inc

.PHONY: all check check-btrfs proto install dist clean
