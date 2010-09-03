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
#include <unistd.h>
#include <sys/socket.h>
#include <errno.h>
#include <ctype.h>
#include <stdio.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>

struct hthead *mkreq(char *method, char *url, char *ver)
{
    struct hthead *req;
    
    omalloc(req);
    req->method = sstrdup(method);
    req->url = sstrdup(url);
    req->ver = sstrdup(ver);
    req->rest = sstrdup(url);
    return(req);
}

struct hthead *mkresp(int code, char *msg, char *ver)
{
    struct hthead *resp;
    
    omalloc(resp);
    resp->code = code;
    resp->msg = sstrdup(msg);
    resp->ver = sstrdup(ver);
    return(resp);
}

void freehthead(struct hthead *head)
{
    int i;
    
    if(head->method != NULL)
	free(head->method);
    if(head->url != NULL)
	free(head->url);
    if(head->msg != NULL)
	free(head->msg);
    if(head->ver != NULL)
	free(head->ver);
    if(head->rest != NULL)
	free(head->rest);
    if(head->headers) {
	for(i = 0; i < head->noheaders; i++) {
	    free(head->headers[i][0]);
	    free(head->headers[i][1]);
	    free(head->headers[i]);
	}
	free(head->headers);
    }
    free(head);
}

char *getheader(struct hthead *head, char *name)
{
    int i;
    
    for(i = 0; i < head->noheaders; i++) {
	if(!strcasecmp(head->headers[i][0], name))
	    return(head->headers[i][1]);
    }
    return(NULL);
}

static void trim(struct charbuf *buf)
{
    char *p;
    
    for(p = buf->b; (p - buf->b < buf->d) && isspace(*p); p++);
    memmove(buf->b, p, buf->d -= (p - buf->b));
    if(buf->d > 0)
	for(p = buf->b + buf->d - 1; (p > buf->b) && isspace(*p); p--, buf->d--);
}

int parseheaders(struct hthead *head, FILE *in)
{
    int c, state;
    struct charbuf name, val;
    
    bufinit(name);
    bufinit(val);
    state = 0;
    while(1) {
	c = fgetc(in);
    again:
	if(state == 0) {
	    if(c == '\r') {
	    } else if(c == '\n') {
		break;
	    } else if(c == EOF) {
		goto fail;
	    } else {
		state = 1;
		goto again;
	    }
	} else if(state == 1) {
	    if(c == ':') {
		trim(&name);
		bufadd(name, 0);
		state = 2;
	    } else if(c == '\r') {
	    } else if(c == '\n') {
		goto fail;
	    } else if(c == EOF) {
		goto fail;
	    } else {
		bufadd(name, c);
	    }
	} else if(state == 2) {
	    if(c == '\r') {
	    } else if(c == '\n') {
		trim(&val);
		bufadd(val, 0);
		headappheader(head, name.b, val.b);
		buffree(name);
		buffree(val);
		state = 0;
	    } else if(c == EOF) {
		goto fail;
	    } else {
		bufadd(val, c);
	    }
	}
    }
    return(0);
    
fail:
    buffree(name);
    buffree(val);
    return(-1);
}

void replrest(struct hthead *head, char *rest)
{
    char *tmp;
    
    /* Do not free the current rest string yet, so that the new one
     * can be taken from a subpart of the old one. */
    tmp = head->rest;
    head->rest = sstrdup(rest);
    free(tmp);
}

void headpreheader(struct hthead *head, const char *name, const char *val)
{
    head->headers = srealloc(head->headers, sizeof(*head->headers) * (head->noheaders + 1));
    memmove(head->headers + 1, head->headers, sizeof(*head->headers) * head->noheaders);
    head->noheaders++;
    head->headers[0] = smalloc(sizeof(*head->headers[0]) * 2);
    head->headers[0][0] = sstrdup(name);
    head->headers[0][1] = sstrdup(val);
}

void headappheader(struct hthead *head, const char *name, const char *val)
{
    int i;

    i = head->noheaders++;
    head->headers = srealloc(head->headers, sizeof(*head->headers) * head->noheaders);
    head->headers[i] = smalloc(sizeof(*head->headers[i]) * 2);
    head->headers[i][0] = sstrdup(name);
    head->headers[i][1] = sstrdup(val);
}

void headrmheader(struct hthead *head, const char *name)
{
    int i;
    
    for(i = 0; i < head->noheaders; i++) {
	if(!strcasecmp(head->headers[i][0], name)) {
	    free(head->headers[i][0]);
	    free(head->headers[i][1]);
	    free(head->headers[i]);
	    memmove(head->headers + i, head->headers + i + 1, sizeof(head->headers) * (--head->noheaders - i));
	    return;
	}
    }
}

int writeresp(FILE *out, struct hthead *resp)
{
    int i;
    
    if(fprintf(out, "%s %i %s\r\n", resp->ver, resp->code, resp->msg) < 0)
	return(-1);
    for(i = 0; i < resp->noheaders; i++) {
	if(fprintf(out, "%s: %s\r\n", resp->headers[i][0], resp->headers[i][1]) < 0)
	    return(-1);
    }
    return(0);
}

int sendreq(int sock, struct hthead *req, int fd)
{
    int ret, i;
    struct charbuf buf;
    
    bufinit(buf);
    bufcatstr2(buf, req->method);
    bufcatstr2(buf, req->url);
    bufcatstr2(buf, req->ver);
    bufcatstr2(buf, req->rest);
    for(i = 0; i < req->noheaders; i++) {
	bufcatstr2(buf, req->headers[i][0]);
	bufcatstr2(buf, req->headers[i][1]);
    }
    bufcatstr2(buf, "");
    ret = sendfd(sock, fd, buf.b, buf.d);
    buffree(buf);
    if(ret < 0)
	return(-1);
    else
	return(0);
}

int recvreq(int sock, struct hthead **reqp)
{
    int fd;
    struct charbuf buf;
    char *p;
    size_t l;
    char *name, *val;
    struct hthead *req;
    
    if((fd = recvfd(sock, &buf.b, &buf.d)) < 0) {
	return(-1);
    }
    buf.s = buf.d;
    p = buf.b;
    l = buf.d;
    
    *reqp = omalloc(req);
    if((req->method = sstrdup(decstr(&p, &l))) == NULL)
	goto fail;
    if((req->url = sstrdup(decstr(&p, &l))) == NULL)
	goto fail;
    if((req->ver = sstrdup(decstr(&p, &l))) == NULL)
	goto fail;
    if((req->rest = sstrdup(decstr(&p, &l))) == NULL)
	goto fail;
    
    while(1) {
	if(!*(name = decstr(&p, &l)))
	    break;
	val = decstr(&p, &l);
	headappheader(req, name, val);
    }
    
    buffree(buf);
    return(fd);
    
fail:
    close(fd);
    freehthead(req);
    errno = EPROTO;
    return(-1);
}

char *unquoteurl(char *in)
{
    struct charbuf buf;
    char *p;
    int c;
    
    bufinit(buf);
    p = in;
    while(*p) {
	if(*p == '%') {
	    if(!p[1] || !p[2])
		goto fail;
	    c = 0;
	    if((p[1] >= '0') && (p[1] <= '9'))          c |= (p[1] - '0') << 4;
	    else if((p[1] >= 'a') && (p[1] <= 'f'))     c |= (p[1] - 'a' + 10) << 4;
	    else if((p[1] >= 'A') && (p[1] <= 'F'))     c |= (p[1] - 'A' + 10) << 4;
	    else                                        goto fail;
	    if((p[2] >= '0') && (p[2] <= '9'))          c |= (p[2] - '0');
	    else if((p[2] >= 'a') && (p[2] <= 'f'))     c |= (p[2] - 'a' + 10);
	    else if((p[2] >= 'A') && (p[2] <= 'F'))     c |= (p[2] - 'A' + 10);
	    else                                        goto fail;
	    bufadd(buf, c);
	    p += 3;
	} else {
	    bufadd(buf, *(p++));
	}
    }
    bufadd(buf, 0);
    return(buf.b);
fail:
    buffree(buf);
    return(NULL);
}
