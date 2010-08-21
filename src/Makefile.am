bin_PROGRAMS = htparser dirplex sendfile callcgi patplex
noinst_PROGRAMS = debugsink

htparser_SOURCES = htparser.c htparser.h plaintcp.c
dirplex_SOURCES = dirplex.c
debugsink_SOURCES = debugsink.c

LDADD = $(top_srcdir)/lib/libht.a
AM_CPPFLAGS = -I$(top_srcdir)/lib

sendfile_LDADD = $(LDADD) -lmagic @XATTR_LIBS@
