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

int sendreq(int sock, struct hthead *req)
{
    int ret, i;
    int pfds[2];
    struct charbuf buf;
    
    if(socketpair(PF_UNIX, SOCK_DGRAM, 0, pfds))
	return(-1);
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
    ret = sendfd(sock, pfds[0], buf.b, buf.d);
    buffree(buf);
    close(pfds[0]);
    if(ret < 0) {
	close(pfds[1]);
	return(-1);
    } else {
	return(pfds[1]);
    }
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
