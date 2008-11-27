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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <req.h>

struct htreq *mkreq(char *method, char *url, char *ver)
{
    struct htreq *req;
    
    omalloc(req);
    req->method = sstrdup(method);
    req->url = sstrdup(url);
    req->ver = sstrdup(ver);
    req->restbuf = sstrdup(url);
    req->rest = req->restbuf;
    return(req);
}

void freereq(struct htreq *req)
{
    int i;
    
    free(req->method);
    free(req->url);
    free(req->ver);
    free(req->restbuf);
    if(req->headers) {
	for(i = 0; i < req->noheaders; i++) {
	    free(req->headers[i][0]);
	    free(req->headers[i][1]);
	    free(req->headers[i]);
	}
	free(req->headers);
    }
    free(req);
}

void reqpreheader(struct htreq *req, char *name, char *val)
{
    req->headers = srealloc(req->headers, sizeof(*req->headers) * (req->noheaders + 1));
    memmove(req->headers + 1, req->headers, sizeof(*req->headers) * req->noheaders);
    req->noheaders++;
    req->headers[0] = smalloc(sizeof(*req->headers[0]) * 2);
    req->headers[0][0] = sstrdup(name);
    req->headers[0][1] = sstrdup(val);
}

void reqappheader(struct htreq *req, char *name, char *val)
{
    int i;

    i = req->noheaders++;
    req->headers = srealloc(req->headers, sizeof(*req->headers) * req->noheaders);
    req->headers[i] = smalloc(sizeof(*req->headers[i]) * 2);
    req->headers[i][0] = sstrdup(name);
    req->headers[i][1] = sstrdup(val);
}
