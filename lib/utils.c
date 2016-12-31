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
#include <string.h>
#include <ctype.h>
#include <errno.h>

#include <utils.h>

static char *base64set = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static int base64rev[] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
};

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
    
    if(ca == NULL)
	return;
    for(c = ca; *c; c++)
	free(*c);
    free(ca);
}

int calen(char **a)
{
    int i;
    
    for(i = 0; *a; a++, i++);
    return(i);
}

void bvprintf(struct charbuf *buf, char *format, va_list al)
{
    va_list al2;
    int ret;
    
    while(1) {
	va_copy(al2, al);
	ret = vsnprintf(buf->b + buf->d, buf->s - buf->d, format, al2);
	va_end(al2);
	if(ret < buf->s - buf->d) {
	    buf->d += ret;
	    return;
	}
	sizebuf(*buf, buf->d + ret + 1);
    }
}

void bprintf(struct charbuf *buf, char *format, ...)
{
    va_list args;
    
    va_start(args, format);
    bvprintf(buf, format, args);
    va_end(args);
}

void replstr(char **p, char *n)
{
    char *tmp;
    
    tmp = *p;
    if(n)
	*p = sstrdup(n);
    else
	*p = NULL;
    if(tmp)
	free(tmp);
}

char *base64encode(char *data, size_t datalen)
{
    struct charbuf buf;
    
    if(datalen == 0)
	return(sstrdup(""));
    bufinit(buf);
    while(datalen >= 3)
    {
	bufadd(buf, base64set[(data[0] & 0xfc) >> 2]);
	bufadd(buf, base64set[((data[0] & 0x03) << 4) | ((data[1] & 0xf0) >> 4)]);
	bufadd(buf, base64set[((data[1] & 0x0f) << 2) | ((data[2] & 0xc0) >> 6)]);
	bufadd(buf, base64set[data[2] & 0x3f]);
	datalen -= 3;
	data += 3;
    }
    if(datalen == 1)
    {
	bufadd(buf, base64set[(data[0] & 0xfc) >> 2]);
	bufadd(buf, base64set[(data[0] & 0x03) << 4]);
	bufcat(buf, "==", 2);
    }
    if(datalen == 2)
    {
	bufadd(buf, base64set[(data[0] & 0xfc) >> 2]);
	bufadd(buf, base64set[((data[0] & 0x03) << 4) | ((data[1] & 0xf0) >> 4)]);
	bufadd(buf, base64set[(data[1] & 0x0f) << 2]);
	bufadd(buf, '=');
    }
    bufadd(buf, 0);
    return(buf.b);
}

char *base64decode(char *data, size_t *datalen)
{
    int b, c;
    char cur;
    struct charbuf buf;
    
    bufinit(buf);
    cur = 0;
    b = 8;
    for(; *data > 0; data++)
    {
	c = (int)(unsigned char)*data;
	if(c == '=')
	    break;
	if(c == '\n')
	    continue;
	if(base64rev[c] == -1)
	{
	    buffree(buf);
	    return(NULL);
	}
	b -= 6;
	if(b <= 0)
	{
	    cur |= base64rev[c] >> -b;
	    bufadd(buf, cur);
	    b += 8;
	    cur = 0;
	}
	cur |= base64rev[c] << b;
    }
    if(datalen != NULL)
	*datalen = buf.d;
    bufadd(buf, 0);
    return(buf.b);
}

int hexdigit(char c)
{
    if((c >= '0') && (c <= '9'))      return(c - '0');
    else if((c >= 'a') && (c <= 'f')) return(c - 'a' + 10);
    else if((c >= 'A') && (c <= 'F')) return(c - 'A' + 10);
    return(-1);
}

static int btheight(struct btree *tree)
{
    if(tree == NULL)
	return(0);
    return(tree->h);
}

static void btsetheight(struct btree *tree)
{
    if(tree == NULL)
	return;
    tree->h = max(btheight(tree->l), btheight(tree->r)) + 1;
}

static void bbtrl(struct btree **tree);

static void bbtrr(struct btree **tree)
{
    struct btree *m, *l, *r;
    
    if(btheight((*tree)->l->r) > btheight((*tree)->l->l))
	bbtrl(&(*tree)->l);
    r = (*tree);
    l = r->l;
    m = l->r;
    r->l = m;
    btsetheight(r);
    l->r = r;
    btsetheight(l);
    *tree = l;
}

static void bbtrl(struct btree **tree)
{
    struct btree *m, *l, *r;
    
    if(btheight((*tree)->r->l) > btheight((*tree)->r->r))
	bbtrr(&(*tree)->r);
    l = (*tree);
    r = l->r;
    m = r->l;
    l->r = m;
    btsetheight(l);
    r->l = l;
    btsetheight(r);
    *tree = r;
}

static int ptrcmp(void *a, void *b)
{
    if(a < b)
	return(-1);
    else if(a > b)
	return(1);
    return(0);
}

int bbtreedel(struct btree **tree, void *item, int (*cmp)(void *, void *))
{
    int c, r;
    struct btree *s, **sp, *o;
    
    if(cmp == NULL)
	cmp = ptrcmp;
    if(*tree == NULL)
	return(0);
    if((c = cmp(item, (*tree)->d)) < 0) {
	r = bbtreedel(&(*tree)->l, item, cmp);
    } else if(c > 0) {
	r = bbtreedel(&(*tree)->r, item, cmp);
    } else {
	r = 1;
	o = *tree;
	if(((*tree)->r != NULL) && ((*tree)->l != NULL)) {
	    sp = &(*tree)->r;
	    s = *sp;
	    while(s->l != NULL) {
		sp = &(s->l);
		s = *sp;
	    }
	    *sp = (*sp)->r;
	    s->l = (*tree)->l;
	    s->r = (*tree)->r;
	    *tree = s;
	} else if((*tree)->l != NULL) {
	    *tree = (*tree)->l;
	} else if((*tree)->r != NULL) {
	    *tree = (*tree)->r;
	} else {
	    *tree = NULL;
	}
	free(o);
    }
    if(*tree != NULL) {
	btsetheight(*tree);
	if(btheight((*tree)->l) > btheight((*tree)->r) + 1)
	    bbtrr(tree);
	if(btheight((*tree)->r) > btheight((*tree)->l) + 1)
	    bbtrl(tree);
    }
    return(r);
}

void freebtree(struct btree **tree, void (*ffunc)(void *))
{
    if(*tree == NULL)
	return;
    freebtree(&(*tree)->l, ffunc);
    freebtree(&(*tree)->r, ffunc);
    if(ffunc != NULL)
	ffunc((*tree)->d);
    free(*tree);
    *tree = NULL;
}

int bbtreeput(struct btree **tree, void *item, int (*cmp)(void *, void *))
{
    int c, r;
    
    if(cmp == NULL)
	cmp = ptrcmp;
    if(*tree == NULL) {
	*tree = szmalloc(sizeof(**tree));
	(*tree)->d = item;
	(*tree)->h = 1;
	return(1);
    }
    if((c = cmp(item, (*tree)->d)) < 0)
	r = bbtreeput(&(*tree)->l, item, cmp);
    else if(c > 0)
	r = bbtreeput(&(*tree)->r, item, cmp);
    else
	return(0);
    btsetheight(*tree);
    if(btheight((*tree)->l) > btheight((*tree)->r) + 1)
	bbtrr(tree);
    if(btheight((*tree)->r) > btheight((*tree)->l) + 1)
	bbtrl(tree);
    return(r);
}

void *btreeget(struct btree *tree, void *key, int (*cmp)(void *, void *))
{
    int c;
    
    if(cmp == NULL)
	cmp = ptrcmp;
    while(1) {
	if(tree == NULL)
	    return(NULL);
	c = cmp(key, tree->d);
	if(c < 0)
	    tree = tree->l;
	else if(c > 0)
	    tree = tree->r;
	else
	    return(tree->d);
    }
}

struct stdif {
    ssize_t (*read)(void *pdata, void *buf, size_t len);
    ssize_t (*write)(void *pdata, const void *buf, size_t len);
    off_t (*seek)(void *pdata, off_t offset, int whence);
    int (*close)(void *pdata);
    void *pdata;
};

#if defined(HAVE_GLIBC_STDIO)
static ssize_t wrapread(void *pdata, char *buf, size_t len)
{
    struct stdif *nf = pdata;
    
    return(nf->read(nf->pdata, buf, len));
}

static ssize_t wrapwrite(void *pdata, const char *buf, size_t len)
{
    struct stdif *nf = pdata;
    size_t off;
    ssize_t ret;
    
    /*
     * XXX? In seeming violation of its own manual, glibc requires the
     * cookie-write function to complete writing the entire buffer,
     * rather than working like write(2).
     */
    off = 0;
    while(off < len) {
	ret = nf->write(nf->pdata, buf + off, len - off);
	if(ret < 0)
	    return(off);
	off += ret;
    }
    return(off);
}

static int wrapseek(void *pdata, off64_t *pos, int whence)
{
    struct stdif *nf = pdata;
    off_t ret;
    
    ret = nf->seek(nf->pdata, *pos, whence);
    if(ret < 0)
	return(-1);
    *pos = ret;
    return(0);
}

static int wrapclose(void *pdata)
{
    struct stdif *nf = pdata;
    int ret;
    
    if(nf->close != NULL)
	ret = nf->close(nf->pdata);
    else
	ret = 0;
    free(nf);
    return(ret);
}

FILE *funstdio(void *pdata,
	       ssize_t (*read)(void *pdata, void *buf, size_t len),
	       ssize_t (*write)(void *pdata, const void *buf, size_t len),
	       off_t (*seek)(void *pdata, off_t offset, int whence),
	       int (*close)(void *pdata))
{
    struct stdif *nf;
    cookie_io_functions_t io;
    char *mode;
    
    if(read && write) {
	mode = "r+";
    } else if(read) {
	mode = "r";
    } else if(write) {
	mode = "w";
    } else {
	errno = EINVAL;
	return(NULL);
    }
    omalloc(nf);
    *nf = (struct stdif){.read = read, .write = write, .seek = seek, .close = close, .pdata = pdata};
    io = (cookie_io_functions_t) {
	.read = read?wrapread:NULL,
	.write = write?wrapwrite:NULL,
	.seek = seek?wrapseek:NULL,
	.close = wrapclose,
    };
    return(fopencookie(nf, mode, io));
}
#elif defined(HAVE_BSD_STDIO)
static int wrapread(void *pdata, char *buf, int len)
{
    struct stdif *nf = pdata;
    
    return(nf->read(nf->pdata, buf, len));
}

static int wrapwrite(void *pdata, const char *buf, int len)
{
    struct stdif *nf = pdata;
    
    return(nf->write(nf->pdata, buf, len));
}

static fpos_t wrapseek(void *pdata, fpos_t pos, int whence)
{
    struct stdif *nf = pdata;
    
    return(nf->seek(nf->pdata, pos, whence));
}

static int wrapclose(void *pdata)
{
    struct stdif *nf = pdata;
    int ret;
    
    if(nf->close != NULL)
	ret = nf->close(nf->pdata);
    else
	ret = 0;
    free(nf);
    return(ret);
}

FILE *funstdio(void *pdata,
	       ssize_t (*read)(void *pdata, void *buf, size_t len),
	       ssize_t (*write)(void *pdata, const void *buf, size_t len),
	       off_t (*seek)(void *pdata, off_t offset, int whence),
	       int (*close)(void *pdata))
{
    struct stdif *nf;
    
    omalloc(nf);
    *nf = (struct stdif){.read = read, .write = write, .seek = seek, .close = close, .pdata = pdata};
    return(funopen(nf, read?wrapread:NULL, write?wrapwrite:NULL, seek?wrapseek:NULL, wrapclose));
}
#else
#error "No stdio implementation for this system"
#endif
