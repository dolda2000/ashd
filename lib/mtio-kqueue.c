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
#include <sys/event.h>
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
    time_t to;
    struct muth *th;
};

static int qfd = -1, fdln = 0;
static int exitstatus;
static struct blocker **fdlist;

static int regfd(struct blocker *bl)
{
    struct blocker *o;
    int prev;
    struct kevent evd;
    
    if(bl->fd >= fdln) {
	if(fdlist) {
	    fdlist = srealloc(fdlist, sizeof(*fdlist) * (bl->fd + 1));
	    memset(fdlist + fdln, 0, sizeof(*fdlist) * (bl->fd + 1 - fdln));
	    fdln = bl->fd + 1;
	} else {
	    fdlist = szmalloc(sizeof(*fdlist) * (fdln = (bl->fd + 1)));
	}
    }
    for(prev = 0, o = fdlist[bl->fd]; o; o = o->n2)
	prev |= o->ev;
    if((bl->ev & EV_READ) && !(prev & EV_READ)) {
	evd = (struct kevent) {
	    .flags = EV_ADD,
	    .ident = bl->fd,
	    .filter = EVFILT_READ,
	};
	if(kevent(qfd, &evd, 1, NULL, 0, NULL) < 0) {
	    /* XXX?! Whatever to do, really? */
	    flog(LOG_ERR, "kevent(EV_ADD, EVFILT_READ) on fd %i: %s", bl->fd, strerror(errno));
	    return(-1);
	}
    }
    if((bl->ev & EV_WRITE) && !(prev & EV_WRITE)) {
	evd = (struct kevent) {
	    .flags = EV_ADD,
	    .ident = bl->fd,
	    .filter = EVFILT_WRITE,
	};
	if(kevent(qfd, &evd, 1, NULL, 0, NULL) < 0) {
	    /* XXX?! Whatever to do, really? */
	    flog(LOG_ERR, "kevent(EV_ADD, EVFILT_WRITE) on fd %i: %s", bl->fd, strerror(errno));
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
    struct kevent evd;
    int left;
    
    if(!bl->reg)
	return;
    if(bl->n2)
	bl->n2->p2 = bl->p2;
    if(bl->p2)
	bl->p2->n2 = bl->n2;
    if(bl == fdlist[bl->fd])
	fdlist[bl->fd] = bl->n2;
    for(left = 0, o = fdlist[bl->fd]; o; o = o->n2)
	left |= o->ev;
    if((bl->ev & EV_READ) && !(left & EV_READ)) {
	evd = (struct kevent) {
	    .flags = EV_DELETE,
	    .ident = bl->fd,
	    .filter = EVFILT_READ,
	};
	if(kevent(qfd, &evd, 1, NULL, 0, NULL) < 0) {
	    /* XXX?! Whatever to do, really? */
	    flog(LOG_ERR, "kevent(EV_DELETE, EVFILT_READ) on fd %i: %s", bl->fd, strerror(errno));
	}
    }
    if((bl->ev & EV_WRITE) && !(left & EV_WRITE)) {
	evd = (struct kevent) {
	    .flags = EV_DELETE,
	    .ident = bl->fd,
	    .filter = EVFILT_WRITE,
	};
	if(kevent(qfd, &evd, 1, NULL, 0, NULL) < 0) {
	    /* XXX?! Whatever to do, really? */
	    flog(LOG_ERR, "kevent(EV_DELETE, EVFILT_WRITE) on fd %i: %s", bl->fd, strerror(errno));
	}
    }
    bl->reg = 0;
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
    if((qfd >= 0) && regfd(bl)) {
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

int ioloop(void)
{
    struct blocker *bl, *nbl;
    struct kevent evs[16];
    int i, fd, nev, ev;
    time_t now, timeout;
    struct timespec *toval;
    
    exitstatus = 0;
    qfd = kqueue();
    fcntl(qfd, F_SETFD, FD_CLOEXEC);
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
	    toval  = NULL;
	else if(timeout > now)
	    toval = &(struct timespec){.tv_sec = timeout - now};
	else
	    toval = &(struct timespec){.tv_sec = 1};
	if(exitstatus)
	    break;
	nev = kevent(qfd, NULL, 0, evs, sizeof(evs) / sizeof(*evs), toval);
	if(nev < 0) {
	    if(errno != EINTR) {
		flog(LOG_CRIT, "ioloop: kevent errored out: %s", strerror(errno));
		/* To avoid CPU hogging in case it's bad, which it
		 * probably is. */
		sleep(1);
	    }
	    continue;
	}
	for(i = 0; i < nev; i++) {
	    fd = (int)evs[i].ident;
	    ev = (evs[i].filter == EVFILT_READ)?EV_READ:EV_WRITE;
	    for(bl = fdlist[fd]; bl; bl = nbl) {
		nbl = bl->n2;
		if(ev & bl->ev)
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
    for(bl = blockers; bl; bl = bl->n)
	remfd(bl);
    close(qfd);
    qfd = -1;
    return(exitstatus);
}

void exitioloop(int status)
{
    exitstatus = status;
}
