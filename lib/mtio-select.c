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
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>
#include <sys/select.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <log.h>
#include <utils.h>
#include <mt.h>
#include <mtio.h>

static struct blocker *blockers;
static int exitstatus;

struct blocker {
    struct blocker *n, *p;
    struct iterator *it;
    int fd;
    int ev, rev, id;
    time_t to;
    struct muth *th;
};

struct iterator {
    struct blocker *bl;
};

static void addblock(struct blocker *bl)
{
    bl->n = blockers;
    if(blockers)
	blockers->p = bl;
    blockers = bl;
}

static void remblock(struct blocker *bl)
{
    if(bl->n)
	bl->n->p = bl->p;
    if(bl->p)
	bl->p->n = bl->n;
    if(bl == blockers)
	blockers = bl->n;
    if(bl->it) {
	if((bl->it->bl = bl->n) != NULL)
	    bl->it->bl->it = bl->it;
	bl->it = NULL;
    }
}

struct selected mblock(time_t to, int n, struct selected *spec)
{
    int i, id;
    struct blocker bls[n];
    
    to = (to > 0)?(time(NULL) + to):0;
    for(i = 0; i < n; i++) {
	bls[i] = (struct blocker){
	    .fd = spec[i].fd,
	    .ev = spec[i].ev,
	    .id = i,
	    .to = to,
	    .th = current,
	};
	addblock(&bls[i]);
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
    
    if(fd >= FD_SETSIZE) {
	flog(LOG_ERR, "tried to use more file descriptors than select() can handle: fd %i", fd);
	errno = EMFILE;
	return(-1);
    }
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
    int ret;
    fd_set rfds, wfds, efds;
    struct blocker *bl;
    struct iterator it;
    struct timeval toval;
    time_t now, timeout;
    int maxfd;
    int ev;
    
    exitstatus = 0;
    while(blockers != NULL) {
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);
	maxfd = 0;
	now = time(NULL);
	timeout = 0;
	for(bl = blockers; bl; bl = bl->n) {
	    if(bl->fd >= 0) {
		if(bl->ev & EV_READ)
		    FD_SET(bl->fd, &rfds);
		if(bl->ev & EV_WRITE)
		    FD_SET(bl->fd, &wfds);
		FD_SET(bl->fd, &efds);
		if(bl->fd > maxfd)
		    maxfd = bl->fd;
	    }
	    if((bl->to != 0) && ((timeout == 0) || (timeout > bl->to)))
		timeout = bl->to;
	}
	if(exitstatus)
	    return(exitstatus);
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
	} else {
	    now = time(NULL);
	    for(bl = it.bl = blockers; bl; bl = it.bl) {
		if((it.bl = bl->n) != NULL)
		    it.bl->it = &it;
		ev = 0;
		if(bl->fd >= 0) {
		    if(FD_ISSET(bl->fd, &rfds))
			ev |= EV_READ;
		    if(FD_ISSET(bl->fd, &wfds))
			ev |= EV_WRITE;
		    if(FD_ISSET(bl->fd, &efds))
			ev = -1;
		}
		if((ev < 0) || (ev & bl->ev)) {
		    if(bl->id < 0) {
			resume(bl->th, ev);
		    } else {
			bl->rev = ev;
			resume(bl->th, bl->id);
		    }
		} else if((bl->to != 0) && (bl->to <= now)) {
		    if(bl->id < 0) {
			resume(bl->th, 0);
		    } else {
			bl->rev = 0;
			resume(bl->th, bl->id);
		    }
		}
		if(it.bl)
		    it.bl->it = NULL;
	    }
	}
    }
    return(0);
}

void exitioloop(int status)
{
    exitstatus = status;
}
