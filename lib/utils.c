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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>

void _sizebuf(struct buffer *buf, size_t wanted, size_t el)
{
    size_t n;
    
    n = buf->s;
    if(n == 0)
	n = 1;
    while(n < wanted)
	n <<= 1;
    if(n <= buf->s)
	return;
    if(buf->b != NULL)
	buf->b = srealloc(buf->b, n * el);
    else
	buf->b = smalloc(n * el);
    buf->s = n;
}

char *decstr(char **p, size_t *len)
{
    char *p2, *ret;
    
    for(p2 = *p; (p2 - *p) < *len; p2++) {
	if(*p2 == 0)
	    break;
    }
    if((p2 - *p) == *len)
	return(NULL);
    p2++;
    ret = *p;
    *len -= p2 - *p;
    *p = p2;
    return(ret);
}

char *vsprintf2(char *format, va_list al)
{
    int ret;
    char *buf;
    va_list al2;
    
    va_copy(al2, al);
    ret = vsnprintf(NULL, 0, format, al2);
    va_end(al2);
    buf = smalloc(ret + 1);
    va_copy(al2, al);
    vsnprintf(buf, ret + 1, format, al2);
    va_end(al2);
    return(buf);
}

char *sprintf2(char *format, ...)
{
    va_list args;
    char *buf;
    
    va_start(args, format);
    buf = vsprintf2(format, args);
    va_end(args);
    return(buf);
}

char *sprintf3(char *format, ...)
{
    static char *buf = NULL;
    va_list args;
    
    if(buf != NULL)
	free(buf);
    va_start(args, format);
    buf = vsprintf2(format, args);
    va_end(args);
    return(buf);
}

off_t atoo(char *n)
{
    return((off_t)strtoll(n, NULL, 10));
}
