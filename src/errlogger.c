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
#include <syslog.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

static int prio;

static void logloop(int fd)
{
    FILE *in;
    char buf[1024];
    size_t len;
    
    in = fdopen(fd, "r");
    while(fgets(buf, sizeof(buf), in) != NULL) {
	len = strlen(buf);
	if(buf[len - 1] == '\n')
	    buf[len - 1] = 0;
	syslog(prio, "%s", buf);
    }
    fclose(in);
}

static void usage(FILE *out)
{
    fprintf(out, "usage: errlogger [-h] [-n NAME] [-f FACILITY] [-p PRIO] PROGRAM [ARGS...]\n");
}

int main(int argc, char **argv)
{
    int c;
    int pfd[2];
    pid_t ch;
    char *name;
    int fac;
    
    name = NULL;
    prio = LOG_WARNING;
    fac = LOG_DAEMON;
    while((c = getopt(argc, argv, "hn:p:f:")) >= 0) {
	switch(c) {
	case 'n':
	    name = optarg;
	    break;
	case 'f':
	    if(!strcmp(optarg, "auth")) {
		fac = LOG_AUTH;
	    } else if(!strcmp(optarg, "authpriv")) {
		fac = LOG_AUTHPRIV;
	    } else if(!strcmp(optarg, "cron")) {
		fac = LOG_CRON;
	    } else if(!strcmp(optarg, "daemon")) {
		fac = LOG_DAEMON;
	    } else if(!strcmp(optarg, "ftp")) {
		fac = LOG_FTP;
	    } else if(!strcmp(optarg, "kern")) {
		fac = LOG_KERN;
	    } else if(!strcmp(optarg, "lpr")) {
		fac = LOG_LPR;
	    } else if(!strcmp(optarg, "mail")) {
		fac = LOG_MAIL;
	    } else if(!strcmp(optarg, "news")) {
		fac = LOG_NEWS;
	    } else if(!strcmp(optarg, "user")) {
		fac = LOG_USER;
	    } else if(!strcmp(optarg, "uucp")) {
		fac = LOG_UUCP;
	    } else if(!strncmp(optarg, "local", 5) && (optarg[5] >= '0') && (optarg[5] <= '7') && !optarg[6]) {
		fac = LOG_LOCAL0 + (optarg[5] - '0');
	    } else {
		fprintf(stderr, "errlogger: unknown facility %s\n", optarg);
		exit(1);
	    }
	    break;
	case 'p':
	    if(!strcmp(optarg, "emerg")) {
		fac = LOG_EMERG;
	    } else if(!strcmp(optarg, "alert")) {
		fac = LOG_ALERT;
	    } else if(!strcmp(optarg, "crit")) {
		fac = LOG_CRIT;
	    } else if(!strcmp(optarg, "err")) {
		fac = LOG_ERR;
	    } else if(!strcmp(optarg, "warning")) {
		fac = LOG_WARNING;
	    } else if(!strcmp(optarg, "notice")) {
		fac = LOG_NOTICE;
	    } else if(!strcmp(optarg, "info")) {
		fac = LOG_INFO;
	    } else if(!strcmp(optarg, "debug")) {
		fac = LOG_DEBUG;
	    } else {
		fprintf(stderr, "errlogger: unknown priorty %s\n", optarg);
		exit(1);
	    }
	    break;
	case 'h':
	    usage(stdout);
	    exit(0);
	default:
	    usage(stderr);
	    exit(1);
	}
    }
    if(argc - optind < 1) {
	usage(stderr);
	exit(1);
    }
    if(name == NULL)
	name = argv[optind];
    openlog(name, 0, fac);
    pipe(pfd);
    if((ch = fork()) == 0) {
	close(pfd[0]);
	if(pfd[1] != 2) {
	    dup2(pfd[1], 2);
	    close(pfd[1]);
	}
	execvp(argv[optind], argv + optind);
	fprintf(stderr, "errlogger: %s: %s", argv[optind], strerror(errno));
	exit(127);
    }
    close(pfd[1]);
    signal(SIGCHLD, SIG_IGN);
    logloop(pfd[0]);
    return(0);
}
