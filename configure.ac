AC_INIT([ashd], [0.13])
AC_CONFIG_SRCDIR(src/htparser.c)
AM_INIT_AUTOMAKE
AC_CONFIG_HEADERS(config.h)
AC_USE_SYSTEM_EXTENSIONS

AC_PROG_CC
AM_PROG_CC_C_O
AC_PROG_INSTALL
AC_PROG_RANLIB

HAS_MAGIC=yes
AC_CHECK_LIB(magic, magic_open, [:], [HAS_MAGIC=no])
AC_CHECK_HEADER(magic.h, [], [HAS_MAGIC=no])
if test "$HAS_MAGIC" = no; then
	AC_MSG_ERROR([*** cannot find libmagic on this system])
fi

AH_TEMPLATE(HAVE_GLIBC_STDIO, [define to indicate system support for glibc cookie streams])
AH_TEMPLATE(HAVE_BSD_STDIO, [define to indicate system support for BSD-style funopen streams])

HAS_FOPENCOOKIE=yes
AC_CHECK_FUNC(fopencookie, [], [HAS_FOPENCOOKIE=no])
AC_CHECK_MEMBER([cookie_io_functions_t.read], [], [HAS_FOPENCOOKIE=no])

HAS_FUNOPEN=yes
AC_CHECK_FUNC(funopen, [], [HAS_FUNOPEN=no])

if test "$HAS_FOPENCOOKIE" = yes; then
        AC_DEFINE(HAVE_GLIBC_STDIO)
elif test "$HAS_FUNOPEN" = yes; then
        AC_DEFINE(HAVE_BSD_STDIO)
else
        AC_MSG_ERROR([*** libc support for custom stdio streams is required])
fi

AH_TEMPLATE(HAVE_VALGRIND, [define to include debugging support for Valgrind])
AC_CHECK_HEADER(valgrind/memcheck.h, [AC_DEFINE(HAVE_VALGRIND)], [])

AH_TEMPLATE(HAVE_EPOLL, [define to enable epoll support])
AC_ARG_WITH(epoll, AS_HELP_STRING([--with-epoll], [enable epoll(2) support]))
HAS_EPOLL=""
if test "$with_epoll" = no; then HAS_EPOLL=no; fi
if test -z "$HAS_EPOLL"; then
	AC_CHECK_LIB(c, epoll_ctl, [:], [HAS_EPOLL=no])
fi
if test -z "$HAS_EPOLL"; then
	AC_CHECK_HEADER(sys/epoll.h, [], [HAS_EPOLL=no])
fi
if test "$HAS_EPOLL" != no; then HAS_EPOLL=yes; fi
if test "$with_epoll" = yes -a "$HAS_EPOLL" = no; then
	AC_MSG_ERROR([*** cannot find epoll support on this system])
fi
if test "$HAS_EPOLL" = yes; then
	AC_DEFINE(HAVE_EPOLL)
fi

AH_TEMPLATE(HAVE_KQUEUE, [define to enable kqueue support])
AC_ARG_WITH(kqueue, AS_HELP_STRING([--with-kqueue], [enable kqueue(2) support]))
HAS_KQUEUE=""
if test "$with_kqueue" = no; then HAS_QUEUE=no; fi
if test -z "$HAS_KQUEUE"; then
        AC_CHECK_FUNC(kqueue, [], [HAS_KQUEUE=no])
fi
if test -z "$HAS_KQUEUE"; then
        AC_CHECK_HEADER(sys/event.h, [], [HAS_KQUEUE=no])
fi
if test "$HAS_KQUEUE" != no; then HAS_KQUEUE=yes; fi
if test "$with_kqueue" = yes -a "$HAS_KQUEUE" = no; then
        AC_MSG_ERROR([*** cannot find kqueue support on this system])
fi
if test "$HAS_KQUEUE" = yes; then
        AC_DEFINE(HAVE_KQUEUE)
fi

AM_CONDITIONAL(USE_EPOLL, [test "$HAS_EPOLL" = yes])
AM_CONDITIONAL(USE_KQUEUE, [test "$HAS_KQUEUE" = yes])

AH_TEMPLATE(HAVE_XATTR, [define to compile support for filesystem extended attributes])
AC_ARG_WITH(xattr, AS_HELP_STRING([--with-xattr], [enable XATTR support]))
HAS_XATTR=""
if test "$with_xattr" = no; then HAS_XATTR=no; fi
if test -z "$HAS_XATTR"; then
	AC_CHECK_LIB(attr, getxattr, [:], [HAS_XATTR=no])
fi
if test -z "$HAS_XATTR"; then
	AC_CHECK_HEADER(attr/xattr.h, [], [HAS_XATTR=no])
fi
if test "$HAS_XATTR" != no; then HAS_XATTR=yes; fi
if test "$with_xattr" = yes -a "$HAS_XATTR" = no; then
	AC_MSG_ERROR([*** cannot find xattr support on this system])
fi
if test "$HAS_XATTR" = yes; then
	XATTR_LIBS=-lattr
	AC_DEFINE(HAVE_XATTR)
fi
AC_SUBST(XATTR_LIBS)

AH_TEMPLATE(HAVE_GNUTLS, [define to use the GnuTLS library for SSL support])
AH_TEMPLATE(HAVE_OPENSSL, [define to use the OpenSSL library for SSL support])
AC_ARG_WITH(gnutls, AS_HELP_STRING([--with-gnutls], [enable SSL support with the GnuTLS library]))
AC_ARG_WITH(openssl, AS_HELP_STRING([--with-openssl], [enable SSL support with the OpenSSL library]))
HAS_GNUTLS=""
HAS_OPENSSL=""

if test "$with_gnutls" = no; then HAS_GNUTLS=no; fi
if test -z "$HAS_GNUTLS"; then
	AC_CHECK_LIB(gnutls, gnutls_global_init, [:], [HAS_GNUTLS=no])
fi
if test -z "$HAS_GNUTLS";  then
	AC_CHECK_HEADER(gnutls/gnutls.h, [], [HAS_GNUTLS=no])
fi
if test "$HAS_GNUTLS" != no; then HAS_GNUTLS=yes; fi
if test "$with_gnutls" = yes -a "$HAS_GNUTLS" = no; then
	AC_MSG_ERROR([*** cannot find GnuTLS on this system])
fi
if test "$HAS_GNUTLS" = yes; then
	GNUTLS_LIBS=-lgnutls
	GNUTLS_CPPFLAGS=-D_GNU_SOURCE
	AC_DEFINE(HAVE_GNUTLS)
fi
AC_SUBST(GNUTLS_CPPFLAGS)
AC_SUBST(GNUTLS_LIBS)

if test "$with_openssl" = no; then
	HAS_OPENSSL=no
elif test -z "$with_openssl" -a "$HAS_GNUTLS" = yes; then
	HAS_OPENSSL=no
fi
if test -z "$HAS_OPENSSL"; then
	AC_CHECK_LIB(ssl, SSL_CTX_new, [:], [HAS_OPENSSL=no])
fi
if test -z "$HAS_OPENSSL"; then
	AC_CHECK_LIB(crypto, ERR_get_error, [:], [HAS_OPENSSL=no])
fi
if test -z "$HAS_OPENSSL";  then
	AC_CHECK_HEADER(openssl/ssl.h, [], [HAS_OPENSSL=no])
fi
if test "$HAS_OPENSSL" != no; then HAS_OPENSSL=yes; fi
if test "$with_openssl" = yes -a "$HAS_OPENSSL" = no; then
	AC_MSG_ERROR([*** cannot find OpenSSL on this system])
fi
if test "$HAS_OPENSSL" = yes; then
	OPENSSL_LIBS="-lssl -lcrypto"
	GNUTLS_CPPFLAGS=
	AC_DEFINE(HAVE_OPENSSL)
fi
AC_SUBST(OPENSSL_CPPFLAGS)
AC_SUBST(OPENSSL_LIBS)

if test "$HAS_GNUTLS" = yes -a "$HAS_OPENSSL" = yes; then
	AC_MSG_ERROR([*** only one of GnuTLS and OpenSSL must be enabled])
fi

AC_CONFIG_FILES([
Makefile
src/Makefile
src/dirplex/Makefile
lib/Makefile
doc/Makefile
])
AC_OUTPUT
