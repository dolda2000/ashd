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
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <time.h>
#include <errno.h>
#include <sys/select.h>
#include <sys/socket.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <log.h>
#include <utils.h>
#include <mt.h>
#include <mtio.h>

static struct blocker *blockers;

#ifdef HAVE_EPOLL

/* 
 * Support for epoll. Optimally, different I/O loops should be split
 * into different files for greater clarity, but I'll save that fun
 * for another day. 
 *
 * Scroll down to #else for the normal select loop.
 */

#include <sys/epoll.h>

struct blocker {
    struct blocker *n, *p, *n2, *p2;
    int fd, reg;
    int ev;
    time_t to;
    struct muth *th;
};

static int epfd = -1, fdln = 0;
static struct blocker **fdlist;

static int regfd(struct blocker *bl)
{
    struct blocker *o;
    struct epoll_event evd;
    
    memset(&evd, 0, sizeof(evd));
    evd.events = 0;
    if(bl->ev & EV_READ)
	evd.events |= EPOLLIN;
    if(bl->ev & EV_WRITE)
	evd.events |= EPOLLOUT;
    evd.data.fd = bl->fd;
    if(bl->fd >= fdln) {
	if(fdlist) {
	    fdlist = srealloc(fdlist, sizeof(*fdlist) * (bl->fd + 1));
	    memset(fdlist + fdln, 0, sizeof(*fdlist) * (bl->fd + 1 - fdln));
	    fdln = bl->fd + 1;
	} else {
	    fdlist = szmalloc(sizeof(*fdlist) * (fdln = (bl->fd + 1)));
	}
    }
    if(fdlist[bl->fd] == NULL) {
	if(epoll_ctl(epfd, EPOLL_CTL_ADD, bl->fd, &evd)) {
	    /* XXX?! Whatever to do, really? */
	    flog(LOG_ERR, "epoll_add on fd %i: %s", bl->fd, strerror(errno));
	    return(-1);
	}
    } else {
	for(o = fdlist[bl->fd]; o; o = o->n2) {
	    if(o->ev & EV_READ)
		evd.events |= EPOLLIN;
	    if(o->ev & EV_WRITE)
		evd.events |= EPOLLOUT;
	}
	if(epoll_ctl(epfd, EPOLL_CTL_MOD, bl->fd, &evd)) {
	    /* XXX?! Whatever to do, really? */
	    flog(LOG_ERR, "epoll_mod on fd %i: %s", bl->fd, strerror(errno));
	    return(-1);
	}
    }
    bl->n2 = fdlist[bl->fd];
    bl->p2 = NULL;
    if(fdlist[bl->fd] != NULL)
	fdlist[bl->fd]->p2 = bl;
    fdlist[bl->fd] = bl;
    bl->reg = 1;
    return(0);
}

static void remfd(struct blocker *bl)
{
    struct blocker *o;
    struct epoll_event evd;
    
    if(!bl->reg)
	return;
    if(bl->n2)
	bl->n2->p2 = bl->p2;
    if(bl->p2)
	bl->p2->n2 = bl->n2;
    if(bl == fdlist[bl->fd])
	fdlist[bl->fd] = bl->n2;
    if(fdlist[bl->fd] == NULL) {
	if(epoll_ctl(epfd, EPOLL_CTL_DEL, bl->fd, NULL))
	    flog(LOG_ERR, "epoll_del on fd %i: %s", bl->fd, strerror(errno));
    } else {
	memset(&evd, 0, sizeof(evd));
	evd.events = 0;
	evd.data.fd = bl->fd;
	for(o = fdlist[bl->fd]; o; o = o->n2) {
	    if(o->ev & EV_READ)
		evd.events |= EPOLLIN;
	    if(o->ev & EV_WRITE)
		evd.events |= EPOLLOUT;
	}
	if(epoll_ctl(epfd, EPOLL_CTL_MOD, bl->fd, &evd)) {
	    /* XXX?! Whatever to do, really? */
	    flog(LOG_ERR, "epoll_mod on fd %i: %s", bl->fd, strerror(errno));
	}
    }
}

int block(int fd, int ev, time_t to)
{
    struct blocker *bl;
    int rv;
    
    omalloc(bl);
    bl->fd = fd;
    bl->ev = ev;
    if(to > 0)
	bl->to = time(NULL) + to;
    bl->th = current;
    if((epfd >= 0) && regfd(bl)) {
	free(bl);
	return(-1);
    }
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
    remfd(bl);
    free(bl);
    return(rv);
}

void ioloop(void)
{
    struct blocker *bl, *nbl;
    struct epoll_event evr[16];
    int i, fd, nev, ev, toval;
    time_t now, timeout;
    
    epfd = epoll_create(128);
    fcntl(epfd, F_SETFD, FD_CLOEXEC);
    for(bl = blockers; bl; bl = nbl) {
	nbl = bl->n;
	if(regfd(bl))
	    resume(bl->th, -1);
    }
    while(blockers != NULL) {
	timeout = 0;
	for(bl = blockers; bl; bl = bl->n) {
	    if((bl->to != 0) && ((timeout == 0) || (timeout > bl->to)))
		timeout = bl->to;
	}
	now = time(NULL);
	if(timeout == 0)
	    toval = -1;
	else if(timeout > now)
	    toval = (timeout - now) * 1000;
	else
	    toval = 1000;
	nev = epoll_wait(epfd, evr, sizeof(evr) / sizeof(*evr), toval);
	if(nev < 0) {
	    if(errno != EINTR) {
		flog(LOG_CRIT, "ioloop: select errored out: %s", strerror(errno));
		/* To avoid CPU hogging in case it's bad, which it
		 * probably is. */
		sleep(1);
	    }
	    continue;
	}
	for(i = 0; i < nev; i++) {
	    fd = evr[i].data.fd;
	    ev = 0;
	    if(evr[i].events & EPOLLIN)
		ev |= EV_READ;
	    if(evr[i].events & EPOLLOUT)
		ev |= EV_WRITE;
	    if(evr[i].events & ~(EPOLLIN | EPOLLOUT))
		ev = -1;
	    for(bl = fdlist[fd]; bl; bl = nbl) {
		nbl = bl->n2;
		if((ev < 0) || (ev & bl->ev))
		    resume(bl->th, ev);
	    }
	}
	now = time(NULL);
	for(bl = blockers; bl; bl = nbl) {
	    nbl = bl->n;
	    if((bl->to != 0) && (bl->to <= now))
		resume(bl->th, 0);
	}
    }
    close(epfd);
    epfd = -1;
}

#else

struct blocker {
    struct blocker *n, *p;
    int fd;
    int ev;
    time_t to;
    struct muth *th;
};

int block(int fd, int ev, time_t to)
{
    struct blocker *bl;
    int rv;
    
    omalloc(bl);
    bl->fd = fd;
    bl->ev = ev;
    if(to > 0)
	bl->to = time(NULL) + to;
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
    free(bl);
    return(rv);
}

void ioloop(void)
{
    int ret;
    fd_set rfds, wfds, efds;
    struct blocker *bl, *nbl;
    struct timeval toval;
    time_t now, timeout;
    int maxfd;
    int ev;
    
    while(blockers != NULL) {
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	maxfd = 0;
	now = time(NULL);
	timeout = 0;
	for(bl = blockers; bl; bl = bl->n) {
	    if(bl->ev & EV_READ)
		FD_SET(bl->fd, &rfds);
	    if(bl->ev & EV_WRITE)
		FD_SET(bl->fd, &wfds);
	    FD_SET(bl->fd, &efds);
	    if(bl->fd > maxfd)
		maxfd = bl->fd;
	    if((bl->to != 0) && ((timeout == 0) || (timeout > bl->to)))
		timeout = bl->to;
	}
	toval.tv_sec = timeout - now;
	toval.tv_usec = 0;
	ret = select(maxfd + 1, &rfds, &wfds, &efds, timeout?(&toval):NULL);
	if(ret < 0) {
	    if(errno != EINTR) {
		flog(LOG_CRIT, "ioloop: select errored out: %s", strerror(errno));
		/* To avoid CPU hogging in case it's bad, which it
		 * probably is. */
		sleep(1);
	    }
	}
	now = time(NULL);
	for(bl = blockers; bl; bl = nbl) {
	    nbl = bl->n;
	    ev = 0;
	    if(FD_ISSET(bl->fd, &rfds))
		ev |= EV_READ;
	    if(FD_ISSET(bl->fd, &wfds))
		ev |= EV_WRITE;
	    if(FD_ISSET(bl->fd, &efds))
		ev = -1;
	    if((ev < 0) || (ev & bl->ev))
		resume(bl->th, ev);
	    else if((bl->to != 0) && (bl->to <= now))
		resume(bl->th, 0);
	}
    }
}

#endif

struct stdiofd {
    int fd;
    int sock;
    int timeout;
};

static ssize_t mtread(void *cookie, char *buf, size_t len)
{
    struct stdiofd *d = cookie;
    int ev;
    ssize_t ret;
    
    while(1) {
	ret = read(d->fd, buf, len);
	if((ret < 0) && (errno == EAGAIN)) {
	    ev = block(d->fd, EV_READ, d->timeout);
	    if(ev < 0) {
		/* If we just go on, we should get the real error. */
		continue;
	    } else if(ev == 0) {
		errno = ETIMEDOUT;
		return(-1);
	    } else {
		continue;
	    }
	} else {
	    return(ret);
	}
    }
}

static ssize_t mtwrite(void *cookie, const char *buf, size_t len)
{
    struct stdiofd *d = cookie;
    int ev;
    size_t off;
    ssize_t ret;
    
    off = 0;
    while(off < len) {
	if(d->sock)
	    ret = send(d->fd, buf + off, len - off, MSG_DONTWAIT | MSG_NOSIGNAL);
	else
	    ret = write(d->fd, buf + off, len - off);
	if(ret < 0) {
	    if(errno == EAGAIN) {
		ev = block(d->fd, EV_WRITE, d->timeout);
		if(ev < 0) {
		    /* If we just go on, we should get the real error. */
		    continue;
		} else if(ev == 0) {
		    errno = ETIMEDOUT;
		    return(off);
		} else {
		    continue;
		}
	    } else {
		return(off);
	    }
	} else {
	    off += ret;
	}
    }
    return(off);
}

static int mtclose(void *cookie)
{
    struct stdiofd *d = cookie;
    
    close(d->fd);
    free(d);
    return(0);
}

static cookie_io_functions_t iofuns = {
    .read = mtread,
    .write = mtwrite,
    .close = mtclose,
};

FILE *mtstdopen(int fd, int issock, int timeout, char *mode)
{
    struct stdiofd *d;
    FILE *ret;
    
    omalloc(d);
    d->fd = fd;
    d->sock = issock;
    d->timeout = timeout;
    ret = fopencookie(d, mode, iofuns);
    if(!ret)
	free(d);
    else
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return(ret);
}
