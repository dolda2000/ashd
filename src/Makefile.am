SUBDIRS = dirplex

bin_PROGRAMS =	htparser sendfile callcgi patplex userplex htls \
		callscgi accesslog htextauth callfcgi multifscgi \
		errlogger httimed psendfile httrcall htpipe ratequeue

htparser_SOURCES = htparser.c htparser.h plaintcp.c ssl-gnutls.c

LDADD = $(top_srcdir)/lib/libht.a
AM_CPPFLAGS = -I$(top_srcdir)/lib

htparser_CPPFLAGS = $(AM_CPPFLAGS) @GNUTLS_CPPFLAGS@
htparser_LDADD = $(LDADD) @GNUTLS_LIBS@
sendfile_LDADD = $(LDADD) -lmagic @XATTR_LIBS@
psendfile_LDADD = $(LDADD) -lmagic @XATTR_LIBS@
