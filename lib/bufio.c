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
#include <string.h>
#include <errno.h>

#include <utils.h>
#include <bufio.h>

struct bufio *bioopen(void *pdata, struct bufioops *ops)
{
    struct bufio *bio;
    
    omalloc(bio);
    bio->pdata = pdata;
    bio->ops = ops;
    bio->bufhint = 4096;
    return(bio);
}

int bioclose(struct bufio *bio)
{
    int rv;
    
    bioflush(bio);
    if(bio->ops->close)
	rv = bio->ops->close(bio->pdata);
    else
	rv = 0;
    buffree(bio->rbuf);
    buffree(bio->wbuf);
    free(bio);
    return(rv);
}

size_t biordata(struct bufio *bio)
{
    return(bio->rbuf.d - bio->rh);
}

size_t biorspace(struct bufio *bio)
{
    if((bio->rbuf.d - bio->rh) >= bio->bufhint)
	return(0);
    return(bio->bufhint - (bio->rbuf.d - bio->rh));
}

int bioeof(struct bufio *bio)
{
    return(bio->eof && (bio->rh >= bio->rbuf.d));
}

static ssize_t biofill(struct bufio *bio)
{
    size_t ns;
    ssize_t ret;
    
    if(!bio->ops->read) {
	bio->eof = 1;
	return(0);
    }
    if(bio->eof)
	return(0);
    if(bio->rh == bio->rbuf.d)
	bio->rh = bio->rbuf.d = 0;
    if(bio->rbuf.d == bio->rbuf.s) {
	if(bio->rh > 0) {
	    memmove(bio->rbuf.b, bio->rbuf.b + bio->rh, bio->rbuf.d -= bio->rh);
	    bio->rh = 0;
	} else {
	    if((ns = bio->rbuf.s * 2) < bio->bufhint)
		ns = bio->bufhint;
	    sizebuf(bio->rbuf, ns);
	}
    }
    if((bio->rbuf.s > bio->bufhint) && (bio->rbuf.d < bio->bufhint))
	bio->rbuf.b = srealloc(bio->rbuf.b, bio->rbuf.s = bio->bufhint);
    ret = bio->ops->read(bio->pdata, bio->rbuf.b + bio->rbuf.d, bio->rbuf.s - bio->rbuf.d);
    if(ret < 0) {
	bio->err = errno;
	return(-1);
    } else if(ret == 0) {
	bio->eof = 1;
	return(0);
    }
    bio->rbuf.d += ret;
    return(bio->rbuf.d - bio->rh);
}

ssize_t biorensure(struct bufio *bio, size_t bytes)
{
    ssize_t ret;
    
    while(bio->rbuf.d - bio->rh < bytes) {
	if((ret = biofill(bio)) <= 0)
	    return(ret);
    }
    return(bio->rbuf.d - bio->rh);
}

ssize_t biofillsome(struct bufio *bio)
{
    return(biofill(bio));
}

int biogetc(struct bufio *bio)
{
    ssize_t ret;
    
    while(bio->rbuf.d <= bio->rh) {
	if((ret = biofill(bio)) <= 0)
	    return(EOF);
    }
    return((unsigned char)bio->rbuf.b[bio->rh++]);
}

ssize_t bioreadsome(struct bufio *bio, void *buf, size_t len)
{
    ssize_t ret;
    
    if((bio->rh >= bio->rbuf.d) && ((ret = biofill(bio)) <= 0))
	return(ret);
    ret = min(len, bio->rbuf.d - bio->rh);
    memcpy(buf, bio->rbuf.b + bio->rh, ret);
    bio->rh += ret;
    return(ret);
}

size_t biowdata(struct bufio *bio)
{
    return(bio->wbuf.d - bio->wh);
}

size_t biowspace(struct bufio *bio)
{
    if((bio->wbuf.d - bio->wh) >= bio->bufhint)
	return(0);
    return(bio->bufhint - (bio->wbuf.d - bio->wh));
}

int bioflush(struct bufio *bio)
{
    ssize_t ret;
    
    while(bio->wh < bio->wbuf.d) {
	ret = bio->ops->write(bio->pdata, bio->wbuf.b + bio->wh, bio->wbuf.d - bio->wh);
	if(ret < 0) {
	    bio->err = errno;
	    return(-1);
	}
	bio->wh += ret;
    }
    return(0);
}

int bioflushsome(struct bufio *bio)
{
    ssize_t ret;
    
    if(bio->wh < bio->wbuf.d) {
	ret = bio->ops->write(bio->pdata, bio->wbuf.b + bio->wh, bio->wbuf.d - bio->wh);
	if(ret < 0) {
	    bio->err = errno;
	    return(-1);
	}
	bio->wh += ret;
	return(1);
    } else {
	return(0);
    }
}

ssize_t biowensure(struct bufio *bio, size_t bytes)
{
    if(bio->wbuf.s - bio->wbuf.d < bytes) {
	if(!bio->ops->write) {
	    errno = bio->err = EPIPE;
	    return(-1);
	}
	if(bioflush(bio) < 0)
	    return(-1);
	bio->wh = bio->wbuf.d = 0;
	if((bio->wbuf.s > bio->bufhint) && (bytes <= bio->bufhint))
	    bio->wbuf.b = srealloc(bio->wbuf.b, bio->wbuf.s = bio->bufhint);
	else
	    sizebuf(bio->wbuf, (bytes < bio->bufhint)?bio->bufhint:bytes);
    }
    return(0);
}

int bioputc(struct bufio *bio, int c)
{
    if(biowensure(bio, 1) < 0)
	return(-1);
    bio->wbuf.b[bio->wbuf.d++] = c;
    return(0);
}

ssize_t biowrite(struct bufio *bio, const void *data, size_t len)
{
    ssize_t wb, ret;
    
    wb = 0;
    while(len > 0) {
	if(biowensure(bio, min(len, bio->bufhint)) < 0) {
	    if(wb > 0)
		return(wb);
	    return(-1);
	}
	if(len < bio->wbuf.s - bio->wbuf.d) {
	    memcpy(bio->wbuf.b + bio->wbuf.d, data, len);
	    bio->wbuf.d += len;
	    wb += len;
	    len = 0;
	} else {
	    if(bioflush(bio) < 0) {
		if(wb > 0)
		    return(wb);
		return(-1);
	    }
	    bio->wh = bio->wbuf.d = 0;
	    ret = bio->ops->write(bio->pdata, data, len);
	    if(ret < 0) {
		if(wb > 0)
		    return(wb);
		bio->err = errno;
		return(-1);
	    }
	    data += ret; len -= ret; wb += ret;
	}
    }
    return(wb);
}

ssize_t biowritesome(struct bufio *bio, const void *data, size_t len)
{
    ssize_t ret;
    
    sizebuf(bio->wbuf, bio->bufhint);
    if(bio->wh == bio->wbuf.d)
	bio->wh = bio->wbuf.d = 0;
    if(bio->wbuf.d == bio->wbuf.s) {
	if(bio->wh > 0) {
	    memmove(bio->wbuf.b, bio->wbuf.b + bio->wh, bio->wbuf.d -= bio->wh);
	    bio->wh = 0;
	}
    }
    ret = min(len, bio->wbuf.s - bio->wbuf.d);
    memcpy(bio->wbuf.b + bio->wbuf.d, data, ret);
    bio->wbuf.d += ret;
    if(bioflushsome(bio) < 0) {
	if(ret == 0)
	    return(-1);
	if(ret < bio->wbuf.d - bio->wh) { /* Should never be false */
	    bio->wbuf.d -= ret;
	    return(-1);
	}
    }
    return(ret);
}

int bioprintf(struct bufio *bio, const char *format, ...)
{
    va_list args;
    int ret;
    
    if(biowensure(bio, strlen(format)) < 0)
	return(-1);
    while(1) {
	va_start(args, format);
	ret = vsnprintf(bio->wbuf.b + bio->wbuf.d, bio->wbuf.s - bio->wbuf.d, format, args);
	va_end(args);
	if(ret <= bio->wbuf.s - bio->wbuf.d) {
	    bio->wbuf.d += ret;
	    return(0);
	}
	if(biowensure(bio, ret) < 0)
	    return(-1);
    }
}

ssize_t biocopysome(struct bufio *dst, struct bufio *src)
{
    ssize_t ret;
    
    if(src->rh >= src->rbuf.d)
	return(0);
    if((ret = biowritesome(dst, src->rbuf.b + src->rh, src->rbuf.d - src->rh)) < 0)
	return(-1);
    src->rh += ret;
    return(ret);
}
