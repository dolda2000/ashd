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
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <sys/poll.h>
#include <time.h>
#include <sys/time.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>

#define DEFFORMAT "%{%Y-%m-%d %H:%M:%S}t %m %u %A \"%G\""

static int ch;
static FILE *out;
static int flush = 1;
static char *format;
static time_t now;

static void qputs(char *s, FILE *o)
{
    for(; *s; s++) {
	if(*s == '\"') {
	    fputs("\\\"", o);
	} else if(*s == '\\') {
	    fputs("\\\\", o);
	} else if(*s == '\n') {
	    fputs("\\n", o);
	} else if(*s == '\t') {
	    fputs("\\t", o);
	} else if((*s < 32) || (*s >= 128)) {
	    fprintf(o, "\\x%02x", *s);
	} else {
	    fputc(*s, o);
	}
    }
}

static void logitem(struct hthead *req, char o, char *d)
{
    char *h, *p;
    char buf[1024];
    struct timeval tv;
    
    switch(o) {
    case '%':
	putc('%', out);
	break;
    case 'h':
	if((h = getheader(req, d)) == NULL) {
	    putc('-', out);
	} else {
	    qputs(h, out);
	}
	break;
    case 'u':
	qputs(req->url, out);
	break;
    case 'U':
	strcpy(buf, req->url);
	if((p = strchr(buf, '?')) != NULL)
	    *p = 0;
	qputs(buf, out);
	break;
    case 'm':
	qputs(req->method, out);
	break;
    case 'r':
	qputs(req->rest, out);
	break;
    case 'v':
	qputs(req->ver, out);
	break;
    case 't':
	if(!*d)
	    d = "%a, %d %b %Y %H:%M:%S %z";
	strftime(buf, sizeof(buf), d, localtime(&now));
	qputs(buf, out);
	break;
    case 'T':
	if(!*d)
	    d = "%a, %d %b %Y %H:%M:%S %z";
	strftime(buf, sizeof(buf), d, gmtime(&now));
	qputs(buf, out);
	break;
    case 's':
	gettimeofday(&tv, NULL);
	fprintf(out, "%06i", (int)tv.tv_usec);
	break;
    case 'A':
	logitem(req, 'h', "X-Ash-Address");
	break;
    case 'H':
	logitem(req, 'h', "Host");
	break;
    case 'R':
	logitem(req, 'h', "Referer");
	break;
    case 'G':
	logitem(req, 'h', "User-Agent");
	break;
    }
}

static void logreq(struct hthead *req)
{
    char *p, *p2;
    char d[strlen(format)];
    char o;
    
    p = format;
    while(*p) {
	if(*p == '%') {
	    p++;
	    if(*p == '{') {
		p++;
		if((p2 = strchr(p, '}')) == NULL)
		    continue;
		memcpy(d, p, p2 - p);
		d[p2 - p] = 0;
		p = p2 + 1;
	    } else {
		d[0] = 0;
	    }
	    o = *p++;
	    if(o == 0)
		break;
	    logitem(req, o, d);
	} else {
	    fputc(*p++, out);
	}
    }
    fputc('\n', out);
    if(flush)
	fflush(out);
}

static void serve(struct hthead *req, int fd)
{
    now = time(NULL);
    if(sendreq(ch, req, fd)) {
	flog(LOG_ERR, "accesslog: could not pass request to child: %s", strerror(errno));
	exit(1);
    }
    logreq(req);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: accesslog [-hFa] [-f FORMAT] OUTFILE CHILD [ARGS...]\n");
}

int main(int argc, char **argv)
{
    int c, ret;
    struct hthead *req;
    int fd;
    struct pollfd pfd[2];
    
    optarg = NULL;
    while((c = getopt(argc, argv, "+hFaf:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'F':
	    flush = 0;
	    break;
	case 'f':
	    format = optarg;
	    break;
	case 'a':
	    format = "%A - - [%{%d/%b/%Y:%H:%M:%S %z}t] \"%m %u %v\" - - \"%R\" \"%G\"";
	    break;
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    if(argc - optind < 2) {
	usage(stderr);
	exit(1);
    }
    if(format == NULL)
	format = DEFFORMAT;
    if(!strcmp(argv[optind], "-")) {
	out = stdout;
    } else {
	if((out = fopen(argv[optind], "a")) == NULL) {
	    flog(LOG_ERR, "accesslog: could not open %s for logging: %s", argv[optind], strerror(errno));
	    exit(1);
	}
    }
    if((ch = stdmkchild(argv + optind + 1, NULL, NULL)) < 0) {
	flog(LOG_ERR, "accesslog: could fork child: %s", strerror(errno));
	exit(1);
    }
    while(1) {
	memset(pfd, 0, sizeof(pfd));
	pfd[0].fd = 0;
	pfd[0].events = POLLIN;
	pfd[1].fd = ch;
	pfd[1].events = POLLHUP;
	if((ret = poll(pfd, 2, -1)) < 0) {
	    if(errno != EINTR) {
		flog(LOG_ERR, "accesslog: error in poll: %s", strerror(errno));
		exit(1);
	    }
	}
	if(pfd[0].revents) {
	    if((fd = recvreq(0, &req)) < 0) {
		if(errno == 0)
		    break;
		flog(LOG_ERR, "accesslog: error in recvreq: %s", strerror(errno));
		exit(1);
	    }
	    serve(req, fd);
	    freehthead(req);
	    close(fd);
	}
	if(pfd[1].revents & POLLHUP)
	    break;
    }
    return(0);
}
