bin_PROGRAMS = htparser dirplex sendfile callcgi patplex userplex htls callscgi
noinst_PROGRAMS = debugsink

htparser_SOURCES = htparser.c htparser.h plaintcp.c ssl-gnutls.c
dirplex_SOURCES = dirplex.c
debugsink_SOURCES = debugsink.c

LDADD = $(top_srcdir)/lib/libht.a
AM_CPPFLAGS = -I$(top_srcdir)/lib

htparser_CPPFLAGS = $(AM_CPPFLAGS) @GNUTLS_CPPFLAGS@
htparser_LDADD = $(LDADD) @GNUTLS_LIBS@
sendfile_LDADD = $(LDADD) -lmagic @XATTR_LIBS@
