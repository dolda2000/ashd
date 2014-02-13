dist_man1_MANS =	callcgi.1 dirplex.1 htparser.1 patplex.1 sendfile.1 \
			userplex.1 htls.1 callscgi.1 accesslog.1 htextauth.1 \
			callfcgi.1 multifscgi.1 errlogger.1 httimed.1 \
			psendfile.1 httrcall.1

dist_man7_MANS = ashd.7

%.7 %.1: %.doc
	a2x -f manpage $<

%.html: %.doc
	a2x -f xhtml $<

manpages: $(dist_man1_MANS) $(dist_man7_MANS)

htmldoc: $(patsubst %.doc, %.html, *.doc)

EXTRA_DIST = *.doc
