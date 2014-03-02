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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <sys/socket.h>

#include <log.h>
#include <utils.h>
#include <mt.h>
#include <mtio.h>

struct stdiofd {
    int fd;
    int sock;
    int timeout;
};

static ssize_t mtread(void *cookie, void *buf, size_t len)
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

static ssize_t mtwrite(void *cookie, const void *buf, size_t len)
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

FILE *mtstdopen(int fd, int issock, int timeout, char *mode)
{
    struct stdiofd *d;
    FILE *ret;
    int r, w;
    
    if(!strcmp(mode, "r")) {
	r = 1; w = 0;
    } else if(!strcmp(mode, "w")) {
	r = 0; w = 1;
    } else if(!strcmp(mode, "r+")) {
	r = w = 1;
    } else {
	return(NULL);
    }
    omalloc(d);
    d->fd = fd;
    d->sock = issock;
    d->timeout = timeout;
    ret = funstdio(d, r?mtread:NULL, w?mtwrite:NULL, NULL, mtclose);
    if(!ret)
	free(d);
    else
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    return(ret);
}
