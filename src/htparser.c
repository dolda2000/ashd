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
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <mt.h>
#include <log.h>
#include <req.h>

#define EV_READ 1
#define EV_WRITE 2

struct blocker {
    struct blocker *n, *p;
    int fd;
    int ev;
    struct muth *th;
};

static struct blocker *blockers;

static int block(int fd, int ev)
{
    struct blocker *bl;
    int rv;
    
    omalloc(bl);
    bl->fd = fd;
    bl->ev = ev;
    bl->th = current;
    bl->n = blockers;
    if(blockers)
	blockers->p = bl;
    blockers = bl;
    rv = yield();
    if(bl->n)
	bl->n->p = bl->p;
    if(bl->p)
	bl->p->n = bl->n;
    if(bl == blockers)
	blockers = bl->n;
    return(rv);
}

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
		    block(fd, EV_READ);
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
static struct htreq *parseraw(char *buf)
{
    char *p, *p2, *nl;
    char *method, *url, *ver;
    struct htreq *req;
    
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
	reqappheader(req, p, p2);
	p = nl;
    }
    return(req);
    
fail:
    freereq(req);
    return(NULL);
}

static void serve(struct muth *muth, va_list args)
{
    vavar(int, fd);
    struct charbuf buf;
    struct htreq *req;
    size_t headoff;
    
    bufinit(buf);
    while(1) {
	buf.d = 0;
	if((headoff = readhead(fd, &buf)) < 0)
	    goto out;
	if((req = parseraw(buf.b)) == NULL)
	    goto out;
	printf("\"%s\", \"%s\", \"%s\", \"%s\"\n", req->method, req->url, req->ver, getheader(req, "host"));
	freereq(req);
    }
    
out:
    buffree(buf);
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
	block(ss, EV_READ);
	ns = accept(ss, (struct sockaddr *)&name, &namelen);
	if(ns < 0) {
	    flog(LOG_ERR, "accept: %s", strerror(errno));
	    goto out;
	}
	mustart(serve, ns);
    }
    
out:
    close(ss);
}

static void ioloop(void)
{
    int ret;
    fd_set rfds, wfds, efds;
    struct blocker *bl, *nbl;
    int maxfd;
    int ev;
    
    while(blockers != NULL) {
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	maxfd = 0;
	for(bl = blockers; bl; bl = bl->n) {
	    if(bl->ev & EV_READ)
		FD_SET(bl->fd, &rfds);
	    if(bl->ev & EV_WRITE)
		FD_SET(bl->fd, &wfds);
	    FD_SET(bl->fd, &efds);
	    if(bl->fd > maxfd)
		maxfd = bl->fd;
	}
	ret = select(maxfd + 1, &rfds, &wfds, &efds, NULL);
	if(ret < 0) {
	    if(errno != EINTR) {
		flog(LOG_CRIT, "ioloop: select errored out: %s", strerror(errno));
		/* To avoid CPU hogging in case it's bad, which it
		 * probably is. */
		sleep(1);
	    }
	}
	for(bl = blockers; bl; bl = nbl) {
	    nbl = bl->n;
	    ev = 0;
	    if(FD_ISSET(bl->fd, &rfds))
		ev |= EV_READ;
	    if(FD_ISSET(bl->fd, &wfds))
		ev |= EV_WRITE;
	    if(FD_ISSET(bl->fd, &efds))
		ev = -1;
	    if(ev != 0)
		resume(bl->th, ev);
	}
    }
}

int main(int argc, char **argv)
{
    int fd;
    
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
    ioloop();
    return(0);
}
