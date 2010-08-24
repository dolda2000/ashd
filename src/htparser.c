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
#include <string.h>
#include <sys/socket.h>
#include <pwd.h>
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

#include "htparser.h"

static int plex;

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

static struct hthead *parsereq(FILE *in)
{
    struct hthead *req;
    struct charbuf method, url, ver;
    int c;
    
    req = NULL;
    bufinit(method);
    bufinit(url);
    bufinit(ver);
    while(1) {
	c = getc(in);
	if(c == ' ') {
	    break;
	} else if((c == EOF) || (c < 32) || (c >= 128)) {
	    goto fail;
	} else {
	    bufadd(method, c);
	}
    }
    while(1) {
	c = getc(in);
	if(c == ' ') {
	    break;
	} else if((c == EOF) || (c < 32)) {
	    goto fail;
	} else {
	    bufadd(url, c);
	}
    }
    while(1) {
	c = getc(in);
	if(c == 10) {
	    break;
	} else if(c == 13) {
	} else if((c == EOF) || (c < 32) || (c >= 128)) {
	    goto fail;
	} else {
	    bufadd(ver, c);
	}
    }
    bufadd(method, 0);
    bufadd(url, 0);
    bufadd(ver, 0);
    req = mkreq(method.b, url.b, ver.b);
    if(parseheaders(req, in))
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

static struct hthead *parseresp(FILE *in)
{
    struct hthead *req;
    int code;
    struct charbuf ver, msg;
    int c;
    
    req = NULL;
    bufinit(ver);
    bufinit(msg);
    code = 0;
    while(1) {
	c = getc(in);
	if(c == ' ') {
	    break;
	} else if((c == EOF) || (c < 32) || (c >= 128)) {
	    goto fail;
	} else {
	    bufadd(ver, c);
	}
    }
    while(1) {
	c = getc(in);
	if(c == ' ') {
	    break;
	} else if((c == EOF) || (c < '0') || (c > '9')) {
	    goto fail;
	} else {
	    code = (code * 10) + (c - '0');
	}
    }
    while(1) {
	c = getc(in);
	if(c == 10) {
	    break;
	} else if(c == 13) {
	} else if((c == EOF) || (c < 32)) {
	    goto fail;
	} else {
	    bufadd(msg, c);
	}
    }
    bufadd(msg, 0);
    bufadd(ver, 0);
    req = mkresp(code, msg.b, ver.b);
    if(parseheaders(req, in))
	goto fail;
    goto out;
    
fail:
    if(req != NULL) {
	freehthead(req);
	req = NULL;
    }
out:
    buffree(msg);
    buffree(ver);
    return(req);
}

static off_t passdata(FILE *in, FILE *out, off_t max)
{
    size_t read;
    off_t total;
    char buf[8192];
    
    total = 0;
    while(!feof(in) && ((max < 0) || (total < max))) {
	read = sizeof(buf);
	if(max >= 0)
	    read = min(max - total, read);
	read = fread(buf, 1, read, in);
	if(ferror(in))
	    return(-1);
	if(fwrite(buf, 1, read, out) != read)
	    return(-1);
	total += read;
    }
    return(total);
}

static int passchunks(FILE *in, FILE *out)
{
    char buf[8192];
    size_t read;
    
    do {
	read = fread(buf, 1, sizeof(buf), in);
	if(ferror(in))
	    return(-1);
	fprintf(out, "%zx\r\n", read);
	if(fwrite(buf, 1, read, out) != read)
	    return(-1);
	fprintf(out, "\r\n");
    } while(read > 0);
    return(0);
}

static int hasheader(struct hthead *head, char *name, char *val)
{
    char *hd;
    
    if((hd = getheader(head, name)) == NULL)
	return(0);
    return(!strcasecmp(hd, val));
}

void serve(FILE *in, struct conn *conn)
{
    int pfds[2];
    FILE *out;
    struct hthead *req, *resp;
    char *hd, *p;
    off_t dlen;
    
    out = NULL;
    req = resp = NULL;
    while(1) {
	if((req = parsereq(in)) == NULL)
	    break;
	replrest(req, req->url);
	if(req->rest[0] == '/')
	    replrest(req, req->rest + 1);
	if((p = strchr(req->rest, '?')) != NULL)
	    *p = 0;
	
	if((conn->initreq != NULL) && conn->initreq(conn, req))
	    break;
	
	if(block(plex, EV_WRITE, 60) <= 0)
	    break;
	if(socketpair(PF_UNIX, SOCK_STREAM, 0, pfds))
	    break;
	if(sendreq(plex, req, pfds[0]))
	    break;
	close(pfds[0]);
	out = mtstdopen(pfds[1], 1, 600, "r+");

	if((hd = getheader(req, "content-length")) != NULL) {
	    dlen = atoo(hd);
	    if(dlen > 0) {
		if(passdata(in, out, dlen) != dlen)
		    break;
	    }
	}
	if(fflush(out))
	    break;
	/* Make sure to send EOF */
	shutdown(pfds[1], SHUT_WR);
	
	if((resp = parseresp(out)) == NULL)
	    break;
	replstr(&resp->ver, req->ver);

	if(!strcmp(req->ver, "HTTP/1.0")) {
	    writeresp(in, resp);
	    fprintf(in, "\r\n");
	    if((hd = getheader(resp, "content-length")) != NULL) {
		dlen = passdata(out, in, -1);
		if(dlen != atoo(hd))
		    break;
		if(!hasheader(req, "connection", "keep-alive"))
		    break;
	    } else {
		passdata(out, in, -1);
		break;
	    }
	    if(hasheader(req, "connection", "close") || hasheader(resp, "connection", "close"))
		break;
	} else if(!strcmp(req->ver, "HTTP/1.1")) {
	    if((hd = getheader(resp, "content-length")) != NULL) {
		writeresp(in, resp);
		fprintf(in, "\r\n");
		dlen = passdata(out, in, -1);
		if(dlen != atoo(hd))
		    break;
	    } else if(!getheader(resp, "transfer-encoding")) {
		headappheader(resp, "Transfer-Encoding", "chunked");
		writeresp(in, resp);
		fprintf(in, "\r\n");
		if(passchunks(out, in))
		    break;
	    } else {
		writeresp(in, resp);
		fprintf(in, "\r\n");
		passdata(out, in, -1);
		break;
	    }
	    if(hasheader(req, "connection", "close") || hasheader(resp, "connection", "close"))
		break;
	} else {
	    break;
	}

	fclose(out);
	out = NULL;
	freehthead(req);
	freehthead(resp);
	req = resp = NULL;
    }
    
    if(out != NULL)
	fclose(out);
    if(req != NULL)
	freehthead(req);
    if(resp != NULL)
	freehthead(resp);
    fclose(in);
}

static void plexwatch(struct muth *muth, va_list args)
{
    vavar(int, fd);
    char *buf;
    int ret;
    
    while(1) {
	block(fd, EV_READ, 0);
	buf = smalloc(65536);
	ret = recv(fd, buf, 65536, 0);
	if(ret < 0) {
	    flog(LOG_WARNING, "received error on rootplex read channel: %s", strerror(errno));
	    exit(1);
	} else if(ret == 0) {
	    exit(0);
	}
	/* Maybe I'd like to implement some protocol in this direction
	 * some day... */
	free(buf);
    }
}

static void usage(FILE *out)
{
    fprintf(out, "usage: htparser [-hSf] [-u USER] [-r ROOT] PORTSPEC... -- ROOT [ARGS...]\n");
    fprintf(out, "\twhere PORTSPEC is HANDLER[:PAR[=VAL][(,PAR[=VAL])...]] (try HANDLER:help)\n");
    fprintf(out, "\tavailable handlers are `plain'.\n");
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
    } else {
	flog(LOG_ERR, "htparser: unknown port handler `%s'", nm);
	exit(1);
    }
    
    buffree(pars);
    buffree(vals);
}

int main(int argc, char **argv)
{
    int c;
    int i, s1;
    int daemonize, logsys;
    char *root;
    struct passwd *pwent;
    
    daemonize = logsys = 0;
    root = NULL;
    pwent = NULL;
    while((c = getopt(argc, argv, "+hSfu:r:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'f':
	    daemonize = 1;
	    break;
	case 'S':
	    logsys = 1;
	    break;
	case 'u':
	    if((pwent = getpwnam(optarg)) == NULL) {
		flog(LOG_ERR, "could not find user %s", optarg);
		exit(1);
	    }
	    break;
	case 'r':
	    root = optarg;
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
    if((plex = stdmkchild(argv + ++i)) < 0) {
	flog(LOG_ERR, "could not spawn root multiplexer: %s", strerror(errno));
	return(1);
    }
    mustart(plexwatch, plex);
    if(logsys)
	opensyslog();
    if(root) {
	if(chroot(root)) {
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
    if(daemonize) {
	daemon(0, 0);
    }
    ioloop();
    return(0);
}
