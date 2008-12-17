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
#include <string.h>
#include <ctype.h>

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

char **tokenize(char *src)
{
    char **ret;
    char *p, *p2, *n;
    int s, q, cl;
    
    p = src;
    s = 0;
    ret = NULL;
    while(1) {
	while(isspace(*p))
	    p++;
	if(!*p)
	    break;
	p2 = p;
	q = 0;
	while(1) {
	    if(q) {
		if(*p == '\"')
		    q = 0;
		else if(*p == '\\')
		    p++;
	    } else {
		if(*p == '\"')
		    q = 1;
		else if(isspace(*p) || !*p)
		    break;
		else if(*p == '\\')
		    p++;
	    }
	    p++;
	}
	cl = p - p2;
	n = memcpy(malloc(cl + 1), p2, cl);
	n[cl] = 0;
	for(p2 = n; *p2; cl--) {
	    if(*p2 == '\\') {
		memmove(p2, p2 + 1, cl--);
		p2++;
	    } else if(*p2 == '\"') {
		memmove(p2, p2 + 1, cl);
	    } else {
		p2++;
	    }
	}
	ret = realloc(ret, sizeof(char *) * (++s));
	ret[s - 1] = n;
    }
    ret = realloc(ret, sizeof(char *) * (++s));
    ret[s - 1] = NULL;
    return(ret);
}

void freeca(char **ca)
{
    char **c;
    
    for(c = ca; *c; c++)
	free(*c);
    free(ca);
}
