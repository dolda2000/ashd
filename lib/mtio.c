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

FILE *mtstdopen(int fd, int issock, int timeout, char *mode, struct stdiofd **infop)
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
    if(!(ret = funstdio(d, r?mtread:NULL, w?mtwrite:NULL, NULL, mtclose))) {
	free(d);
	return(NULL);
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    if(infop)
	*infop = d;
    return(ret);
}

struct pipe {
    struct charbuf data;
    size_t bufmax;
    int closed;
    struct muth *r, *w;
};

static void freepipe(struct pipe *p)
{
    buffree(p->data);
    free(p);
}

static ssize_t piperead(void *pdata, void *buf, size_t len)
{
    struct pipe *p = pdata;
    ssize_t ret;
    
    while(p->data.d == 0) {
	if(p->closed & 2)
	    return(0);
	if(p->r) {
	    errno = EBUSY;
	    return(-1);
	}
	p->r = current;
	yield();
	p->r = NULL;
    }
    ret = min(len, p->data.d);
    memcpy(buf, p->data.b, ret);
    memmove(p->data.b, p->data.b + ret, p->data.d -= ret);
    if(p->w)
	resume(p->w, 0);
    return(ret);
}

static int piperclose(void *pdata)
{
    struct pipe *p = pdata;
    
    if(p->closed & 2) {
	freepipe(p);
    } else {
	p->closed |= 1;
	if(p->w)
	    resume(p->w, 0);
    }
    return(0);
}

static ssize_t pipewrite(void *pdata, const void *buf, size_t len)
{
    struct pipe *p = pdata;
    ssize_t ret;
    
    if(p->closed & 1) {
	errno = EPIPE;
	return(-1);
    }
    while(p->data.d >= p->bufmax) {
	if(p->w) {
	    errno = EBUSY;
	    return(-1);
	}
	p->w = current;
	yield();
	p->w = NULL;
	if(p->closed & 1) {
	    errno = EPIPE;
	    return(-1);
	}
    }
    ret = min(len, p->bufmax - p->data.d);
    sizebuf(p->data, p->data.d + ret);
    memcpy(p->data.b + p->data.d, buf, ret);
    p->data.d += ret;
    if(p->r)
	resume(p->r, 0);
    return(ret);
}

static int pipewclose(void *pdata)
{
    struct pipe *p = pdata;
    
    if(p->closed & 1) {
	freepipe(p);
    } else {
	p->closed |= 2;
	if(p->r)
	    resume(p->r, 0);
    }
    return(0);
}

void mtiopipe(FILE **read, FILE **write)
{
    struct pipe *p;
    
    omalloc(p);
    p->bufmax = 4096;
    *read = funstdio(p, piperead, NULL, NULL, piperclose);
    *write = funstdio(p, NULL, pipewrite, NULL, pipewclose);
}
