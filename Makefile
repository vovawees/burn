# burn - the CleanLinux package manager
# SPDX-License-Identifier: MIT
#
#   make                 build ./burn
#   make check           unit + command line tests
#   make proto           compare with the bash prototype: PROTO= RECIPES= PROFILES=
#   make check-btrfs     snapshots on a real btrfs (loop device; KERNEL=bzImage: in qemu)
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
PROTO ?= ../CleanLinux/burn/burn
RECIPES_DIR = $(or $(RECIPES),../CleanLinux-Recipes/packages)
PROFILES ?= ../CleanLinux/profiles
proto: burn
	sh tests/proto.sh ./burn $(PROTO) $(RECIPES_DIR) $(PROFILES)

# snapshots on a real btrfs: a loop device (root or sudo), or in qemu with KERNEL=bzImage
check-btrfs: burn
	if [ -n "$(KERNEL)" ]; then sh tests/qemu-btrfs.sh $(KERNEL) ./burn; else sh tests/btrfs.sh ./burn; fi

install: burn
	install -Dm755 burn $(DESTDIR)$(BINDIR)/burn

clean:
	rm -f burn tests/unit tests/shdump tests/tarls tests/tarw tests/elfscan src/*.o src/prelude.inc

.PHONY: all check check-btrfs proto install clean
