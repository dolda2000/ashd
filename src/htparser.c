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
	block(ns, EV_WRITE);
	write(ns, "test\n", 5);
	close(ns);
    }
}

static void ioloop(void)
{
    int ret;
    fd_set rfds, wfds, efds;
    struct blocker *bl, *nbl;
    int maxfd;
    int ev;
    
    while(1) {
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
