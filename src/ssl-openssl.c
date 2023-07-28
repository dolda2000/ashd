/*
    ashd - A Sane HTTP Daemon
    Copyright (C) 2008  Fredrik Tolf <fredrik@dolda2000.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <mt.h>
#include <mtio.h>
#include <req.h>
#include <log.h>
#include <bufio.h>

#include "htparser.h"

#ifdef HAVE_OPENSSL

#include <openssl/ssl.h>
#include <openssl/err.h>

struct sslport {
    int fd, sport;
    SSL_CTX *ctx;
};

struct sslconn {
    struct sslport *port;
    int fd;
    SSL *ssl;
    struct sockaddr *name;
    socklen_t namelen;
};

static int tlsblock(int fd, int err, int to)
{
    if(err == SSL_ERROR_WANT_READ) {
	if(block(fd, EV_READ, to) <= 0)
	    return(1);
	return(0);
    } else if(err == SSL_ERROR_WANT_WRITE) {
	if(block(fd, EV_WRITE, to) <= 0)
	    return(1);
	return(0);
    } else {
	return(1);
    }
}

static ssize_t sslread(void *cookie, void *buf, size_t len)
{
    struct sslconn *sdat = cookie;
    int ret, err, nb;
    size_t off;
    
    off = 0;
    while(off < len) {
	nb = ((len - off) > INT_MAX) ? INT_MAX : (len - off);
	if((ret = SSL_read(sdat->ssl, buf, nb)) <= 0) {
	    if(off > 0)
		return(off);
	    err = SSL_get_error(sdat->ssl, ret);
	    if(err == SSL_ERROR_ZERO_RETURN) {
		return(0);
	    } else if((err == SSL_ERROR_WANT_READ) || (err == SSL_ERROR_WANT_WRITE)) {
		if(tlsblock(sdat->fd, err, 60)) {
		    errno = ETIMEDOUT;
		    return(-1);
		}
	    } else {
		if(err != SSL_ERROR_SYSCALL)
		    errno = EPROTO;
		return(-1);
	    }
	} else {
	    off += ret;
	}
    }
    return(off);
}

static ssize_t sslwrite(void *cookie, const void *buf, size_t len)
{
    struct sslconn *sdat = cookie;
    int ret, err, nb;
    size_t off;
    
    off = 0;
    while(off < len) {
	nb = ((len - off) > INT_MAX) ? INT_MAX : (len - off);
	if((ret = SSL_write(sdat->ssl, buf, nb)) <= 0) {
	    if(off > 0)
		return(off);
	    err = SSL_get_error(sdat->ssl, ret);
	    if((err == SSL_ERROR_WANT_READ) || (err == SSL_ERROR_WANT_WRITE)) {
		if(tlsblock(sdat->fd, err, 60)) {
		    errno = ETIMEDOUT;
		    return(-1);
		}
	    } else {
		if(err != SSL_ERROR_SYSCALL)
		    errno = EIO;
		return(-1);
	    }
	} else {
	    off += ret;
	}
    }
    return(off);
}

static int sslclose(void *cookie)
{
    return(0);
}

static struct bufioops iofuns = {
    .read = sslread,
    .write = sslwrite,
    .close = sslclose,
};

static int initreq(struct conn *conn, struct hthead *req)
{
    struct sslconn *sdat = conn->pdata;
    struct sockaddr_storage sa;
    socklen_t salen;
    
    headappheader(req, "X-Ash-Address", formathaddress(sdat->name, sdat->namelen));
    if(sdat->name->sa_family == AF_INET)
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in *)sdat->name)->sin_port)));
    else if(sdat->name->sa_family == AF_INET6)
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in6 *)sdat->name)->sin6_port)));
    salen = sizeof(sa);
    if(!getsockname(sdat->fd, (struct sockaddr *)&sa, &salen))
	headappheader(req, "X-Ash-Server-Address", formathaddress((struct sockaddr *)&sa, salen));
    headappheader(req, "X-Ash-Server-Port", sprintf3("%i", sdat->port->sport));
    headappheader(req, "X-Ash-Protocol", "https");
    return(0);
}

static void servessl(struct muth *muth, va_list args)
{
    vavar(int, fd);
    vavar(struct sockaddr_storage, name);
    vavar(struct sslport *, pd);
    int ret;
    SSL *ssl;
    struct conn conn;
    struct sslconn sdat;
    
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    ssl = SSL_new(pd->ctx);
    SSL_set_fd(ssl, fd);
    while((ret = SSL_accept(ssl)) <= 0) {
	if(tlsblock(fd, SSL_get_error(ssl, ret), 60))
	    goto out;
    }
    memset(&conn, 0, sizeof(conn));
    memset(&sdat, 0, sizeof(sdat));
    conn.pdata = &sdat;
    conn.initreq = initreq;
    sdat.port = pd;
    sdat.fd = fd;
    sdat.ssl = ssl;
    sdat.name = (struct sockaddr *)&name;
    sdat.namelen = sizeof(name);
    serve(bioopen(&sdat, &iofuns), fd, &conn);
    while((ret = SSL_shutdown(ssl)) < 0) {
	if(tlsblock(fd, SSL_get_error(ssl, ret), 60))
	    goto out;
    }
out:
    SSL_free(ssl);
    close(fd);
}

static void listenloop(struct muth *muth, va_list args)
{
    vavar(struct sslport *, pd);
    int i, ns, n;
    struct sockaddr_storage name;
    socklen_t namelen;
    
    fcntl(pd->fd, F_SETFL, fcntl(pd->fd, F_GETFL) | O_NONBLOCK);
    while(1) {
	namelen = sizeof(name);
	if(block(pd->fd, EV_READ, 0) == 0)
	    goto out;
	for(n = 0; n < 100; n++) {
	    if((ns = accept(pd->fd, (struct sockaddr *)&name, &namelen)) < 0) {
		if(errno == EAGAIN)
		    break;
		if(errno == ECONNABORTED)
		    continue;
		flog(LOG_ERR, "accept: %s", strerror(errno));
		goto out;
	    }
	    mustart(servessl, ns, name, pd);
	}
    }
    
out:
    close(pd->fd);
    free(pd);
    for(i = 0; i < listeners.d; i++) {
	if(listeners.b[i] == muth)
	    bufdel(listeners, i);
    }
}

void handleossl(int argc, char **argp, char **argv)
{
    int i, port, fd;
    SSL_CTX *ctx;
    char *crtfile, *keyfile;
    struct sslport *pd;
    
    ctx = SSL_CTX_new(TLS_server_method());
    if(!ctx) {
	flog(LOG_ERR, "ssl: could not create context: %s", ERR_error_string(ERR_get_error(), NULL));
	exit(1);
    }
    port = 443;
    for(i = 0; i < argc; i++) {
	if(!strcmp(argp[i], "help")) {
	    printf("ssl handler parameters:\n");
	    printf("\tcert=CERT-FILE  [mandatory]\n");
	    printf("\t\tThe name of the file to read the certificate from.\n");
	    printf("\tkey=KEY-FILE    [same as CERT-FILE]\n");
	    printf("\t\tThe name of the file to read the private key from.\n");
	    printf("\tport=PORT       [443]\n");
	    printf("\t\tThe TCP port to listen on.\n");
	    exit(0);
	} else if(!strcmp(argp[i], "cert")) {
	    crtfile = argv[i];
	} else if(!strcmp(argp[i], "key")) {
	    keyfile = argv[i];
	} else if(!strcmp(argp[i], "port")) {
	    port = atoi(argv[i]);
	} else {
	    flog(LOG_ERR, "unknown parameter `%s' to ssl handler", argp[i]);
	    exit(1);
	}
    }
    if(crtfile == NULL) {
	flog(LOG_ERR, "ssl: needs certificate file at the very least");
	exit(1);
    }
    if(keyfile == NULL)
	keyfile = crtfile;
    if(SSL_CTX_use_certificate_file(ctx, crtfile, SSL_FILETYPE_PEM) <= 0) {
	flog(LOG_ERR, "ssl: could not load certificate: %s", ERR_error_string(ERR_get_error(), NULL));
	exit(1);
    }
    if(SSL_CTX_use_PrivateKey_file(ctx, keyfile, SSL_FILETYPE_PEM) <= 0) {
	flog(LOG_ERR, "ssl: could not load certificate: %s", ERR_error_string(ERR_get_error(), NULL));
	exit(1);
    }
    if(!SSL_CTX_check_private_key(ctx)) {
	flog(LOG_ERR, "ssl: key and certificate do not match");
	exit(1);
    }
    if((fd = listensock6(port)) < 0) {
	flog(LOG_ERR, "could not listen on IPv65 port (port %i): %s", port, strerror(errno));
	exit(1);
    }
    omalloc(pd);
    pd->fd = fd;
    pd->sport = port;
    pd->ctx = ctx;
    bufadd(listeners, mustart(listenloop, pd));
    if((fd = listensock4(port)) < 0) {
	if(errno != EADDRINUSE) {
	    flog(LOG_ERR, "could not listen on IPv4 port (port %i): Is", port, strerror(errno));
	    exit(1);
	}
    } else {
	omalloc(pd);
	pd->fd = fd;
	pd->sport = port;
	pd->ctx = ctx;
	bufadd(listeners, mustart(listenloop, pd));
    }
}

#endif
