PREFIX = /usr/local
GREETING = plain

all: btmake libbtm.so.1 libbtlto.a

libbtm.so.1: lib.c btm.h
	$(CC) $(CFLAGS) -fPIC -shared -Wl,-soname,libbtm.so.1 $(LDFLAGS) -o $@ lib.c

btmake: main.c btm.h libbtm.so.1
	$(CC) $(CFLAGS) -DGREETING='"$(GREETING)"' $(LDFLAGS) -o $@ main.c libbtm.so.1

libbtlto.a: lto.c
	$(CC) $(CFLAGS) -frandom-seed=lto -c -o lto.o lto.c
	ar rcsD $@ lto.o

check: btmake
	LD_LIBRARY_PATH=. ./btmake

install: all
	install -Dm755 btmake $(DESTDIR)$(PREFIX)/bin/btmake
	install -Dm755 libbtm.so.1 $(DESTDIR)$(PREFIX)/lib/libbtm.so.1
	ln -sf libbtm.so.1 $(DESTDIR)$(PREFIX)/lib/libbtm.so
	install -Dm644 libbtlto.a $(DESTDIR)$(PREFIX)/lib/libbtlto.a
	install -Dm644 lto.o $(DESTDIR)$(PREFIX)/lib/btmake/lto.o
	install -Dm644 btm.h $(DESTDIR)$(PREFIX)/include/btm.h
	$(if $(INSTALL_LIBTOOL),install -Dm644 btm.h $(DESTDIR)$(PREFIX)/lib/libbtm.la)

.PHONY: all check install
