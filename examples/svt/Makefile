include config.mk

SRC = svt.c
BIN = svt svt-editor svt-pager
MANUALS = svt.1 svt-editor.1 svt-pager.1

VERSION = $(shell git describe --always --dirty 2>/dev/null || echo "0.15-git")
SVTCFLAGS += ${CFLAGS} -DVERSION=\"${VERSION}\"
DEBUG_CFLAGS = ${SVTFLAGS} -UNDEBUG -O0 -g -ggdb -Wall -Wextra -Wno-unused-parameter

all: svt svt-editor

options:
	@echo svt build options:
	@echo "CFLAGS  = $(SVTCFLAGS)"
	@echo "LDFLAGS = $(SVTDFLAGS)"
	@echo "CC      = $(CC)"

config.h:
	cp config.def.h config.h

svt: config.h config.mk *.c *.h
	${CC} ${SVTCFLAGS} ${SRC} ${SVTLDFLAGS} ${LIBS} -o $@

svt-editor: svt-editor.c
	${CC} ${SVTCFLAGS} $^ ${SVTLDFLAGS} -o $@

man:
	@for m in ${MANUALS}; do \
		echo "Generating $$m"; \
		sed -e "s/VERSION/${VERSION}/" "$$m" | mandoc -W warning -T utf8 -T xhtml -O man=%N.%S.html -O style=mandoc.css 1> "$$m.html" || true; \
	done

debug: clean
	@$(MAKE) CFLAGS='${DEBUG_CFLAGS}'

clean:
	@echo cleaning
	@rm -f svt
	@rm -f svt-editor

dist: clean
	@echo creating dist tarball
	@git archive --prefix=svt-${VERSION}/ -o svt-${VERSION}.tar.gz HEAD

install: all
	@mkdir -p ${DESTDIR}${PREFIX}/bin
	@for b in ${BIN}; do \
		echo "installing ${DESTDIR}${PREFIX}/bin/$$b"; \
		cp -f "$$b" "${DESTDIR}${PREFIX}/bin" && \
		chmod 755 "${DESTDIR}${PREFIX}/bin/$$b"; \
	done
	@echo installing manual page to ${DESTDIR}${MANPREFIX}/man1
	@mkdir -p ${DESTDIR}${MANPREFIX}/man1
	@for m in ${MANUALS}; do \
		sed -e "s/VERSION/${VERSION}/" < "$$m" >  "${DESTDIR}${MANPREFIX}/man1/$$m" && \
		chmod 644 "${DESTDIR}${MANPREFIX}/man1/$$m"; \
	done

uninstall:
	@for b in ${BIN}; do \
		echo "removing ${DESTDIR}${PREFIX}/bin/$$b"; \
		rm -f "${DESTDIR}${PREFIX}/bin/$$b"; \
	done
	@echo removing manual page from ${DESTDIR}${MANPREFIX}/man1
	@rm -f ${DESTDIR}${MANPREFIX}/man1/svt.1

.PHONY: all clean dist install uninstall debug
