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
#include <time.h>
#include <fcntl.h>
#include <string.h>
#include <sys/epoll.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <log.h>
#include <utils.h>
#include <mt.h>
#include <mtio.h>

static struct blocker *blockers;

struct blocker {
    struct blocker *n, *p, *n2, *p2;
    int fd, reg;
    int ev;
    int thpos;
    time_t to;
    struct muth *th;
};

struct timeentry {
    time_t to;
    struct blocker *bl;
};

static int epfd = -1, fdln = 0;
static int exitstatus;
static struct blocker **fdlist;
static typedbuf(struct timeentry) timeheap;

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
    bl->reg = 0;
}

static void thraise(struct timeentry ent, int n)
{
    int p;
    
    while(n > 0) {
	p = (n - 1) >> 1;
	if(timeheap.b[p].to <= ent.to)
	    break;
	timeheap.b[n] = timeheap.b[p];
	timeheap.b[n].bl->thpos = n;
	n = p;
    }
    timeheap.b[n] = ent;
    ent.bl->thpos = n;
}

static void thlower(struct timeentry ent, int n)
{
    int c;
    
    while(1) {
	c = (n << 1) + 1;
	if(c >= timeheap.d)
	    break;
	if((c + 1 < timeheap.d) && (timeheap.b[c + 1].to < timeheap.b[c].to))
	    c = c + 1;
	if(timeheap.b[c].to > ent.to)
	    break;
	timeheap.b[n] = timeheap.b[c];
	timeheap.b[n].bl->thpos = n;
	n = c;
    }
    timeheap.b[n] = ent;
    ent.bl->thpos = n;
}

static void addtimeout(struct blocker *bl, time_t to)
{
    sizebuf(timeheap, ++timeheap.d);
    thraise((struct timeentry){.to = to, .bl = bl}, timeheap.d - 1);
}

static void deltimeout(struct blocker *bl)
{
    struct timeentry ent;
    int n;
    
    if(bl->thpos == timeheap.d - 1) {
	timeheap.d--;
	return;
    }
    n = bl->thpos;
    ent = timeheap.b[--timeheap.d];
    if((n > 0) && (timeheap.b[(n - 1) >> 1].to > ent.to))
	thraise(ent, n);
    else
	thlower(ent, n);
}

int block(int fd, int ev, time_t to)
{
    struct blocker *bl;
    int rv;
    
    omalloc(bl);
    bl->fd = fd;
    bl->ev = ev;
    bl->th = current;
    if((epfd >= 0) && regfd(bl)) {
	free(bl);
	return(-1);
    }
    bl->n = blockers;
    if(blockers)
	blockers->p = bl;
    blockers = bl;
    if(to > 0)
	addtimeout(bl, bl->to = (time(NULL) + to));
    rv = yield();
    if(bl->to > 0)
	deltimeout(bl);
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

int ioloop(void)
{
    struct blocker *bl, *nbl;
    struct epoll_event evr[16];
    int i, fd, nev, ev, toval;
    time_t now;
    
    exitstatus = 0;
    epfd = epoll_create(128);
    fcntl(epfd, F_SETFD, FD_CLOEXEC);
    bufinit(timeheap);
    for(bl = blockers; bl; bl = nbl) {
	nbl = bl->n;
	if(regfd(bl))
	    resume(bl->th, -1);
    }
    while(blockers != NULL) {
	now = time(NULL);
	if(timeheap.d == 0)
	    toval = -1;
	else if(timeheap.b[0].to > now)
	    toval = (timeheap.b[0].to - now) * 1000;
	else
	    toval = 1000;
	if(exitstatus)
	    break;
	nev = epoll_wait(epfd, evr, sizeof(evr) / sizeof(*evr), toval);
	if(nev < 0) {
	    if(errno != EINTR) {
		flog(LOG_CRIT, "ioloop: epoll_wait errored out: %s", strerror(errno));
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
	while((timeheap.d > 0) && (timeheap.b[0].to <= now))
	    resume(timeheap.b[0].bl->th, 0);
    }
    for(bl = blockers; bl; bl = bl->n)
	remfd(bl);
    buffree(timeheap);
    close(epfd);
    epfd = -1;
    return(exitstatus);
}

void exitioloop(int status)
{
    exitstatus = status;
}
