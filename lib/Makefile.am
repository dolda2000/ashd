lib_LIBRARIES = libht.a

libht_a_SOURCES =	utils.c mt.c log.c req.c proc.c mtio.c resp.c cf.c
libht_a_CFLAGS	=	-fPIC
if USE_EPOLL
libht_a_SOURCES += mtio-epoll.c
else
libht_a_SOURCES += mtio-select.c
endif

pkginclude_HEADERS =	utils.h mt.h log.h req.h proc.h mtio.h resp.h cf.h
