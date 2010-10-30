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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <time.h>

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif
#include <utils.h>

static int nchildren;
static pid_t *children;
static char **chspec;
static volatile int done, chdone;

static void runchild(void)
{
    execvp(chspec[0], chspec);
    exit(127);
}

static void manage(void)
{
    int i, st;
    sigset_t ss, ns;
    pid_t ch;
    
    sigemptyset(&ss);
    sigaddset(&ss, SIGCHLD);
    sigaddset(&ss, SIGTERM);
    sigaddset(&ss, SIGINT);
    sigprocmask(SIG_BLOCK, &ss, &ns);
    while(!done) {
	for(i = 0; i < nchildren; i++) {
	    if(children[i] == 0) {
		if((ch = fork()) < 0)
		    break;
		if(ch == 0)
		    runchild();
		children[i] = ch;
	    }
	}
	pselect(0, NULL, NULL, NULL, NULL, &ns);
	if(chdone) {
	    while((ch = waitpid(-1, &st, WNOHANG)) > 0) {
		for(i = 0; i < nchildren; i++) {
		    if(children[i] == ch)
			children[i] = 0;
		}
	    }
	    chdone = 0;
	}
    }
    sigprocmask(SIG_SETMASK, &ns, NULL);
}

static void killall(void)
{
    int i, try, left, st;
    sigset_t ss, ns;
    struct timespec to;
    pid_t ch;
    time_t b;
    
    signal(SIGINT, SIG_DFL);
    signal(SIGTERM, SIG_DFL);
    sigemptyset(&ss);
    sigaddset(&ss, SIGCHLD);
    sigprocmask(SIG_BLOCK, &ss, &ns);
    for(try = 0; try < 2; try++) {
	for(i = 0; i < nchildren; i++) {
	    if(children[i] != 0)
		kill(children[i], SIGTERM);
	}
	b = time(NULL);
	while(time(NULL) - b < 5) {
	    for(i = 0, left = 0; i < nchildren; i++) {
		if(children[i] != 0)
		    left++;
	    }
	    if(!left)
		return;
	    to.tv_sec = 1;
	    to.tv_nsec = 0;
	    pselect(0, NULL, NULL, NULL, &to, &ns);
	    if(chdone) {
		while((ch = waitpid(-1, &st, WNOHANG)) > 0) {
		    for(i = 0; i < nchildren; i++) {
			if(children[i] == ch)
			    children[i] = 0;
		    }
		}
		chdone = 0;
	    }
	}
    }
    for(i = 0; i < nchildren; i++) {
	if(children[i] != 0)
	    kill(children[i], SIGKILL);
    }
}

static void chld(int sig)
{
    chdone = 1;
}

static void term(int sig)
{
    done = 1;
}

static void usage(FILE *out)
{
    fprintf(out, "usage: multifscgi NUM PROGRAM [ARGS...]\n");
}

int main(int argc, char **argv)
{
    int c;
    
    while((c = getopt(argc, argv, "h")) >= 0) {
	switch(c) {
	case 'h':
	    usage(stdout);
	    return(0);
	default:
	    usage(stderr);
	    return(1);
	}
    }
    if(argc - optind < 2) {
	usage(stderr);
	return(1);
    }
    nchildren = atoi(argv[optind]);
    if(nchildren < 1) {
	usage(stderr);
	return(1);
    }
    children = szmalloc(sizeof(pid_t) * nchildren);
    chspec = argv + optind + 1;
    signal(SIGINT, term);
    signal(SIGTERM, term);
    signal(SIGCHLD, chld);
    manage();
    killall();
    return(0);
}
