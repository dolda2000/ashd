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

/*
 * XXX: This program is mostly copied from callscgi. It may be
 * reasonable to unify some of their shared code in a source file.
 */

/*
 * XXX: All the various ways to start a child process makes this
 * program quite ugly at the moment. It is unclear whether it is
 * meaningfully possible to unify them better than they currently are.
 */

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <netdb.h>
#include <signal.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <req.h>
#include <resp.h>
#include <log.h>
#include <mt.h>
#include <mtio.h>

#define FCGI_BEGIN_REQUEST 1
#define FCGI_ABORT_REQUEST 2
#define FCGI_END_REQUEST 3
#define FCGI_PARAMS 4
#define FCGI_STDIN 5
#define FCGI_STDOUT 6
#define FCGI_STDERR 7

static char **progspec;
static char *sockid, *unspec, *inspec;
static int nolisten;
static struct sockaddr *curaddr;
static size_t caddrlen;
static int cafamily, isanon;
static pid_t child;

static struct addrinfo *resolv(int flags)
{
    int ret;
    struct addrinfo *ai, h;
    char *name, *srv, *p;
    
    if((p = strchr(inspec, ':')) != NULL) {
	name = smalloc(p - inspec + 1);
	memcpy(name, inspec, p - inspec);
	name[p - inspec] = 0;
	srv = p + 1;
    } else {
	name = sstrdup("localhost");
	srv = inspec;
    }
    memset(&h, 0, sizeof(h));
    h.ai_family = AF_UNSPEC;
    h.ai_socktype = SOCK_STREAM;
    h.ai_flags = flags;
    ret = getaddrinfo(name, srv, &h, &ai);
    free(name);
    if(ret != 0) {
	flog(LOG_ERR, "could not resolve TCP specification `%s': %s", inspec, gai_strerror(ret));
	exit(1);
    }
    return(ai);
}

static char *mksockid(char *sockid)
{
    char *home;
    
    home = getenv("HOME");
    if(home && !access(sprintf3("%s/.ashd/sockets/", home), X_OK))
	return(sprintf3("%s/.ashd/sockets/fcgi-p-%s", home, sockid));
    return(sprintf3("/tmp/fcgi-%i-%s", getuid(), sockid));
}

static char *mkanonid(void)
{
    char *home;
    char *tmpl;
    int fd;
    
    home = getenv("HOME");
    if(home && !access(sprintf3("%s/.ashd/sockets/", home), X_OK))
	tmpl = sprintf2("%s/.ashd/sockets/fcgi-a-XXXXXX", home);
    else
	tmpl = sprintf2("/tmp/fcgi-a-%i-XXXXXX", getuid());
    if((fd = mkstemp(tmpl)) < 0) {
	flog(LOG_ERR, "could not create anonymous socket `%s': %s", tmpl, strerror(errno));
	exit(1);
    }
    close(fd);
    unlink(tmpl);
    return(tmpl);
}

static void setupchild(void)
{
    /* PHP appears to not expect to inherit SIGCHLD set to SIG_IGN, so
     * reset it for it. */
    signal(SIGCHLD, SIG_DFL);
}

static void startlisten(void)
{
    int fd;
    struct addrinfo *ai, *cai;
    char *unpath;
    struct sockaddr_un unm;
    char *aname;
    
    isanon = 0;
    if(inspec != NULL) {
	fd = -1;
	for(cai = ai = resolv(AI_PASSIVE); cai != NULL; cai = cai->ai_next) {
	    if((fd = socket(cai->ai_family, cai->ai_socktype, cai->ai_protocol)) < 0)
		continue;
	    if(bind(fd, cai->ai_addr, cai->ai_addrlen)) {
		close(fd);
		fd = -1;
		continue;
	    }
	    if(listen(fd, 128)) {
		close(fd);
		fd = -1;
		continue;
	    }
	    break;
	}
	freeaddrinfo(ai);
	if(fd < 0) {
	    flog(LOG_ERR, "could not bind to specified TCP address: %s", strerror(errno));
	    exit(1);
	}
    } else if((unspec != NULL) || (sockid != NULL)) {
	if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	    flog(LOG_ERR, "could not create Unix socket: %s", strerror(errno));
	    exit(1);
	}
	if(unspec != NULL)
	    unpath = unspec;
	else
	    unpath = mksockid(sockid);
	unlink(unpath);
	unm.sun_family = AF_UNIX;
	strcpy(unm.sun_path, unpath);
	if(bind(fd, (struct sockaddr *)&unm, sizeof(unm))) {
	    flog(LOG_ERR, "could not bind Unix socket to `%s': %s", unspec, strerror(errno));
	    exit(1);
	}
	if(listen(fd, 128)) {
	    flog(LOG_ERR, "listen: %s", strerror(errno));
	    exit(1);
	}
    } else {
	if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	    flog(LOG_ERR, "could not create Unix socket: %s", strerror(errno));
	    exit(1);
	}
	memset(&unm, 0, sizeof(unm));
	aname = mkanonid();
	unm.sun_family = AF_UNIX;
	strcpy(unm.sun_path, aname);
	free(aname);
	if(bind(fd, (struct sockaddr *)&unm, sizeof(unm))) {
	    flog(LOG_ERR, "could not bind Unix socket to `%s': %s", unspec, strerror(errno));
	    exit(1);
	}
	if(listen(fd, 128)) {
	    flog(LOG_ERR, "listen: %s", strerror(errno));
	    exit(1);
	}
	
	curaddr = smalloc(caddrlen = sizeof(unm));
	memcpy(curaddr, &unm, sizeof(unm));
	cafamily = AF_UNIX;
	isanon = 1;
    }
    if((child = fork()) < 0) {
	flog(LOG_ERR, "could not fork: %s", strerror(errno));
	exit(1);
    }
    if(child == 0) {
	setupchild();
	dup2(fd, 0);
	close(fd);
	execvp(*progspec, progspec);
	flog(LOG_ERR, "callfcgi: %s: %s", *progspec, strerror(errno));
	_exit(127);
    }
    close(fd);
}

static void startnolisten(void)
{
    int fd;
    
    if((child = fork()) < 0) {
	flog(LOG_ERR, "could not fork: %s", strerror(errno));
	exit(1);
    }
    if(child == 0) {
	setupchild();
	if((fd = open("/dev/null", O_RDONLY)) < 0) {
	    flog(LOG_ERR, "/dev/null: %s", strerror(errno));
	    _exit(127);
	}
	dup2(fd, 0);
	close(fd);
	execvp(*progspec, progspec);
	flog(LOG_ERR, "callfcgi: %s: %s", *progspec, strerror(errno));
	_exit(127);
    }
}

static int sconnect(void)
{
    int fd;
    int err;
    socklen_t errlen;

    fd = socket(cafamily, SOCK_STREAM, 0);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    while(1) {
	if(!connect(fd, curaddr, caddrlen))
	    return(fd);
	if(errno == EAGAIN) {
	    block(-1, 0, 1);
	    continue;
	}
	if(errno == EINPROGRESS) {
	    block(fd, EV_WRITE, 30);
	    errlen = sizeof(err);
	    if(getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &errlen) || ((errno = err) != 0)) {
		close(fd);
		return(-1);
	    }
	    return(fd);
	}
	close(fd);
	return(-1);
    }
}

static int econnect(void)
{
    int fd;
    struct addrinfo *ai, *cai;
    int tries;
    char *unpath;
    struct sockaddr_un unm;
    
    tries = 0;
retry:
    if(inspec != NULL) {
	fd = -1;
	for(cai = ai = resolv(0); cai != NULL; cai = cai->ai_next) {
	    if((fd = socket(cai->ai_family, cai->ai_socktype, cai->ai_protocol)) < 0)
		continue;
	    if(connect(fd, cai->ai_addr, cai->ai_addrlen)) {
		close(fd);
		fd = -1;
		continue;
	    }
	    break;
	}
	if(fd < 0) {
	    if(tries++ < nolisten) {
		sleep(1);
		goto retry;
	    }
	    flog(LOG_ERR, "could not connect to specified TCP address: %s", strerror(errno));
	    exit(1);
	}
	curaddr = smalloc(caddrlen = cai->ai_addrlen);
	memcpy(curaddr, cai->ai_addr, caddrlen);
	cafamily = cai->ai_family;
	isanon = 0;
	freeaddrinfo(ai);
	return(fd);
    } else if((unspec != NULL) || (sockid != NULL)) {
	if((fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
	    flog(LOG_ERR, "could not create Unix socket: %s", strerror(errno));
	    exit(1);
	}
	if(unspec != NULL)
	    unpath = unspec;
	else
	    unpath = mksockid(sockid);
	unlink(unpath);
	unm.sun_family = AF_UNIX;
	strcpy(unm.sun_path, unpath);
	if(connect(fd, (struct sockaddr *)&unm, sizeof(unm))) {
	    close(fd);
	    if(tries++ < nolisten) {
		sleep(1);
		goto retry;
	    }
	    flog(LOG_ERR, "could not connect to Unix socket `%s': %s", unspec, strerror(errno));
	    exit(1);
	}
	curaddr = smalloc(caddrlen = sizeof(unm));
	memcpy(curaddr, &unm, sizeof(unm));
	cafamily = AF_UNIX;
	isanon = 0;
	return(fd);
    } else {
	flog(LOG_ERR, "callfcgi: cannot use an anonymous socket without a program to start");
	exit(1);
    }
}

static int startconn(void)
{
    if(*progspec) {
	if(nolisten == 0)
	    startlisten();
	else
	    startnolisten();
    }
    if(curaddr != NULL)
	return(sconnect());
    return(econnect());
}

static void killcuraddr(void)
{
    if(curaddr == NULL)
	return;
    if(isanon) {
	unlink(((struct sockaddr_un *)curaddr)->sun_path);
	if(child > 0)
	    kill(child, SIGTERM);
    }
    free(curaddr);
    curaddr = NULL;
}

static int reconn(void)
{
    int fd;
    
    if(curaddr != NULL) {
	if((fd = sconnect()) >= 0)
	    return(fd);
	killcuraddr();
    }
    return(startconn());
}

static off_t passdata(FILE *in, FILE *out)
{
    size_t read;
    off_t total;
    char buf[8192];
    
    total = 0;
    while(!feof(in)) {
	read = fread(buf, 1, sizeof(buf), in);
	if(ferror(in))
	    return(-1);
	if(fwrite(buf, 1, read, out) != read)
	    return(-1);
	total += read;
    }
    return(total);
}

static void bufcatkv(struct charbuf *dst, char *key, char *val)
{
    size_t kl, vl;
    
    if((kl = strlen(key)) < 128) {
	bufadd(*dst, kl);
    } else {
	bufadd(*dst, ((kl & 0x7f000000) >> 24) | 0x80);
	bufadd(*dst, (kl & 0x00ff0000) >> 16);
	bufadd(*dst, (kl & 0x0000ff00) >> 8);
	bufadd(*dst, kl & 0x000000ff);
    }
    if((vl = strlen(val)) < 128) {
	bufadd(*dst, vl);
    } else {
	bufadd(*dst, ((vl & 0x7f000000) >> 24) | 0x80);
	bufadd(*dst, (vl & 0x00ff0000) >> 16);
	bufadd(*dst, (vl & 0x0000ff00) >> 8);
	bufadd(*dst, vl & 0x000000ff);
    }
    bufcat(*dst, key, kl);
    bufcat(*dst, val, vl);
}

static void bufaddenv(struct charbuf *dst, char *name, char *fmt, ...)
{
    va_list args;
    char *val = NULL;
    
    va_start(args, fmt);
    val = vsprintf2(fmt, args);
    va_end(args);
    bufcatkv(dst, name, val);
    free(val);
}

static char *absolutify(char *file)
{
    static int inited = 0;
    static char cwd[1024];
    
    if(*file != '/') {
	if(!inited) {
	    getcwd(cwd, sizeof(cwd));
	    inited = 1;
	}
	return(sprintf2("%s/%s", cwd, file));
    }
    return(sstrdup(file));
}

/* Mostly copied from callcgi. */
static void mkcgienv(struct hthead *req, struct charbuf *dst)
{
    int i;
    char *url, *pi, *tmp, *qp, *h, *p;
    
    bufaddenv(dst, "SERVER_SOFTWARE", "ashd/%s", VERSION);
    bufaddenv(dst, "GATEWAY_INTERFACE", "CGI/1.1");
    bufaddenv(dst, "SERVER_PROTOCOL", "%s", req->ver);
    bufaddenv(dst, "REQUEST_METHOD", "%s", req->method);
    bufaddenv(dst, "REQUEST_URI", "%s", req->url);
    url = sstrdup(req->url);
    if((qp = strchr(url, '?')) != NULL)
	*(qp++) = 0;
    /* XXX: This is an ugly hack (I think), but though I can think of
     * several alternatives, none seem to be better. */
    if(*req->rest && (strlen(url) >= strlen(req->rest)) &&
       !strcmp(req->rest, url + strlen(url) - strlen(req->rest))) {
	url[strlen(url) - strlen(req->rest)] = 0;
    }
    if((pi = unquoteurl(req->rest)) == NULL)
	pi = sstrdup(req->rest);
    if(!strcmp(url, "/")) {
	/* This seems to be normal CGI behavior, but see callcgi.c for
	 * details. */
	url[0] = 0;
	pi = sprintf2("/%s", tmp = pi);
	free(tmp);
    }
    bufaddenv(dst, "PATH_INFO", "%s", pi);
    bufaddenv(dst, "SCRIPT_NAME", "%s", url);
    bufaddenv(dst, "QUERY_STRING", "%s", qp?qp:"");
    free(pi);
    free(url);
    if((h = getheader(req, "Host")) != NULL)
	bufaddenv(dst, "SERVER_NAME", "%s", h);
    if((h = getheader(req, "X-Ash-Server-Address")) != NULL)
	bufaddenv(dst, "SERVER_ADDR", "%s", h);
    if((h = getheader(req, "X-Ash-Server-Port")) != NULL)
	bufaddenv(dst, "SERVER_PORT", "%s", h);
    if((h = getheader(req, "X-Ash-Remote-User")) != NULL)
	bufaddenv(dst, "REMOTE_USER", "%s", h);
    if(((h = getheader(req, "X-Ash-Protocol")) != NULL) && !strcmp(h, "https"))
	bufaddenv(dst, "HTTPS", "on");
    if((h = getheader(req, "X-Ash-Address")) != NULL)
	bufaddenv(dst, "REMOTE_ADDR", "%s", h);
    if((h = getheader(req, "X-Ash-Port")) != NULL)
	bufaddenv(dst, "REMOTE_PORT", "%s", h);
    if((h = getheader(req, "Content-Type")) != NULL)
	bufaddenv(dst, "CONTENT_TYPE", "%s", h);
    if((h = getheader(req, "Content-Length")) != NULL)
	bufaddenv(dst, "CONTENT_LENGTH", "%s", h);
    else
	bufaddenv(dst, "CONTENT_LENGTH", "0");
    if((h = getheader(req, "X-Ash-File")) != NULL) {
	h = absolutify(h);
	bufaddenv(dst, "SCRIPT_FILENAME", "%s", h);
	free(h);
    }
    for(i = 0; i < req->noheaders; i++) {
	h = sprintf2("HTTP_%s", req->headers[i][0]);
	for(p = h; *p; p++) {
	    if(isalnum(*p))
		*p = toupper(*p);
	    else
		*p = '_';
	}
	bufcatkv(dst, h, req->headers[i][1]);
	free(h);
    }
}

static struct hthead *parseresp(FILE *in)
{
    struct hthead *resp;
    char *st, *p;
    
    omalloc(resp);
    resp->ver = sstrdup("HTTP/1.1");
    if(parseheaders(resp, in)) {
	freehthead(resp);
	return(NULL);
    }
    if((st = getheader(resp, "Status")) != NULL) {
	if((p = strchr(st, ' ')) != NULL) {
	    *(p++) = 0;
	    resp->code = atoi(st);
	    resp->msg = sstrdup(p);
	} else {
	    resp->code = atoi(st);
	    resp->msg = sstrdup(httpdefstatus(resp->code));
	}
	headrmheader(resp, "Status");
    } else if(getheader(resp, "Location")) {
	resp->code = 303;
	resp->msg = sstrdup("See Other");
    } else {
	resp->code = 200;
	resp->msg = sstrdup("OK");
    }
    return(resp);
}

#define fputc2(b, f) if(fputc((b), (f)) == EOF) return(-1);

static int sendrec(FILE *out, int type, int rid, char *data, size_t dlen)
{
    off_t off;
    size_t cl;
    int p;
    
    off = 0;
    do {
	cl = min(dlen - off, 65535);
	p = (8 - (cl % 8)) % 8;
	fputc2(1, out);
	fputc2(type, out);
	fputc2((rid & 0xff00) >> 8, out);
	fputc2(rid & 0x00ff, out);
	fputc2((cl & 0xff00) >> 8, out);
	fputc2(cl & 0x00ff, out);
	fputc2(p, out);
	fputc2(0, out);
	if(fwrite(data + off, 1, cl, out) != cl)
	    return(-1);
	for(; p > 0; p--)
	    fputc2(0, out);
    } while((off += cl) < dlen);
    return(0);
}

static int recvrec(FILE *in, int *type, int *rid, char **data, size_t *dlen)
{
    unsigned char header[8];
    int tl;
    
    if(fread(header, 1, 8, in) != 8)
	return(-1);
    if(header[0] != 1)
	return(-1);
    *type = header[1];
    *rid  = (header[2] << 8) | header[3];
    *dlen = (header[4] << 8) | header[5];
    tl = *dlen + header[6];
    if(header[7] != 0)
	return(-1);
    *data = smalloc(max(tl, 1));
    if(fread(*data, 1, tl, in) != tl) {
	free(*data);
	return(-1);
    }
    return(0);
}

static int begreq(FILE *out, int rid)
{
    char rec[] = {0, 1, 0, 0, 0, 0, 0, 0};
    
    return(sendrec(out, FCGI_BEGIN_REQUEST, rid, rec, 8));
}

static void outplex(struct muth *muth, va_list args)
{
    vavar(FILE *, sk);
    struct {
	struct ch {
	    FILE *s;
	    int id;
	} *b;
	size_t s, d;
    } outs;
    int i;
    struct ch ch;
    int type, rid;
    char *data;
    size_t dlen;
    
    bufinit(outs);
    while((ch.s = va_arg(args, FILE *)) != NULL) {
	ch.id = va_arg(args, int);
	bufadd(outs, ch);
    }
    data = NULL;
    while(1) {
	if(recvrec(sk, &type, &rid, &data, &dlen))
	    goto out;
	if(rid != 1)
	    goto out;
	for(i = 0; i < outs.d; i++) {
	    if(outs.b[i].id == type) {
		if(outs.b[i].s != NULL) {
		    if(dlen == 0) {
			fclose(outs.b[i].s);
			outs.b[i].s = NULL;
		    } else {
			if(fwrite(data, 1, dlen, outs.b[i].s) != dlen)
			    goto out;
		    }
		}
		break;
	    }
	}
	free(data);
	data = NULL;
    }

out:
    if(data != NULL)
	free(data);
    for(i = 0; i < outs.d; i++) {
	if(outs.b[i].s != NULL)
	    fclose(outs.b[i].s);
    }
    buffree(outs);
    fclose(sk);
}

static void errhandler(struct muth *muth, va_list args)
{
    vavar(FILE *, in);
    char buf[1024];
    char *p;
    
    bufinit(buf);
    while(fgets(buf, sizeof(buf), in) != NULL) {
	p = buf + strlen(buf) - 1;
	while((p >= buf) && (*p == '\n'))
	    *(p--) = 0;
	if(buf[0])
	    flog(LOG_INFO, "child said: %s", buf);
    }
    fclose(in);
}

static void serve(struct muth *muth, va_list args)
{
    vavar(struct hthead *, req);
    vavar(int, fd);
    vavar(int, sfd);
    FILE *is, *os, *outi, *outo, *erri, *erro;
    struct charbuf head;
    struct hthead *resp;
    size_t read;
    char buf[8192];
    
    is = mtstdopen(fd, 1, 60, "r+", NULL);
    os = mtstdopen(sfd, 1, 600, "r+", NULL);
    
    outi = NULL;
    mtiopipe(&outi, &outo); mtiopipe(&erri, &erro);
    mustart(outplex, mtstdopen(dup(sfd), 1, 600, "r+", NULL), outo, FCGI_STDOUT, erro, FCGI_STDERR, NULL);
    mustart(errhandler, erri);
    
    if(begreq(os, 1))
	goto out;
    bufinit(head);
    mkcgienv(req, &head);
    if(sendrec(os, FCGI_PARAMS, 1, head.b, head.d))
	goto out;
    if(sendrec(os, FCGI_PARAMS, 1, NULL, 0))
	goto out;
    buffree(head);
    if(fflush(os))
	goto out;
    
    while(!feof(is)) {
	read = fread(buf, 1, sizeof(buf), is);
	if(ferror(is))
	    goto out;
	if(sendrec(os, FCGI_STDIN, 1, buf, read))
	    goto out;
    }
    if(sendrec(os, FCGI_STDIN, 1, NULL, 0))
	goto out;
    if(fflush(os))
	goto out;
    
    if((resp = parseresp(outi)) == NULL)
	goto out;
    writeresp(is, resp);
    freehthead(resp);
    fputc('\n', is);
    if(passdata(outi, is) < 0)
	goto out;
    
out:
    freehthead(req);
    buffree(head);
    shutdown(sfd, SHUT_RDWR);
    if(outi != NULL)
	fclose(outi);
    fclose(is);
    fclose(os);
}

static void listenloop(struct muth *muth, va_list args)
{
    vavar(int, lfd);
    int fd, sfd;
    struct hthead *req;
    
    while(1) {
	block(0, EV_READ, 0);
	if((fd = recvreq(lfd, &req)) < 0) {
	    if(errno != 0)
		flog(LOG_ERR, "recvreq: %s", strerror(errno));
	    break;
	}
	if((sfd = reconn()) < 0) {
	    close(fd);
	    freehthead(req);
	    continue;
	}
	mustart(serve, req, fd, sfd);
    }
}

static void sigign(int sig)
{
}

static void sigexit(int sig)
{
    shutdown(0, SHUT_RDWR);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: callfcgi [-h] [-N RETRIES] [-i ID] [-u UNIX-PATH] [-t [HOST:]TCP-PORT] [PROGRAM [ARGS...]]\n");
}

int main(int argc, char **argv)
{
    int c;
    
    while((c = getopt(argc, argv, "+hN:i:u:t:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'N':
	    nolisten = atoi(optarg);
	    break;
	case 'i':
	    sockid = optarg;
	    break;
	case 'u':
	    unspec = optarg;
	    break;
	case 't':
	    inspec = optarg;
	    break;
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    progspec = argv + optind;
    if(((sockid != NULL) + (unspec != NULL) + (inspec != NULL)) > 1) {
	flog(LOG_ERR, "callfcgi: at most one of -i, -u or -t may be given");
	exit(1);
    }
    signal(SIGCHLD, SIG_IGN);
    signal(SIGPIPE, sigign);
    signal(SIGINT, sigexit);
    signal(SIGTERM, sigexit);
    mustart(listenloop, 0);
    ioloop();
    killcuraddr();
    return(0);
}
