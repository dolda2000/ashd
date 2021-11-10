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
#include <dirent.h>
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
#include <bufio.h>

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
    gnutls_priority_t ciphers;
    struct namedcreds **ncreds;
};

struct sslconn {
    int fd;
    struct sslport *port;
    struct sockaddr_storage name;
    gnutls_session_t sess;
    struct charbuf in;
};

struct savedsess {
    struct savedsess *next, *prev;
    gnutls_datum_t key, value;
};

struct certbuffer {
    gnutls_x509_crt_t *b;
    size_t s, d;
};

static int numconn = 0, numsess = 0;
static struct btree *sessidx = NULL;
static struct savedsess *sesslistf = NULL, *sesslistl = NULL;

static int sesscmp(void *ap, void *bp)
{
    struct savedsess *a = ap, *b = bp;
    
    if(a->key.size != b->key.size)
	return(a->key.size - b->key.size);
    return(memcmp(a->key.data, b->key.data, a->key.size));
}

static gnutls_datum_t sessdbfetch(void *uudata, gnutls_datum_t key)
{
    struct savedsess *sess, lkey;
    gnutls_datum_t ret;
    
    memset(&ret, 0, sizeof(ret));
    lkey.key = key;
    if((sess = btreeget(sessidx, &lkey, sesscmp)) == NULL)
	return(ret);
    ret.data = memcpy(gnutls_malloc(ret.size = sess->value.size), sess->value.data, sess->value.size);
    return(ret);
}

static void freesess(struct savedsess *sess)
{
    bbtreedel(&sessidx, sess, sesscmp);
    if(sess->next)
	sess->next->prev = sess->prev;
    if(sess->prev)
	sess->prev->next = sess->next;
    if(sess == sesslistf)
	sesslistf = sess->next;
    if(sess == sesslistl)
	sesslistl = sess->prev;
    free(sess->key.data);
    free(sess->value.data);
    free(sess);
    numsess--;
}

static int sessdbdel(void *uudata, gnutls_datum_t key)
{
    struct savedsess *sess, lkey;
    
    lkey.key = key;
    if((sess = btreeget(sessidx, &lkey, sesscmp)) == NULL)
	return(-1);
    freesess(sess);
    return(0);
}

static void cleansess(void)
{
    while(numsess > (max(numconn, 1) * 100))
	freesess(sesslistl);
}

static int sessdbstore(void *uudata, gnutls_datum_t key, gnutls_datum_t value)
{
    static int cc = 0;
    struct savedsess *sess, lkey;
    
    if((value.data == NULL) || (value.size == 0)) {
	sessdbdel(NULL, key);
	return(0);
    }
    lkey.key = key;
    if((sess = btreeget(sessidx, &lkey, sesscmp)) == NULL) {
	omalloc(sess);
	sess->key.data = memcpy(smalloc(sess->key.size = key.size), key.data, key.size);
	sess->value.data = memcpy(smalloc(sess->value.size = value.size), value.data, value.size);
	bbtreeput(&sessidx, sess, sesscmp);
	sess->prev = NULL;
	sess->next = sesslistf;
	if(sesslistf)
	    sesslistf->prev = sess;
	sesslistf = sess;
	if(sesslistl == NULL)
	    sesslistl = sess;
	numsess++;
    } else {
	free(sess->value.data);
	sess->value.data = memcpy(smalloc(sess->value.size = value.size), value.data, value.size);
	if(sess != sesslistf) {
	    if(sess->next)
		sess->next->prev = sess->prev;
	    if(sess->prev)
		sess->prev->next = sess->next;
	    if(sess == sesslistl)
		sesslistl = sess->prev;
	    sess->prev = NULL;
	    sess->next = sesslistf;
	    if(sesslistf)
		sesslistf->prev = sess;
	    sesslistf = sess;
	}
    }
    if(cc++ > 100) {
	cleansess();
	cc = 0;
    }
    return(0);
}

static int tlsblock(int fd, gnutls_session_t sess, time_t to)
{
    if(gnutls_record_get_direction(sess))
	return(block(fd, EV_WRITE, to));
    else
	return(block(fd, EV_READ, to));
}

static ssize_t sslread(void *cookie, void *buf, size_t len)
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

static ssize_t sslwrite(void *cookie, const void *buf, size_t len)
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
    struct sslconn *ssl = cookie;
    
    buffree(ssl->in);
    return(0);
}

static struct bufioops iofuns = {
    .read = sslread,
    .write = sslwrite,
    .close = sslclose,
};

static int initreq(struct conn *conn, struct hthead *req)
{
    struct sslconn *ssl = conn->pdata;
    struct sockaddr_storage sa;
    socklen_t salen;
    
    headappheader(req, "X-Ash-Address", formathaddress((struct sockaddr *)&ssl->name, sizeof(sa)));
    if(ssl->name.ss_family == AF_INET)
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in *)&ssl->name)->sin_port)));
    else if(ssl->name.ss_family == AF_INET6)
	headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in6 *)&ssl->name)->sin6_port)));
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

    numconn++;
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    gnutls_init(&sess, GNUTLS_SERVER);
    gnutls_priority_set(sess, pd->ciphers);
    gnutls_db_set_retrieve_function(sess, sessdbfetch);
    gnutls_db_set_store_function(sess, sessdbstore);
    gnutls_db_set_remove_function(sess, sessdbdel);
    gnutls_db_set_ptr(sess, NULL);
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
    serve(bioopen(&ssl, &iofuns), fd, &conn);
    
out:
    gnutls_deinit(sess);
    close(fd);
    numconn--;
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
	n = 0;
	while(1) {
	    ns = accept(pd->fd, (struct sockaddr *)&name, &namelen);
	    if(ns < 0) {
		if(errno == EAGAIN)
		    break;
		if(errno == ECONNABORTED)
		    continue;
		flog(LOG_ERR, "accept: %s", strerror(errno));
		goto out;
	    }
	    mustart(servessl, ns, name, pd);
	    if(++n >= 100)
		break;
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

static gnutls_dh_params_t dhparams(void)
{
    static int inited = 0;
    static gnutls_dh_params_t pars;
    int ret;
    
    if(!inited) {
	if(((ret = gnutls_dh_params_init(&pars)) != 0) ||
	   ((ret = gnutls_dh_params_generate2(pars, 2048)) != 0)) {
	    flog(LOG_ERR, "GnuTLS could not generate Diffie-Hellman parameters: %s", gnutls_strerror(ret));
	    exit(1);
	}
	inited = 1;
    }
    return(pars);
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
}

/* This implementation seems somewhat ugly, but it's the way the
 * GnuTLS implements the same thing internally, so it should probably
 * be interoperable, at least. */
static int readcrtchain(struct certbuffer *ret, struct charbuf *pem)
{
    static char *headers[] = {"-----BEGIN CERTIFICATE", "-----BEGIN X509 CERTIFICATE"};
    int i, rv;
    char *p, *p2, *f;
    gnutls_x509_crt_t crt;
    
    for(i = 0, p = NULL; i < sizeof(headers) / sizeof(*headers); i++) {
	f = memmem(pem->b, pem->d, headers[i], strlen(headers[i]));
	if((f != NULL) && ((p == NULL) || (f < p)))
	    p = f;
    }
    if(p == NULL)
	return(-GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE);
    do {
	if((rv = gnutls_x509_crt_init(&crt)) < 0)
	    goto error;
	if((rv = gnutls_x509_crt_import(crt, &(gnutls_datum_t){.data = (unsigned char *)p, .size = pem->d - (p - pem->b)}, GNUTLS_X509_FMT_PEM)) < 0) {
	    gnutls_x509_crt_deinit(crt);
	    goto error;
	}
	bufadd(*ret, crt);
	for(i = 0, p2 = NULL; i < sizeof(headers) / sizeof(*headers); i++) {
	    f = memmem(p + 1, pem->d - (p + 1 - pem->b), headers[i], strlen(headers[i]));
	    if((f != NULL) && ((p2 == NULL) || (f < p2)))
		p2 = f;
	}
    } while((p = p2) != NULL);
    return(0);
error:
    for(i = 0; i < ret->d; i++)
	gnutls_x509_crt_deinit(ret->b[i]);
    ret->d = 0;
    return(rv);
}

static struct namedcreds *readncreds(char *file, gnutls_x509_privkey_t defkey)
{
    int i, fd, ret;
    struct namedcreds *nc;
    struct certbuffer crts;
    gnutls_x509_privkey_t key;
    char cn[1024];
    size_t cnl;
    struct charbuf keybuf;
    struct charvbuf names;
    unsigned int type;
    
    bufinit(keybuf);
    bufinit(crts);
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
    if((ret = readcrtchain(&crts, &keybuf)) != 0) {
	flog(LOG_ERR, "ssl: could not load certificate chain from %s: %s", file, gnutls_strerror(ret));
	exit(1);
    }
    cnl = sizeof(cn) - 1;
    if((ret = gnutls_x509_crt_get_dn_by_oid(crts.b[0], GNUTLS_OID_X520_COMMON_NAME, 0, 0, cn, &cnl)) != 0) {
	flog(LOG_ERR, "ssl: could not read common name from %s: %s", file, gnutls_strerror(ret));
	exit(1);
    }
    cn[cnl] = 0;
    bufadd(names, sstrdup(cn));
    for(i = 0; 1; i++) {
	cnl = sizeof(cn) - 1;
	if(gnutls_x509_crt_get_subject_alt_name2(crts.b[0], i, cn, &cnl, &type, NULL) < 0)
	    break;
	cn[cnl] = 0;
	if(type == GNUTLS_SAN_DNSNAME)
	    bufadd(names, sstrdup(cn));
    }
    gnutls_x509_privkey_init(&key);
    if((ret = gnutls_x509_privkey_import(key, &(gnutls_datum_t){.data = (unsigned char *)keybuf.b, .size = keybuf.d}, GNUTLS_X509_FMT_PEM)) != 0) {
	if(ret == GNUTLS_E_REQUESTED_DATA_NOT_AVAILABLE) {
	    gnutls_x509_privkey_deinit(key);
	    key = defkey;
	} else {
	    flog(LOG_ERR, "ssl: could not load key from %s: %s", file, gnutls_strerror(ret));
	    exit(1);
	}
    }
    buffree(keybuf);
    bufadd(names, NULL);
    omalloc(nc);
    nc->names = names.b;
    gnutls_certificate_allocate_credentials(&nc->creds);
    if((ret = gnutls_certificate_set_x509_key(nc->creds, crts.b, crts.d, key)) != 0) {
	flog(LOG_ERR, "ssl: could not use certificate from %s: %s", file, gnutls_strerror(ret));
	exit(1);
    }
    gnutls_certificate_set_dh_params(nc->creds, dhparams());
    return(nc);
}

static void readncdir(struct ncredbuf *buf, char *dir, gnutls_x509_privkey_t defkey)
{
    DIR *d;
    struct dirent *e;
    size_t es;
    
    if((d = opendir(dir)) == NULL) {
	flog(LOG_ERR, "ssl: could not read certificate directory %s: %s", dir, strerror(errno));
	exit(1);
    }
    while((e = readdir(d)) != NULL) {
	if(e->d_name[0] == '.')
	    continue;
	if((es = strlen(e->d_name)) <= 4)
	    continue;
	if(strcmp(e->d_name + es - 4, ".crt"))
	    continue;
	bufadd(*buf, readncreds(sprintf3("%s/%s", dir, e->d_name), defkey));
    }
    closedir(d);
}

void handlegnussl(int argc, char **argp, char **argv)
{
    int i, ret, port, fd;
    gnutls_certificate_credentials_t creds;
    gnutls_priority_t ciphers;
    gnutls_x509_privkey_t defkey;
    struct ncredbuf ncreds;
    struct charvbuf ncertf, ncertd;
    struct sslport *pd;
    char *crtfile, *keyfile, *perr;
    
    init();
    port = 443;
    bufinit(ncreds);
    bufinit(ncertf);
    bufinit(ncertd);
    gnutls_certificate_allocate_credentials(&creds);
    keyfile = crtfile = NULL;
    ciphers = NULL;
    for(i = 0; i < argc; i++) {
	if(!strcmp(argp[i], "help")) {
	    printf("ssl handler parameters:\n");
	    printf("\tcert=CERT-FILE  [mandatory]\n");
	    printf("\t\tThe name of the file to read the certificate from.\n");
	    printf("\tkey=KEY-FILE    [same as CERT-FILE]\n");
	    printf("\t\tThe name of the file to read the private key from.\n");
	    printf("\tprio=PRIORITIES [NORMAL]\n");
	    printf("\t\tCiphersuite priorities, as a GnuTLS priority string.\n");
	    printf("\ttrust=CA-FILE   [no default]\n");
	    printf("\t\tThe name of a file to read trusted certificates from.\n");
	    printf("\t\tMay be given multiple times.\n");
	    printf("\tcrl=CRL-FILE    [no default]\n");
	    printf("\t\tThe name of a file to read revocation lists from.\n");
	    printf("\t\tMay be given multiple times.\n");
	    printf("\tncert=CERT-FILE [no default]\n");
	    printf("\t\tThe name of a file to read a named certificate from,\n");
	    printf("\t\tfor use with SNI-enabled clients.\n");
	    printf("\t\tMay be given multiple times.\n");
	    printf("\tncertdir=DIR    [no default]\n");
	    printf("\t\tRead all *.crt files in the given directory as if they\n");
	    printf("\t\twere given with `ncert' options.\n");
	    printf("\t\tMay be given multiple times.\n");
	    printf("\tport=PORT       [443]\n");
	    printf("\t\tThe TCP port to listen on.\n");
	    printf("\n");
	    printf("\tAll X.509 data files must be PEM-encoded.\n");
	    printf("\tIf any certificates were given with `ncert' options, they will be\n");
	    printf("\tused if a client explicitly names one of them with a\n");
	    printf("\tserver-name indication. If a client indicates no server name,\n");
	    printf("\tor if a server-name indication does not match any given\n");
	    printf("\tcertificate, the certificate given with the `cert' option will\n");
	    printf("\tbe used instead.\n");
	    exit(0);
	} else if(!strcmp(argp[i], "cert")) {
	    crtfile = argv[i];
	} else if(!strcmp(argp[i], "key")) {
	    keyfile = argv[i];
	} else if(!strcmp(argp[i], "prio")) {
	    if(ciphers != NULL)
		gnutls_priority_deinit(ciphers);
	    ret = gnutls_priority_init(&ciphers, argv[i], (const char **)&perr);
	    if(ret == GNUTLS_E_INVALID_REQUEST) {
		flog(LOG_ERR, "ssl: invalid cipher priority string, at `%s'", perr);
		exit(1);
	    } else if(ret != 0) {
		flog(LOG_ERR, "ssl: could not initialize cipher priorities: %s", gnutls_strerror(ret));
		exit(1);
	    }
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
	    bufadd(ncertf, argv[i]);
	} else if(!strcmp(argp[i], "ncertdir")) {
	    bufadd(ncertd, argv[i]);
	} else {
	    flog(LOG_ERR, "unknown parameter `%s' to ssl handler", argp[i]);
	    exit(1);
	}
    }
    if(crtfile == NULL) {
	flog(LOG_ERR, "ssl: needs certificate file at the very least");
	exit(1);
    }
    if((fd = listensock6(port)) < 0) {
	flog(LOG_ERR, "could not listen on IPv6 port (port %i): %s", port, strerror(errno));
	exit(1);
    }
    if(keyfile == NULL)
	keyfile = crtfile;
    if((ret = gnutls_certificate_set_x509_key_file(creds, crtfile, keyfile, GNUTLS_X509_FMT_PEM)) != 0) {
	flog(LOG_ERR, "ssl: could not load certificate or key: %s", gnutls_strerror(ret));
	exit(1);
    }
    if((ciphers == NULL) && ((ret = gnutls_priority_init(&ciphers, "NORMAL", NULL)) != 0)) {
	flog(LOG_ERR, "ssl: could not initialize cipher priorities: %s", gnutls_strerror(ret));
	exit(1);
    }
    if((ret = gnutls_certificate_get_x509_key(creds, 0, &defkey)) != 0) {
	flog(LOG_ERR, "ssl: could not get default key: %s", gnutls_strerror(ret));
	exit(1);
    }
    for(i = 0; i < ncertf.d; i++)
	bufadd(ncreds, readncreds(ncertf.b[i], defkey));
    for(i = 0; i < ncertd.d; i++)
	readncdir(&ncreds, ncertd.b[i], defkey);
    buffree(ncertf);
    buffree(ncertd);
    gnutls_certificate_set_dh_params(creds, dhparams());
    bufadd(ncreds, NULL);
    omalloc(pd);
    pd->fd = fd;
    pd->sport = port;
    pd->creds = creds;
    pd->ncreds = ncreds.b;
    pd->ciphers = ciphers;
    bufadd(listeners, mustart(listenloop, pd));
    if((fd = listensock4(port)) < 0) {
	if(errno != EADDRINUSE) {
	    flog(LOG_ERR, "could not listen on IPv4 port (port %i): %s", port, strerror(errno));
	    exit(1);
	}
    } else {
	omalloc(pd);
	pd->fd = fd;
	pd->sport = port;
	pd->creds = creds;
	pd->ncreds = ncreds.b;
	pd->ciphers = ciphers;
	bufadd(listeners, mustart(listenloop, pd));
    }
}

#endif
