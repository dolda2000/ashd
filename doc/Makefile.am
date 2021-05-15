dist_man1_MANS =	callcgi.1 dirplex.1 htparser.1 patplex.1 sendfile.1 \
			userplex.1 htls.1 callscgi.1 accesslog.1 htextauth.1 \
			callfcgi.1 multifscgi.1 errlogger.1 httimed.1 \
			psendfile.1 httrcall.1 htpipe.1

dist_man7_MANS = ashd.7

.doc.1:
	a2x -f manpage $<
.doc.7:
	a2x -f manpage $<
.doc.html:
	a2x -f xhtml $<

manpages: $(dist_man1_MANS) $(dist_man7_MANS)

htmldoc: ${dist_man1_MANS:.1=.html} ${dist_man7_MANS:.7=.html}

EXTRA_DIST = *.doc
