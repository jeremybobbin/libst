# libst - simple terminal
# See LICENSE file for copyright and license details.
.POSIX:

include config.mk

SRC = libst.c
OBJ = $(SRC:.c=.o)

all: options libst.a

options:
	@echo libst build options:
	@echo "CFLAGS  = $(LIBSTCFLAGS)"
	@echo "LDFLAGS = $(LIBSTLDFLAGS)"
	@echo "CC      = $(CC)"

config.h:
	cp config.def.h config.h

.c.o:
	$(CC) $(LIBSTCFLAGS) -c $<

.o.a:
	ar rcs $@ $^

$(OBJ): config.h config.mk

libst.a: $(OBJ)

clean:
	rm -f libst.a $(OBJ) libst-$(VERSION).tar.gz

dist: clean
	mkdir -p libst-$(VERSION)
	cp -R LICENSE Makefile README config.mk\
		libst.info libst.1 libst.h $(SRC)\
		libst-$(VERSION)
	tar -cf - libst-$(VERSION) | gzip > libst-$(VERSION).tar.gz
	rm -rf libst-$(VERSION)

install: libst.a
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	cp -f libst.a $(DESTDIR)$(PREFIX)/lib
	cp -f libst.h $(DESTDIR)$(PREFIX)/include
	mkdir -p $(DESTDIR)$(MANPREFIX)/man1
	sed "s/VERSION/$(VERSION)/g" < libst.1 > $(DESTDIR)$(MANPREFIX)/man1/libst.1
	chmod 644 $(DESTDIR)$(MANPREFIX)/man1/libst.1
	tic -sx libst.info
	@echo Please see the README file regarding the terminfo entry of libst.

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/lib/libst
	rm -f $(DESTDIR)$(MANPREFIX)/man1/libst.1

.PHONY: all options clean dist install uninstall
