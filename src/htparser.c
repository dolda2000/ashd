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

static size_t readhead(int fd, struct charbuf *buf)
{
    int nl;
    size_t off;
    
    int get1(void)
    {
	int ret;
	
	while(!(off < buf->d)) {
	    sizebuf(*buf, buf->d + 1024);
	    ret = recv(fd, buf->b + buf->d, buf->s - buf->d, MSG_DONTWAIT);
	    if(ret <= 0) {
		if((ret < 0) && (errno == EAGAIN)) {
		    if(block(fd, EV_READ, 60) <= 0)
			return(-1);
		    continue;
		}
		return(-1);
	    }
	    buf->d += ret;
	}
	return(buf->b[off++]);
    }

    nl = 0;
    off = 0;
    while(1) {
	switch(get1()) {
	case '\n':
	    if(nl)
		return(off);
	    nl = 1;
	    break;
	case '\r':
	    break;
	case -1:
	    return(-1);
	default:
	    nl = 0;
	    break;
	}
    }
}

#define SKIPNL(ptr) ({				\
	    int __buf__;			\
	    if(*(ptr) == '\r')			\
		*((ptr)++) = 0;			\
	    if(*(ptr) != '\n') {		\
		__buf__ = 0;			\
	    } else {				\
		*((ptr)++) = 0;			\
		__buf__ = 1;			\
	    }					\
	    __buf__;})
static struct hthead *parserawreq(char *buf)
{
    char *p, *p2, *nl;
    char *method, *url, *ver;
    struct hthead *req;
    
    if((nl = strchr(buf, '\n')) == NULL)
	return(NULL);
    if(((p = strchr(buf, ' ')) == NULL) || (p > nl))
	return(NULL);
    method = buf;
    *(p++) = 0;
    if(((p2 = strchr(p, ' ')) == NULL) || (p2 > nl))
	return(NULL);
    url = p;
    p = p2;
    *(p++) = 0;
    if(strncmp(p, "HTTP/", 5))
	return(NULL);
    ver = (p += 5);
    for(; ((*p >= '0') && (*p <= '9')) || (*p == '.'); p++);
    if(!SKIPNL(p))
	return(NULL);

    req = mkreq(method, url, ver);
    while(1) {
	if(SKIPNL(p)) {
	    if(*p)
		goto fail;
	    break;
	}
	if((nl = strchr(p, '\n')) == NULL)
	    goto fail;
	if(((p2 = strchr(p, ':')) == NULL) || (p2 > nl))
	    goto fail;
	*(p2++) = 0;
	for(; (*p2 == ' ') || (*p2 == '\t'); p2++);
	for(nl = p2; (*nl != '\r') && (*nl != '\n'); nl++);
	if(!SKIPNL(nl))
	    goto fail;
	if(strncasecmp(p, "x-ash-", 6))
	    headappheader(req, p, p2);
	p = nl;
    }
    return(req);
    
fail:
    freehthead(req);
    return(NULL);
}

static struct hthead *parserawresp(char *buf)
{
    char *p, *p2, *nl;
    char *msg, *ver;
    int code;
    struct hthead *resp;
    
    if((nl = strchr(buf, '\n')) == NULL)
	return(NULL);
    p = strchr(buf, '\r');
    if((p != NULL) && (p < nl))
	nl = p;
    if(strncmp(buf, "HTTP/", 5))
	return(NULL);
    ver = p = buf + 5;
    for(; ((*p >= '0') && (*p <= '9')) || (*p == '.'); p++);
    if(*p != ' ')
	return(NULL);
    *(p++) = 0;
    if(((p2 = strchr(p, ' ')) == NULL) || (p2 > nl))
	return(NULL);
    *(p2++) = 0;
    code = atoi(p);
    if((code < 100) || (code >= 600))
	return(NULL);
    if(p2 >= nl)
	return(NULL);
    msg = p2;
    p = nl;
    if(!SKIPNL(p))
	return(NULL);

    resp = mkresp(code, msg, ver);
    while(1) {
	if(SKIPNL(p)) {
	    if(*p)
		goto fail;
	    break;
	}
	if((nl = strchr(p, '\n')) == NULL)
	    goto fail;
	if(((p2 = strchr(p, ':')) == NULL) || (p2 > nl))
	    goto fail;
	*(p2++) = 0;
	for(; (*p2 == ' ') || (*p2 == '\t'); p2++);
	for(nl = p2; (*nl != '\r') && (*nl != '\n'); nl++);
	if(!SKIPNL(nl))
	    goto fail;
	headappheader(resp, p, p2);
	p = nl;
    }
    return(resp);
    
fail:
    freehthead(resp);
    return(NULL);
}

static off_t passdata(int src, int dst, struct charbuf *buf, off_t max)
{
    size_t dataoff, smax;
    off_t sent;
    int eof, ret;

    sent = 0;
    eof = 0;
    while((!eof || (buf->d > 0)) && ((max < 0) || (sent < max))) {
	if(!eof && (buf->d < buf->s) && ((max < 0) || (sent + buf->d < max))) {
	    while(1) {
		ret = recv(src, buf->b + buf->d, buf->s - buf->d, MSG_DONTWAIT);
		if((ret < 0) && (errno == EAGAIN)) {
		} else if(ret < 0) {
		    return(-1);
		} else if(ret == 0) {
		    eof = 1;
		    break;
		} else {
		    buf->d += ret;
		    break;
		}
		if(buf->d > 0)
		    break;
		if(block(src, EV_READ, 0) <= 0)
		    return(-1);
	    }
	}
	for(dataoff = 0; (dataoff < buf->d) && ((max < 0) || (sent < max));) {
	    if(block(dst, EV_WRITE, 120) <= 0)
		return(-1);
	    smax = buf->d - dataoff;
	    if(sent + smax > max)
		smax = max - sent;
	    ret = send(dst, buf->b + dataoff, smax, MSG_NOSIGNAL | MSG_DONTWAIT);
	    if(ret < 0)
		return(-1);
	    dataoff += ret;
	    sent += ret;
	}
	bufeat(*buf, dataoff);
    }
    return(sent);
}

static void serve(struct muth *muth, va_list args)
{
    vavar(int, fd);
    vavar(struct sockaddr_storage, name);
    int cfd;
    int pfds[2];
    char old;
    char *hd, *p;
    struct charbuf inbuf, outbuf;
    struct hthead *req, *resp;
    off_t dlen, sent;
    ssize_t headoff;
    char nmbuf[256];
    
    bufinit(inbuf);
    bufinit(outbuf);
    cfd = -1;
    req = resp = NULL;
    while(1) {
	/*
	 * First, find and decode the header:
	 */
	if((headoff = readhead(fd, &inbuf)) < 0)
	    goto out;
	if(headoff > 65536) {
	    /* We cannot handle arbitrarily large headers, as they
	     * need to fit within a single Unix datagram. This is
	     * probably a safe limit, and larger packets than this are
	     * most likely erroneous (or malicious) anyway. */
	    goto out;
	}
	old = inbuf.b[headoff];
	inbuf.b[headoff] = 0;
	if((req = parserawreq(inbuf.b)) == NULL)
	    goto out;
	inbuf.b[headoff] = old;
	bufeat(inbuf, headoff);
	/* We strip off the leading slash and any param string from
	 * the rest string, so that multiplexers can parse
	 * coherently. */
	if(req->rest[0] == '/')
	    replrest(req, req->rest + 1);
	if((p = strchr(req->rest, '?')) != NULL)
	    *p = 0;
	
	/*
	 * Add metainformation and then send the request to the root
	 * multiplexer:
	 */
	if(name.ss_family == AF_INET) {
	    headappheader(req, "X-Ash-Address", inet_ntop(AF_INET, &((struct sockaddr_in *)&name)->sin_addr, nmbuf, sizeof(nmbuf)));
	    headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in *)&name)->sin_port)));
	} else if(name.ss_family == AF_INET6) {
	    headappheader(req, "X-Ash-Address", inet_ntop(AF_INET6, &((struct sockaddr_in6 *)&name)->sin6_addr, nmbuf, sizeof(nmbuf)));
	    headappheader(req, "X-Ash-Port", sprintf3("%i", ntohs(((struct sockaddr_in6 *)&name)->sin6_port)));
	}
	if(block(plex, EV_WRITE, 60) <= 0)
	    goto out;
	if(socketpair(PF_UNIX, SOCK_STREAM, 0, pfds))
	    goto out;
	if(sendreq(plex, req, pfds[0]))
	    goto out;
	close(pfds[0]);
	cfd = pfds[1];

	/*
	 * If there is message data, pass it:
	 */
	if((hd = getheader(req, "content-length")) != NULL) {
	    dlen = atoo(hd);
	    if(dlen > 0) {
		if(passdata(fd, cfd, &inbuf, dlen) < 0)
		    goto out;
	    }
	}
	/* Make sure to send EOF */
	shutdown(cfd, SHUT_WR);
	
	/*
	 * Find and decode the response header:
	 */
	outbuf.d = 0;
	if((headoff = readhead(cfd, &outbuf)) < 0)
	    goto out;
	hd = memcpy(smalloc(headoff + 1), outbuf.b, headoff);
	hd[headoff] = 0;
	if((resp = parserawresp(hd)) == NULL)
	    goto out;
	
	/*
	 * Pass the actual output:
	 */
	sizebuf(outbuf, 65536);
	if((sent = passdata(cfd, fd, &outbuf, -1)) < 0)
	    goto out;
	sent -= headoff;
	
	/*
	 * Check for connection expiry
	 */
	if(strcasecmp(req->method, "head")) {
	    if((hd = getheader(resp, "content-length")) != NULL) {
		if(sent != atoo(hd)) {
		    /* Exit because of error */
		    goto out;
		}
	    } else {
		if(((hd = getheader(resp, "transfer-encoding")) == NULL) || !strcasecmp(hd, "identity"))
		    break;
	    }
	    if(((hd = getheader(req, "connection")) != NULL) && !strcasecmp(hd, "close"))
		break;
	    if(((hd = getheader(resp, "connection")) != NULL) && !strcasecmp(hd, "close"))
		break;
	}
	
	close(cfd);
	cfd = -1;
	freehthead(req);
	req = NULL;
	freehthead(resp);
	resp = NULL;
    }
    
out:
    if(cfd >= 0)
	close(cfd);
    if(req != NULL)
	freehthead(req);
    if(resp != NULL)
	freehthead(resp);
    buffree(inbuf);
    buffree(outbuf);
    close(fd);
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
