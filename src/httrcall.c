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
typedbuf(pid_t) running;
static int limit = 0;
static volatile int exited;

static void checkexit(int block)
{
    int i, st, any;
    pid_t pid;
    
    exited = 0;
    any = 0;
    while((pid = waitpid(-1, &st, (block && !any) ? 0 : WNOHANG)) > 0) {
	any = 1;
	if(WCOREDUMP(st))
	    flog(LOG_WARNING, "child process %i dumped core", pid);
	for(i = 0; i < running.d; i++) {
	    if(running.b[i] == pid) {
		running.b[i] = running.b[--running.d];
		break;
	    }
	}
    }
}

static void serve(struct hthead *req, int fd)
{
    pid_t new;
    
    while((limit > 0) && (running.d >= limit))
	checkexit(1);
    if((new = stdforkserve(prog, req, fd, NULL, NULL)) < 0) {
	simpleerror(fd, 500, "Server Error", "The server appears to be overloaded.");
	return;
    }
    bufadd(running, new);
}

static void chldhandler(int sig)
{
    exited = 1;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: httrcall [-h] [-l LIMIT] PROGRAM [ARGS...]\n");
}

int main(int argc, char **argv)
{
    int c;
    struct hthead *req;
    int fd;
    
    while((c = getopt(argc, argv, "+hl:")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    exit(0);
	case 'l':
	    limit = atoi(optarg);
	    break;
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
    sigaction(SIGCHLD, &(struct sigaction) {
	    .sa_handler = chldhandler,
        }, NULL);
    while(1) {
	if(exited)
	    checkexit(0);
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
