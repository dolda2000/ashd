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
#include <signal.h>
#include <sys/wait.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>
#include <log.h>
#include <req.h>
#include <proc.h>
#include <resp.h>

static char **prog;

static void serve(struct hthead *req, int fd)
{
    if(stdforkserve(prog, req, fd, NULL, NULL) < 0)
	simpleerror(fd, 500, "Server Error", "The server appears to be overloaded.");
}

static void chldhandler(int sig)
{
    pid_t pid;
    int st;
    
    while((pid = waitpid(-1, &st, WNOHANG)) > 0) {
	if(WCOREDUMP(st))
	    flog(LOG_WARNING, "child process %i dumped core", pid);
    }
}

static void usage(FILE *out)
{
    fprintf(out, "usage: httrcall [-h] PROGRAM [ARGS...]\n");
}

int main(int argc, char **argv)
{
    int c;
    struct hthead *req;
    int fd;
    
    while((c = getopt(argc, argv, "+h")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    if(argc < optind - 1) {
	usage(stderr);
	exit(1);
    }
    prog = argv + optind;
    signal(SIGCHLD, chldhandler);
    while(1) {
	if((fd = recvreq(0, &req)) < 0) {
	    if(errno == EINTR)
		continue;
	    if(errno != 0)
		flog(LOG_ERR, "recvreq: %s", strerror(errno));
	    break;
	}
	serve(req, fd);
	freehthead(req);
	close(fd);
    }
    return(0);
}
