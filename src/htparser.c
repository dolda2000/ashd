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
#include <netinet/in.h>
#include <arpa/inet.h>
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

static int plex;

static int listensock4(int port)
{
    struct sockaddr_in name;
    int fd;
    int valbuf;
    
    memset(&name, 0, sizeof(name));
    name.sin_family = AF_INET;
    name.sin_port = htons(port);
    if((fd = socket(PF_INET, SOCK_STREAM, 0)) < 0)
	return(-1);
    valbuf = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &valbuf, sizeof(valbuf));
    if(bind(fd, (struct sockaddr *)&name, sizeof(name))) {
	close(fd);
	return(-1);
    }
    if(listen(fd, 16) < 0) {
	close(fd);
	return(-1);
    }
    return(fd);
}

static int listensock6(int port)
{
    struct sockaddr_in6 name;
    int fd;
    int valbuf;
    
    memset(&name, 0, sizeof(name));
    name.sin6_family = AF_INET6;
    name.sin6_port = htons(port);
    if((fd = socket(PF_INET6, SOCK_STREAM, 0)) < 0)
	return(-1);
    valbuf = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &valbuf, sizeof(valbuf));
    if(bind(fd, (struct sockaddr *)&name, sizeof(name))) {
	close(fd);
	return(-1);
    }
    if(listen(fd, 16) < 0) {
	close(fd);
	return(-1);
    }
    return(fd);
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
    while(!feof(in)) {
	read = sizeof(buf);
	if(max >= 0)
	    read = max(max - total, read);
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
	fprintf(out, "%x\r\n", read);
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

static void serve(struct muth *muth, va_list args)
{
    vavar(int, fd);
    vavar(struct sockaddr_storage, name);
    int pfds[2];
    FILE *in, *out;
    struct hthead *req, *resp;
    char nmbuf[256];
    char *hd, *p;
    off_t dlen;
    
    in = mtstdopen(fd, 1, 60, "r+");
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
	
	if(name.ss_family == AF_INET) {
	    headappheader(req, "X-Ash-Address", inet_ntop(AF_INET, &((struct sockaddr_in *)&name)->sin_addr, nmbuf, sizeof(nmbuf)));
	    headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in *)&name)->sin_port)));
	} else if(name.ss_family == AF_INET6) {
	    headappheader(req, "X-Ash-Address", inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&name)->sin6_addr, nmbuf, sizeof(nmbuf)));
	    headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in6 *)&name)->sin6_port)));
	}

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
	
	resp = parseresp(out);
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

static void listenloop(struct muth *muth, va_list args)
{
    vavar(int, ss);
    int ns;
    struct sockaddr_storage name;
    socklen_t namelen;
    
    while(1) {
	namelen = sizeof(name);
	block(ss, EV_READ, 0);
	ns = accept(ss, (struct sockaddr *)&name, &namelen);
	if(ns < 0) {
	    flog(LOG_ERR, "accept: %s", strerror(errno));
	    goto out;
	}
	mustart(serve, ns, name);
    }
    
out:
    close(ss);
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

int main(int argc, char **argv)
{
    int fd;
    
    if(argc < 2) {
	fprintf(stderr, "usage: htparser ROOT [ARGS...]\n");
	exit(1);
    }
    if((plex = stdmkchild(argv + 1)) < 0) {
	flog(LOG_ERR, "could not spawn root multiplexer: %s", strerror(errno));
	return(1);
    }
    if((fd = listensock6(8080)) < 0) {
	flog(LOG_ERR, "could not listen on IPv6: %s", strerror(errno));
	return(1);
    }
    mustart(listenloop, fd);
    if((fd = listensock4(8080)) < 0) {
	if(errno != EADDRINUSE) {
	    flog(LOG_ERR, "could not listen on IPv4: %s", strerror(errno));
	    return(1);
	}
    } else {
	mustart(listenloop, fd);
    }
    mustart(plexwatch, plex);
    ioloop();
    return(0);
}
