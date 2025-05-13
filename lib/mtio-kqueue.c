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
    int ev, rev, id;
    int thpos;
    time_t to;
    struct muth *th;
};

static int qfd = -1, fdln = 0;
static int exitstatus;
static struct blocker **fdlist;
static typedbuf(struct blocker *) timeheap;

static int regfd(struct blocker *bl)
{
    struct blocker *o;
    int prev;
    struct kevent evd;
    
    if(bl->fd < 0)
	return(0);
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

static void thraise(struct blocker *bl, int n)
{
    int p;
    
    while(n > 0) {
	p = (n - 1) >> 1;
	if(timeheap.b[p]->to <= bl->to)
	    break;
	timeheap.b[n] = timeheap.b[p];
	timeheap.b[n]->thpos = n;
	n = p;
    }
    timeheap.b[n] = bl;
    bl->thpos = n;
}

static void thlower(struct blocker *bl, int n)
{
    int c;
    
    while(1) {
	c = (n << 1) + 1;
	if(c >= timeheap.d)
	    break;
	if((c + 1 < timeheap.d) && (timeheap.b[c + 1]->to < timeheap.b[c]->to))
	    c = c + 1;
	if(timeheap.b[c]->to > bl->to)
	    break;
	timeheap.b[n] = timeheap.b[c];
	timeheap.b[n]->thpos = n;
	n = c;
    }
    timeheap.b[n] = bl;
    bl->thpos = n;
}

static void addtimeout(struct blocker *bl, time_t to)
{
    sizebuf(timeheap, ++timeheap.d);
    thraise(bl, timeheap.d - 1);
}

static void deltimeout(struct blocker *bl)
{
    int n;
    
    if(bl->thpos == timeheap.d - 1) {
	timeheap.d--;
	return;
    }
    n = bl->thpos;
    bl = timeheap.b[--timeheap.d];
    if((n > 0) && (timeheap.b[(n - 1) >> 1]->to > bl->to))
	thraise(bl, n);
    else
	thlower(bl, n);
}

static int addblock(struct blocker *bl)
{
    if((qfd >= 0) && regfd(bl))
	return(-1);
    bl->n = blockers;
    if(blockers)
	blockers->p = bl;
    blockers = bl;
    if(bl->to > 0)
	addtimeout(bl, bl->to);
    return(0);
}

static void remblock(struct blocker *bl)
{
    if(bl->to > 0)
	deltimeout(bl);
    if(bl->n)
	bl->n->p = bl->p;
    if(bl->p)
	bl->p->n = bl->n;
    if(bl == blockers)
	blockers = bl->n;
    remfd(bl);
}

struct selected mblock(time_t to, int n, struct selected *spec)
{
    int i, id;
    struct blocker bls[n];
    
    to = (to > 0)?(time(NULL) + to):0;
    for(i = 0; i < n; i++) {
	bls[i] = (struct blocker) {
	    .fd = spec[i].fd,
	    .ev = spec[i].ev,
	    .id = i,
	    .to = to,
	    .th = current,
	};
	if(addblock(&bls[i])) {
	    for(i--; i >= 0; i--)
		remblock(&bls[i]);
	    return((struct selected){.fd = -1, .ev = -1});
	}
    }
    id = yield();
    for(i = 0; i < n; i++)
	remblock(&bls[i]);
    if(id < 0)
	return((struct selected){.fd = -1, .ev = -1});
    return((struct selected){.fd = bls[id].fd, .ev = bls[id].rev});
}

int block(int fd, int ev, time_t to)
{
    struct blocker bl;
    int rv;
    
    bl = (struct blocker) {
	.fd = fd,
	.ev = ev,
	.id = -1,
	.to = (to > 0)?(time(NULL) + to):0,
	.th = current,
    };
    addblock(&bl);
    rv = yield();
    remblock(&bl);
    return(rv);
}

int ioloop(void)
{
    struct blocker *bl, *nbl;
    struct kevent evs[16];
    int i, fd, nev, ev;
    time_t now;
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
	now = time(NULL);
	toval = &(struct timespec){};
	if(timeheap.d == 0)
	    toval  = NULL;
	else if(timeheap.b[0]->to > now)
	    *toval = (struct timespec){.tv_sec = timeheap.b[0]->to - now};
	else
	    *toval = (struct timespec){.tv_sec = 1};
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
		if(ev & bl->ev) {
		    if(bl->id < 0) {
			resume(bl->th, ev);
		    } else {
			bl->rev = ev;
			resume(bl->th, bl->id);
		    }
		}
	    }
	}
	now = time(NULL);
	while((timeheap.d > 0) && ((bl = timeheap.b[0])->to <= now)) {
	    if(bl->id < 0) {
		resume(bl->th, 0);
	    } else {
		bl->rev = 0;
		resume(bl->th, bl->id);
	    }
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
