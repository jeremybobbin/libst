# libst version
VERSION = 0.1.0

# Customize below to fit your system

# paths
PREFIX = /usr/local
MANPREFIX = $(PREFIX)/share/man

PKG_CONFIG = pkg-config

# includes and libs
LIBS = -lm -lutil

# flags
LIBSTCPPFLAGS = -DVERSION=\"$(VERSION)\" -D_XOPEN_SOURCE=600
LIBSTCFLAGS = $(INCS) $(LIBSTCPPFLAGS) $(CPPFLAGS) $(CFLAGS)
LIBSTLDFLAGS = $(LIBS) $(LDFLAGS)

# OpenBSD:
#CPPFLAGS = -DVERSION=\"$(VERSION)\" -D_XOPEN_SOURCE=600 -D_BSD_SOURCE
#LIBS = -lm -lutil

# compiler and linker
# CC = c99
