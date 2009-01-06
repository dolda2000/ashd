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

struct blocker {
    struct blocker *n, *p;
    int fd;
    int ev;
    time_t to;
    struct muth *th;
};

static struct blocker *blockers;

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
    ssize_t ret;
    
    while(1) {
	if(d->sock)
	    ret = send(d->fd, buf, len, MSG_DONTWAIT | MSG_NOSIGNAL);
	else
	    ret = write(d->fd, buf, len);
	if((ret < 0) && (errno == EAGAIN)) {
	    ev = block(d->fd, EV_WRITE, d->timeout);
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
