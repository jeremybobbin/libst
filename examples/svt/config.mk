# Customize below to fit your system

PREFIX ?= /usr/local
MANPREFIX = ${PREFIX}/share/man
# specify your systems terminfo directory
# leave empty to install into your home folder
TERMINFO := ${DESTDIR}${PREFIX}/share/terminfo

INCS = -I.
LIBS = -lc -lst -lutil -lncursesw
SVTCPPFLAGS = -D_POSIX_C_SOURCE=200809L -D_XOPEN_SOURCE=500 -D_XOPEN_SOURCE_EXTENDED
SVTCFLAGS = -std=c99 ${INCS} -DNDEBUG ${SVTCPPFLAGS}
SVTLDFLAGS = ${LIBS} ${LDFLAGS}

CC ?= cc
