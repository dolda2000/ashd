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
#include <stdio.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <pwd.h>
#include <sys/signal.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <mt.h>
#include <mtio.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <bufio.h>

#include "htparser.h"

static int plex;
static int daemonize, usesyslog;
struct mtbuf listeners;

static void trimx(struct hthead *req)
{
    int i;
    
    i = 0;
    while(i < req->noheaders) {
	if(!strncasecmp(req->headers[i][0], "x-ash-", 6)) {
	    free(req->headers[i][0]);
	    free(req->headers[i][1]);
	    free(req->headers[i]);
	    memmove(req->headers + i, req->headers + i + 1, sizeof(*req->headers) * (--req->noheaders - i));
	} else {
	    i++;
	}
    }
}

static struct hthead *parsereq(struct bufio *in)
{
    struct hthead *req;
    struct charbuf method, url, ver;
    int c;
    
    req = NULL;
    bufinit(method);
    bufinit(url);
    bufinit(ver);
    while(1) {
	c = biogetc(in);
	if(c == ' ') {
	    break;
	} else if((c == EOF) || (c < 32) || (c >= 128)) {
	    goto fail;
	} else {
	    bufadd(method, c);
	    if(method.d >= 128)
		goto fail;
	}
    }
    while(1) {
	c = biogetc(in);
	if(c == ' ') {
	    break;
	} else if((c == EOF) || (c < 32)) {
	    goto fail;
	} else {
	    bufadd(url, c);
	    if(url.d >= 65536)
		goto fail;
	}
    }
    while(1) {
	c = biogetc(in);
	if(c == 10) {
	    break;
	} else if(c == 13) {
	} else if((c == EOF) || (c < 32) || (c >= 128)) {
	    goto fail;
	} else {
	    bufadd(ver, c);
	    if(ver.d >= 128)
		goto fail;
	}
    }
    bufadd(method, 0);
    bufadd(url, 0);
    bufadd(ver, 0);
    req = mkreq(method.b, url.b, ver.b);
    if(parseheadersb(req, in))
	goto fail;
    trimx(req);
    goto out;
    
fail:
    if(req != NULL) {
	freehthead(req);
	req = NULL;
    }
out:
    buffree(method);
    buffree(url);
    buffree(ver);
    return(req);
}

static off_t passdata(struct bufio *in, struct bufio *out, off_t max)
{
    ssize_t read;
    off_t total;
    
    total = 0;
    while(!bioeof(in) && ((max < 0) || (total < max))) {
	if((read = biordata(in)) > 0) {
	    if(max >= 0)
		read = min(max - total, read);
	    if((read = biowritesome(out, in->rbuf.b + in->rh, read)) < 0)
		return(-1);
	    in->rh += read;
	    total += read;
	}
	if(biorspace(in) && ((max < 0) || (biordata(in) < max - total)) && (biofillsome(in) < 0))
	    return(-1);
    }
    return(total);
}

static int recvchunks(struct bufio *in, struct bufio *out)
{
    ssize_t read, chlen;
    int c, r;
    
    while(1) {
	chlen = 0;
	r = 0;
	while(1) {
	    c = biogetc(in);
	    if(c == 10) {
		if(!r)
		    return(-1);
		break;
	    } else if(c == 13) {
	    } else if((c >= '0') && (c <= '9')) {
		chlen = (chlen << 4) + (c - '0');
		r = 1;
	    } else if((c >= 'A') && (c <= 'F')) {
		chlen = (chlen << 4) + (c + 10 - 'A');
		r = 1;
	    } else if((c >= 'a') && (c <= 'f')) {
		chlen = (chlen << 4) + (c + 10 - 'a');
		r = 1;
	    } else {
		/* XXX: Technically, there may be chunk extensions to
		 * be read, but since that will likely never actually
		 * happen in practice, I can just as well add support
		 * for that if it actually does become relevant. */
		return(-1);
	    }
	}
	if(chlen == 0)
	    break;
	while(chlen > 0) {
	    if((read = biordata(in)) > 0) {
		if((read = biowritesome(out, in->rbuf.b + in->rh, min(read, chlen))) < 0)
		    return(-1);
		in->rh += read;
		chlen -= read;
	    }
	    if(biorspace(in) && (biordata(in) < chlen) && (biofillsome(in) <= 0))
		return(-1);
	}
	if((biogetc(in) != 13) || (biogetc(in) != 10))
	    return(-1);
    }
    /* XXX: Technically, there may be trailers to be read, but that's
     * just about as likely as chunk extensions. */
    if((biogetc(in) != 13) || (biogetc(in) != 10))
	return(-1);
    return(0);
}

static int passchunks(struct bufio *in, struct bufio *out)
{
    size_t read;
    
    while(!bioeof(in)) {
	if((read = biordata(in)) > 0) {
	    bioprintf(out, "%zx\r\n", read);
	    if(biowrite(out, in->rbuf.b + in->rh, read) != read)
		return(-1);
	    in->rh += read;
	    bioprintf(out, "\r\n");
	    if(bioflush(out) < 0)
		return(-1);
	}
	if(biorspace(in) && (biofillsome(in) < 0))
	    return(-1);
    }
    bioprintf(out, "0\r\n\r\n");
    return(0);
}

static int hasheader(struct hthead *head, char *name, char *val)
{
    char *hd;
    
    if((hd = getheader(head, name)) == NULL)
	return(0);
    return(!strcasecmp(hd, val));
}

static int canonreq(struct hthead *req)
{
    char *p, *p2, *r;
    int n;

    if(req->url[0] == '/') {
	replrest(req, req->url + 1);
	if((p = strchr(req->rest, '?')) != NULL)
	    *p = 0;
	return(1);
    }
    if((p = strstr(req->url, "://")) != NULL) {
	n = p - req->url;
	if(((n == 4) && !strncasecmp(req->url, "http", 4)) ||
	   ((n == 5) && !strncasecmp(req->url, "https", 5))) {
	    if(getheader(req, "host"))
		return(0);
	    p += 3;
	    if((p2 = strchr(p, '/')) == NULL) {
		headappheader(req, "Host", p);
		free(req->url);
		req->url = sstrdup("/");
	    } else {
		r = sstrdup(p2);
		*(p2++) = 0;
		headappheader(req, "Host", p);
		free(req->url);
		req->url = r;
	    }
	    replrest(req, req->url + 1);
	    if((p = strchr(req->rest, '?')) != NULL)
		*p = 0;
	    return(1);
	}
    }
    return(0);
}

static int http10keep(struct hthead *req, struct hthead *resp)
{
    int fc;
    
    fc = hasheader(resp, "connection", "close");
    headrmheader(resp, "connection");
    if(!fc && hasheader(req, "connection", "keep-alive")) {
	headappheader(resp, "Connection", "Keep-Alive");
	return(1);
    } else {
	return(0);
    }
}

static char *connid(void)
{
    static struct charbuf cur;
    int i;
    char *ret;
    
    for(i = 0; i < cur.d; i++) {
	if((++cur.b[i]) > 'Z')
	    cur.b[i] = 'A';
	else
	    goto done;
    }
    bufadd(cur, 'A');
done:
    ret = memcpy(smalloc(cur.d + 1), cur.b, cur.d);
    ret[cur.d] = 0;
    return(ret);
}

static void passduplex(struct bufio *a, int afd, struct bufio *b, int bfd)
{
    struct selected pfd[4], sel;
    struct bufio *sio;
    int n, ev;
    
    while(!bioeof(a) && !bioeof(b)) {
	biocopybuf(b, a);
	biocopybuf(a, b);
	n = 0;
	if(!a->eof) {
	    ev = 0;
	    if(biorspace(a))
		ev |= EV_READ;
	    if(biowdata(a))
		ev |= EV_WRITE;
	    if(ev)
		pfd[n++] = (struct selected){.fd = afd, .ev = ev};
	}
	if(!b->eof) {
	    ev = 0;
	    if(!b->eof && biorspace(b))
		ev |= EV_READ;
	    if(biowdata(b))
		ev |= EV_WRITE;
	    if(ev)
		pfd[n++] = (struct selected){.fd = bfd, .ev = ev};
	}
	if((sel = mblock(600, n, pfd)).ev == 0)
	    break;
	if(sel.fd == afd)
	    sio = a;
	else if(sel.fd == bfd)
	    sio = b;
	else
	    break;
	if((sel.ev & EV_READ) && (biofillsome(sio) < 0))
	    break;
	if((sel.ev & EV_WRITE) && (bioflushsome(sio) < 0))
	    break;
    }
}

void serve(struct bufio *in, int infd, struct conn *conn)
{
    int pfds[2];
    struct bufio *out, *dout;
    struct stdiofd *outi;
    struct hthead *req, *resp;
    char *hd, *id;
    off_t dlen;
    int keep, duplex;
    
    id = connid();
    out = NULL;
    req = resp = NULL;
    while(plex >= 0) {
	bioflush(in);
	if((req = parsereq(in)) == NULL)
	    break;
	if(!canonreq(req))
	    break;
	
	headappheader(req, "X-Ash-Connection-ID", id);
	if((conn->initreq != NULL) && conn->initreq(conn, req))
	    break;
	
	if((plex < 0) || block(plex, EV_WRITE, 60) <= 0)
	    break;
	if(socketpair(PF_UNIX, SOCK_STREAM, 0, pfds))
	    break;
	if(sendreq(plex, req, pfds[0]))
	    break;
	close(pfds[0]);
	out = mtbioopen(pfds[1], 1, 600, "r+", &outi);

	if(getheader(req, "content-type") != NULL) {
	    if((hd = getheader(req, "content-length")) != NULL) {
		dlen = atoo(hd);
		if(dlen > 0) {
		    if(passdata(in, out, dlen) != dlen)
			break;
		}
	    } else if(((hd = getheader(req, "transfer-encoding")) != NULL) && !strcasecmp(hd, "chunked")) {
		if(recvchunks(in, out))
		    break;
	    } else {
		/* Ignore rather than abort, to be kinder to broken clients. */
		headrmheader(req, "content-type");
	    }
	}
	if(bioflush(out))
	    break;
	/* Make sure to send EOF */
	shutdown(pfds[1], SHUT_WR);
	
	if((resp = parseresponseb(out)) == NULL)
	    break;
	replstr(&resp->ver, req->ver);
	
	if(!getheader(resp, "server"))
	    headappheader(resp, "Server", sprintf3("ashd/%s", VERSION));
	duplex = hasheader(resp, "x-ash-switch", "duplex");
	trimx(resp);

	if(duplex) {
	    if(outi->rights < 0)
		break;
	    writerespb(in, resp);
	    bioprintf(in, "\r\n");
	    dout = mtbioopen(outi->rights, 1, 600, "r+", NULL);
	    passduplex(in, infd, dout, outi->rights);
	    outi->rights = -1;
	    bioclose(dout);
	    break;
	} else if(!strcasecmp(req->ver, "HTTP/1.0")) {
	    if(!strcasecmp(req->method, "head")) {
		keep = http10keep(req, resp);
		writerespb(in, resp);
		bioprintf(in, "\r\n");
	    } else if((hd = getheader(resp, "content-length")) != NULL) {
		keep = http10keep(req, resp);
		dlen = atoo(hd);
		writerespb(in, resp);
		bioprintf(in, "\r\n");
		if(passdata(out, in, dlen) != dlen)
		    break;
	    } else {
		headrmheader(resp, "connection");
		writerespb(in, resp);
		bioprintf(in, "\r\n");
		passdata(out, in, -1);
		break;
	    }
	    if(!keep)
		break;
	} else if(!strcasecmp(req->ver, "HTTP/1.1")) {
	    if(!strcasecmp(req->method, "head")) {
		writerespb(in, resp);
		bioprintf(in, "\r\n");
	    } else if((hd = getheader(resp, "content-length")) != NULL) {
		writerespb(in, resp);
		bioprintf(in, "\r\n");
		dlen = atoo(hd);
		if(passdata(out, in, dlen) != dlen)
		    break;
	    } else if(!getheader(resp, "transfer-encoding")) {
		headappheader(resp, "Transfer-Encoding", "chunked");
		writerespb(in, resp);
		bioprintf(in, "\r\n");
		if(passchunks(out, in))
		    break;
	    } else {
		writerespb(in, resp);
		bioprintf(in, "\r\n");
		passdata(out, in, -1);
		break;
	    }
	    if(hasheader(req, "connection", "close") || hasheader(resp, "connection", "close"))
		break;
	} else {
	    break;
	}

	bioclose(out);
	out = NULL;
	freehthead(req);
	freehthead(resp);
	req = resp = NULL;
    }
    
    if(out != NULL)
	bioclose(out);
    if(req != NULL)
	freehthead(req);
    if(resp != NULL)
	freehthead(resp);
    bioclose(in);
    free(id);
}

static void plexwatch(struct muth *muth, va_list args)
{
    vavar(int, fd);
    char *buf;
    int i, s, ret;
    
    s = 0;
    while(1) {
	if(block(fd, EV_READ, 0) == 0)
	    break;
	buf = smalloc(65536);
	ret = recv(fd, buf, 65536, 0);
	if(ret < 0) {
	    flog(LOG_WARNING, "received error on rootplex read channel: %s", strerror(errno));
	    exit(1);
	} else if(ret == 0) {
	    s = 1;
	    free(buf);
	    break;
	}
	/* Maybe I'd like to implement some protocol in this direction
	 * some day... */
	free(buf);
    }
    shutdown(plex, SHUT_RDWR);
    for(i = 0; i < listeners.d; i++) {
	if(listeners.b[i] == muth)
	    bufdel(listeners, i);
    }
    if(s) {
	flog(LOG_INFO, "root handler exited, so shutting down listening...");
	while(listeners.d > 0)
	    resume(listeners.b[0], 0);
    }
}

static void initroot(void *uu)
{
    int fd;
    
    setsid();
    if(daemonize) {
	chdir("/");
	if((fd = open("/dev/null", O_RDWR)) >= 0) {
	    dup2(fd, 0);
	    dup2(fd, 1);
	    dup2(fd, 2);
	    close(fd);
	}
    }
    if(usesyslog)
	putenv("ASHD_USESYSLOG=1");
    else
	unsetenv("ASHD_USESYSLOG");
}

static void usage(FILE *out)
{
    fprintf(out, "usage: htparser [-hSf] [-u USER] [-r ROOT] [-p PIDFILE] PORTSPEC... -- ROOT [ARGS...]\n");
    fprintf(out, "\twhere PORTSPEC is HANDLER[:PAR[=VAL][(,PAR[=VAL])...]] (try HANDLER:help)\n");
    fprintf(out, "\tavailable handlers are `plain' and `ssl'.\n");
}

static void addport(char *spec)
{
    char *nm, *p, *p2, *n;
    struct charvbuf pars, vals;
    
    bufinit(pars);
    bufinit(vals);
    if((p = strchr(spec, ':')) == NULL) {
	nm = spec;
    } else {
	nm = spec;
	*(p++) = 0;
	do {
	    if((n = strchr(p, ',')) != NULL)
		*(n++) = 0;
	    if((p2 = strchr(p, '=')) != NULL)
		*(p2++) = 0;
	    if(!*p) {
		usage(stderr);
		exit(1);
	    }
	    bufadd(pars, p);
	    if(p2)
		bufadd(vals, p2);
	    else
		bufadd(vals, "");
	} while((p = n) != NULL);
    }
    
    /* XXX: It would be nice to decentralize this, but, meh... */
    if(!strcmp(nm, "plain")) {
	handleplain(pars.d, pars.b, vals.b);
#ifdef HAVE_GNUTLS
    } else if(!strcmp(nm, "ssl")) {
	handlegnussl(pars.d, pars.b, vals.b);
#endif
    } else {
	flog(LOG_ERR, "htparser: unknown port handler `%s'", nm);
	exit(1);
    }
    
    buffree(pars);
    buffree(vals);
}

static void sighandler(int sig)
{
    exitioloop(1);
}

int main(int argc, char **argv)
{
    int c, d;
    int i, s1;
    char *root, *pidfile, *pidtmp;
    FILE *pidout;
    struct passwd *pwent;
    
    daemonize = usesyslog = 0;
    root = pidfile = NULL;
    pwent = NULL;
    while((c = getopt(argc, argv, "+hSfu:r:p:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'f':
	    daemonize = 1;
	    break;
	case 'S':
	    usesyslog = 1;
	    break;
	case 'u':
	    if(optarg[0] && ((pwent = getpwnam(optarg)) == NULL)) {
		flog(LOG_ERR, "could not find user %s", optarg);
		exit(1);
	    }
	    break;
	case 'r':
	    root = optarg[0] ? optarg : NULL;
	    break;
	case 'p':
	    pidfile = optarg[0] ? optarg : NULL;
	    break;
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    s1 = 0;
    for(i = optind; i < argc; i++) {
	if(!strcmp(argv[i], "--"))
	    break;
	s1 = 1;
	addport(argv[i]);
    }
    if(!s1 || (i == argc)) {
	usage(stderr);
	exit(1);
    }
    if((plex = stdmkchild(argv + ++i, initroot, NULL)) < 0) {
	flog(LOG_ERR, "could not spawn root multiplexer: %s", strerror(errno));
	return(1);
    }
    bufadd(listeners, mustart(plexwatch, plex));
    pidout = NULL;
    if(pidfile != NULL) {
	pidtmp = sprintf3("%s.new", pidfile);
	if((pidout = fopen(pidtmp, "w")) == NULL) {
	    flog(LOG_ERR, "could not open %s for writing: %s", pidtmp, strerror(errno));
	    return(1);
	}
	if(rename(pidtmp, pidfile)) {
	    flog(LOG_ERR, "could not overwrite %s: %s", pidfile, strerror(errno));
	    unlink(pidtmp);
	    return(1);
	}
    }
    if(usesyslog)
	opensyslog();
    if(root) {
	if(chdir(root) || chroot(root)) {
	    flog(LOG_ERR, "could not chroot to %s: %s", root, strerror(errno));
	    exit(1);
	}
    }
    if(pwent) {
	if(setgid(pwent->pw_gid)) {
	    flog(LOG_ERR, "could not switch group to %i: %s", (int)pwent->pw_gid, strerror(errno));
	    exit(1);
	}
	if(setuid(pwent->pw_uid)) {
	    flog(LOG_ERR, "could not switch user to %i: %s", (int)pwent->pw_uid, strerror(errno));
	    exit(1);
	}
    }
    signal(SIGPIPE, SIG_IGN);
    signal(SIGCHLD, SIG_IGN);
    signal(SIGINT, sighandler);
    signal(SIGTERM, sighandler);
    if(daemonize) {
	daemon(0, 0);
    }
    if(pidout != NULL) {
	fprintf(pidout, "%i\n", getpid());
	fflush(pidout);
    }
    d = 0;
    while(!d) {
	switch(ioloop()) {
	case 0:
	    d = 1;
	    break;
	case 1:
	    if(listeners.d > 0) {
		while(listeners.d > 0)
		    resume(listeners.b[0], 0);
		flog(LOG_INFO, "no longer listening");
		if(pidout != NULL) {
		    putc('\n', pidout);
		    fflush(pidout);
		}
	    } else {
		d = 1;
	    }
	    break;
	}
    }
    if(pidout != NULL)
	ftruncate(fileno(pidout), 0);
    return(0);
}
