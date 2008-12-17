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
#include <unistd.h>
#include <errno.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>

int main(int argc, char **argv)
{
    struct hthead *req;
    int fd;
    FILE *out;
    
    while(1) {
	if((fd = recvreq(0, &req)) < 0) {
	    if(errno != 0)
		fprintf(stderr, "debugsink: recvreq: %s", strerror(errno));
	    break;
	}
	printf("%s %s %s %s %s\n", req->method, req->url, req->ver, req->rest, getheader(req, "x-ash-address"));
	out = fdopen(fd, "w");
	fprintf(out, "HTTP/1.1 200 OK\r\n");
	fprintf(out, "Content-Type: text/plain; charset=utf8\r\n");
	fprintf(out, "\r\n");
	fprintf(out, "%s\n", getheader(req, "x-ash-port"));
	fprintf(out, "%s\n", getheader(req, "user-agent"));
	fprintf(out, "%s\n", getheader(req, "content-length"));
	fclose(out);
    }
    return(0);
}
