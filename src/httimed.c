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
#include <errno.h>
#include <string.h>
#include <time.h>
#include <sys/poll.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>

static void usage(FILE *out)
{
    fprintf(out, "usage: httimed [-h] [-t TIMEOUT] CHILD [ARGS...]\n");
}

int main(int argc, char **argv)
{
    int c, t, ret;
    int ch, fd;
    struct hthead *req;
    struct pollfd pfd[2];
    time_t lreq, now;
    
    t = 300;
    while((c = getopt(argc, argv, "+ht:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 't':
	    t = atoi(optarg);
	    if(t < 1) {
		fprintf(stderr, "httimed: timeout must be positive\n");
		exit(1);
	    }
	    break;
	}
    }
    if(argc - optind < 1) {
	usage(stderr);
	exit(1);
    }
    if((ch = stdmkchild(argv + optind, NULL, NULL)) < 0) {
	flog(LOG_ERR, "httimed: could not fork child: %s", strerror(errno));
	exit(1);
    }
    lreq = time(NULL);
    while(1) {
	memset(pfd, 0, sizeof(pfd));
	pfd[0].fd = 0;
	pfd[0].events = POLLIN;
	pfd[1].fd = ch;
	pfd[1].events = POLLHUP;
	if((ret = poll(pfd, 2, (t + 1 - (time(NULL) - lreq)) * 1000)) < 0) {
	    if(errno != EINTR) {
		flog(LOG_ERR, "httimed: error in poll: %s", strerror(errno));
		exit(1);
	    }
	}
	now = time(NULL);
	if(pfd[0].revents) {
	    if((fd = recvreq(0, &req)) < 0) {
		if(errno == 0)
		    break;
		flog(LOG_ERR, "httimed: error in recvreq: %s", strerror(errno));
		exit(1);
	    }
	    if(sendreq(ch, req, fd)) {
		flog(LOG_ERR, "httimed: could not pass request to child: %s", strerror(errno));
		exit(1);
	    }
	    freehthead(req);
	    close(fd);
	}
	if(pfd[1].revents & POLLHUP)
	    break;
	if(now - lreq > t)
	    break;
	lreq = now;
    }
    return(0);
}
