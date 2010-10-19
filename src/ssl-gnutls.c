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

#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <mt.h>
#include <mtio.h>
#include <req.h>
#include <log.h>

#include "htparser.h"

#ifdef HAVE_GNUTLS

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

struct namedcreds {
    char **names;
    gnutls_certificate_credentials_t creds;
};

struct ncredbuf {
    struct namedcreds **b;
    size_t s, d;
};

struct sslport {
    int fd;
    int sport;
    gnutls_certificate_credentials_t creds;
    struct namedcreds **ncreds;
};

struct sslconn {
    int fd;
    struct sslport *port;
    struct sockaddr_storage name;
    gnutls_session_t sess;
    struct charbuf in;
};

static gnutls_dh_params_t dhparams;

static int tlsblock(int fd, gnutls_session_t sess, time_t to)
{
    if(gnutls_record_get_direction(sess))
	return(block(fd, EV_WRITE, to));
    else
	return(block(fd, EV_READ, to));
}

static ssize_t sslread(void *cookie, char *buf, size_t len)
{
    struct sslconn *ssl = cookie;
    ssize_t xf;
    int ret;

    while(ssl->in.d == 0) {
	sizebuf(ssl->in, ssl->in.d + 1024);
	ret = gnutls_record_recv(ssl->sess, ssl->in.b + ssl->in.d, ssl->in.s - ssl->in.d);
	if((ret == GNUTLS_E_INTERRUPTED) || (ret == GNUTLS_E_AGAIN)) {
	    if(tlsblock(ssl->fd, ssl->sess, 60) == 0) {
		errno = ETIMEDOUT;
		return(-1);
	    }
	} else if(ret < 0) {
	    errno = EIO;
	    return(-1);
	} else if(ret == 0) {
	    return(0);
	} else {
	    ssl->in.d += ret;
	}
    }
    xf = min(ssl->in.d, len);
    memcpy(buf, ssl->in.b, xf);
    memmove(ssl->in.b, ssl->in.b + xf, ssl->in.d -= xf);
    return(xf);
}

static ssize_t sslwrite(void *cookie, const char *buf, size_t len)
{
    struct sslconn *ssl = cookie;
    int ret;
    size_t off;
    
    off = 0;
    while(off < len) {
	ret = gnutls_record_send(ssl->sess, buf + off, len - off);
	if((ret == GNUTLS_E_INTERRUPTED) || (ret == GNUTLS_E_AGAIN)) {
	    if(tlsblock(ssl->fd, ssl->sess, 60) == 0) {
		if(off == 0) {
		    errno = ETIMEDOUT;
		    return(-1);
		}
		return(off);
	    }
	} else if(ret < 0) {
	    if(off == 0) {
		errno = EIO;
		return(-1);
	    }
	    return(off);
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

static cookie_io_functions_t iofuns = {
    .read = sslread,
    .write = sslwrite,
    .close = sslclose,
};

static int initreq(struct conn *conn, struct hthead *req)
{
    struct sslconn *ssl = conn->pdata;
    struct sockaddr_storage sa;
    socklen_t salen;
    char nmbuf[256];
    
    headappheader(req, "X-Ash-Address", formathaddress((struct sockaddr *)&ssl->name, sizeof(sa)));
    if(ssl->name.ss_family == AF_INET) {
	headappheader(req, "X-Ash-Address", inet_ntop(AF_INET, &((struct sockaddr_in *)&ssl->name)->sin_addr, nmbuf, sizeof(nmbuf)));
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in *)&ssl->name)->sin_port)));
    } else if(ssl->name.ss_family == AF_INET6) {
	headappheader(req, "X-Ash-Address", inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&ssl->name)->sin6_addr, nmbuf, sizeof(nmbuf)));
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in6 *)&ssl->name)->sin6_port)));
    }
    salen = sizeof(sa);
    if(!getsockname(ssl->fd, (struct sockaddr *)&sa, &salen))
	headappheader(req, "X-Ash-Server-Address", formathaddress((struct sockaddr *)&sa, sizeof(sa)));
    headappheader(req, "X-Ash-Server-Port", sprintf3("%i", ssl->port->sport));
    headappheader(req, "X-Ash-Protocol", "https");
    return(0);
}

static void servessl(struct muth *muth, va_list args)
{
    vavar(int, fd);
    vavar(struct sockaddr_storage, name);
    vavar(struct sslport *, pd);
    struct conn conn;
    struct sslconn ssl;
    gnutls_session_t sess;
    int ret;
    FILE *in;
    
    int setcreds(gnutls_session_t sess)
    {
	int i, o, u;
	unsigned int ntype;
	char nambuf[256];
	size_t namlen;
	
	for(i = 0; 1; i++) {
	    namlen = sizeof(nambuf);
	    if(gnutls_server_name_get(sess, nambuf, &namlen, &ntype, i) != 0)
		break;
	    if(ntype != GNUTLS_NAME_DNS)
		continue;
	    for(o = 0; pd->ncreds[o] != NULL; o++) {
		for(u = 0; pd->ncreds[o]->names[u] != NULL; u++) {
		    if(!strcmp(pd->ncreds[o]->names[u], nambuf)) {
			gnutls_credentials_set(sess, GNUTLS_CRD_CERTIFICATE, pd->ncreds[o]->creds);
			gnutls_certificate_server_set_request(sess, GNUTLS_CERT_REQUEST);
			return(0);
		    }
		}
	    }
	}
	gnutls_credentials_set(sess, GNUTLS_CRD_CERTIFICATE, pd->creds);
	gnutls_certificate_server_set_request(sess, GNUTLS_CERT_REQUEST);
	return(0);
    }

    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    gnutls_init(&sess, GNUTLS_SERVER);
    gnutls_set_default_priority(sess);
    gnutls_handshake_set_post_client_hello_function(sess, setcreds);
    gnutls_transport_set_ptr(sess, (gnutls_transport_ptr_t)(intptr_t)fd);
    while((ret = gnutls_handshake(sess)) != 0) {
	if((ret != GNUTLS_E_INTERRUPTED) && (ret != GNUTLS_E_AGAIN))
	    goto out;
	if(tlsblock(fd, sess, 60) <= 0)
	    goto out;
    }
    memset(&conn, 0, sizeof(conn));
    memset(&ssl, 0, sizeof(ssl));
    conn.pdata = &ssl;
    conn.initreq = initreq;
    ssl.fd = fd;
    ssl.port = pd;
    ssl.name = name;
    ssl.sess = sess;
    bufinit(ssl.in);
    in = fopencookie(&ssl, "r+", iofuns);
    serve(in, &conn);
    
out:
    gnutls_deinit(sess);
    close(fd);
}

static void listenloop(struct muth *muth, va_list args)
{
    vavar(struct sslport *, pd);
    int ns;
    struct sockaddr_storage name;
    socklen_t namelen;
    
    while(1) {
	namelen = sizeof(name);
	block(pd->fd, EV_READ, 0);
	ns = accept(pd->fd, (struct sockaddr *)&name, &namelen);
	if(ns < 0) {
	    flog(LOG_ERR, "accept: %s", strerror(errno));
	    goto out;
	}
	mustart(servessl, ns, name, pd);
    }
    
out:
    close(pd->fd);
    free(pd);
}

static void init(void)
{
    static int inited = 0;
    int ret;
    
    if(inited)
	return;
    inited = 1;
    if((ret = gnutls_global_init()) != 0) {
	flog(LOG_ERR, "could not initialize GnuTLS: %s", gnutls_strerror(ret));
	exit(1);
    }
    if(((ret = gnutls_dh_params_init(&dhparams)) != 0) ||
       ((ret = gnutls_dh_params_generate2(dhparams, 2048)) != 0)) {
	flog(LOG_ERR, "GnuTLS could not generate Diffie-Hellman parameters: %s", gnutls_strerror(ret));
	exit(1);
    }
}

static struct namedcreds *readncreds(char *file)
{
    int i, fd, ret;
    struct namedcreds *nc;
    gnutls_x509_crt_t crt;
    gnutls_x509_privkey_t key;
    char cn[1024];
    size_t cnl;
    gnutls_datum_t d;
    struct charbuf keybuf;
    struct charvbuf names;
    unsigned int type;
    
    bufinit(keybuf);
    bufinit(names);
    if((fd = open(file, O_RDONLY)) < 0) {
	flog(LOG_ERR, "ssl: %s: %s", file, strerror(errno));
	exit(1);
    }
    while(1) {
	sizebuf(keybuf, keybuf.d + 1024);
	ret = read(fd, keybuf.b + keybuf.d, keybuf.s - keybuf.d);
	if(ret < 0) {
	    flog(LOG_ERR, "ssl: reading from %s: %s", file, strerror(errno));
	    exit(1);
	} else if(ret == 0) {
	    break;
	}
	keybuf.d += ret;
    }
    close(fd);
    d.data = (unsigned char *)keybuf.b;
    d.size = keybuf.d;
    gnutls_x509_crt_init(&crt);
    if((ret = gnutls_x509_crt_import(crt, &d, GNUTLS_X509_FMT_PEM)) != 0) {
	flog(LOG_ERR, "ssl: could not load certificate from %s: %s", file, gnutls_strerror(ret));
	exit(1);
    }
    cnl = sizeof(cn) - 1;
    if((ret = gnutls_x509_crt_get_dn_by_oid(crt, GNUTLS_OID_X520_COMMON_NAME, 0, 0, cn, &cnl)) != 0) {
	flog(LOG_ERR, "ssl: could not read common name from %s: %s", file, gnutls_strerror(ret));
	exit(1);
    }
    cn[cnl] = 0;
    bufadd(names, sstrdup(cn));
    for(i = 0; 1; i++) {
	cnl = sizeof(cn) - 1;
	if(gnutls_x509_crt_get_subject_alt_name2(crt, i, cn, &cnl, &type, NULL) < 0)
	    break;
	cn[cnl] = 0;
	if(type == GNUTLS_SAN_DNSNAME)
	    bufadd(names, sstrdup(cn));
    }
    gnutls_x509_privkey_init(&key);
    if((ret = gnutls_x509_privkey_import(key, &d, GNUTLS_X509_FMT_PEM)) != 0) {
	flog(LOG_ERR, "ssl: could not load key from %s: %s", file, gnutls_strerror(ret));
	exit(1);
    }
    buffree(keybuf);
    bufadd(names, NULL);
    omalloc(nc);
    nc->names = names.b;
    gnutls_certificate_allocate_credentials(&nc->creds);
    if((ret = gnutls_certificate_set_x509_key(nc->creds, &crt, 1, key)) != 0) {
	flog(LOG_ERR, "ssl: could not use certificate from %s: %s", file, gnutls_strerror(ret));
	exit(1);
    }
    gnutls_certificate_set_dh_params(nc->creds, dhparams);
    return(nc);
}

void handlegnussl(int argc, char **argp, char **argv)
{
    int i, ret, port, fd;
    gnutls_certificate_credentials_t creds;
    struct ncredbuf ncreds;
    struct sslport *pd;
    char *crtfile, *keyfile;
    
    init();
    port = 443;
    bufinit(ncreds);
    gnutls_certificate_allocate_credentials(&creds);
    keyfile = crtfile = NULL;
    for(i = 0; i < argc; i++) {
	if(!strcmp(argp[i], "help")) {
	    printf("ssl handler parameters:\n");
	    printf("\tcert=CERT-FILE  [mandatory]\n");
	    printf("\t\tThe name of the file to read the certificate from.\n");
	    printf("\tkey=KEY-FILE    [same as CERT-FILE]\n");
	    printf("\t\tThe name of the file to read the private key from.\n");
	    printf("\ttrust=CA-FILE   [no default]\n");
	    printf("\t\tThe name of a file to read trusted certificates from.\n");
	    printf("\t\tMay be given multiple times.\n");
	    printf("\tcrl=CRL-FILE    [no default]\n");
	    printf("\t\tThe name of a file to read revocation lists from.\n");
	    printf("\t\tMay be given multiple times.\n");
	    printf("\tport=PORT       [443]\n");
	    printf("\t\tThe TCP port to listen on.\n");
	    printf("\n");
	    printf("\tAll X.509 data files must be PEM-encoded.\n");
	    printf("\tSee the manpage for information on specifying multiple\n\tcertificates to support SNI operation.\n");
	    exit(0);
	} else if(!strcmp(argp[i], "cert")) {
	    crtfile = argv[i];
	} else if(!strcmp(argp[i], "key")) {
	    keyfile = argv[i];
	} else if(!strcmp(argp[i], "trust")) {
	    if((ret = gnutls_certificate_set_x509_trust_file(creds, argv[i], GNUTLS_X509_FMT_PEM)) != 0) {
		flog(LOG_ERR, "ssl: could not load trust file `%s': %s", argv[i], gnutls_strerror(ret));
		exit(1);
	    }
	    for(i = 0; i < ncreds.d; i++) {
		if((ret = gnutls_certificate_set_x509_trust_file(ncreds.b[i]->creds, argv[i], GNUTLS_X509_FMT_PEM)) != 0) {
		    flog(LOG_ERR, "ssl: could not load trust file `%s': %s", argv[i], gnutls_strerror(ret));
		    exit(1);
		}
	    }
	} else if(!strcmp(argp[i], "crl")) {
	    if((ret = gnutls_certificate_set_x509_crl_file(creds, argv[i], GNUTLS_X509_FMT_PEM)) != 0) {
		flog(LOG_ERR, "ssl: could not load CRL file `%s': %s", argv[i], gnutls_strerror(ret));
		exit(1);
	    }
	    for(i = 0; i < ncreds.d; i++) {
		if((ret = gnutls_certificate_set_x509_crl_file(ncreds.b[i]->creds, argv[i], GNUTLS_X509_FMT_PEM)) != 0) {
		    flog(LOG_ERR, "ssl: could not load CRL file `%s': %s", argv[i], gnutls_strerror(ret));
		    exit(1);
		}
	    }
	} else if(!strcmp(argp[i], "port")) {
	    port = atoi(argv[i]);
	} else if(!strcmp(argp[i], "ncert")) {
	    bufadd(ncreds, readncreds(argv[i]));
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
    if((ret = gnutls_certificate_set_x509_key_file(creds, crtfile, keyfile, GNUTLS_X509_FMT_PEM)) != 0) {
	flog(LOG_ERR, "ssl: could not load certificate or key: %s", gnutls_strerror(ret));
	exit(1);
    }
    gnutls_certificate_set_dh_params(creds, dhparams);
    if((fd = listensock6(port)) < 0) {
	flog(LOG_ERR, "could not listen on IPv6 port (port %i): %s", port, strerror(errno));
	exit(1);
    }
    bufadd(ncreds, NULL);
    omalloc(pd);
    pd->fd = fd;
    pd->sport = port;
    pd->creds = creds;
    pd->ncreds = ncreds.b;
    mustart(listenloop, pd);
    if((fd = listensock6(port)) < 0) {
	if(errno != EADDRINUSE) {
	    flog(LOG_ERR, "could not listen on IPv6 port (port %i): %s", port, strerror(errno));
	    exit(1);
	}
    } else {
	omalloc(pd);
	pd->fd = fd;
	pd->sport = port;
	pd->creds = creds;
	mustart(listenloop, pd);
    }
}

#endif
